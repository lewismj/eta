/**
 * @file compiled_example_tests.cpp
 * @brief Integration tests that compile each .eta cookbook program to bytecode (.etac),
 *        then run the compiled bytecode on a fresh Driver, and verify the
 *        output matches the interpreted (direct) execution.
 *
 * This is the round-trip test for the bytecode serializer: it exercises the
 *
 * Paths are injected via CMake compile definitions:
 *   -DETA_COOKBOOK_DIR="..."
 *   -DETA_STDLIB_DIR="..."
 */

#include <boost/test/unit_test.hpp>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#ifdef _WIN32
#include <process.h>
#endif
#include <regex>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "eta/session/driver.h"
#include "eta/interpreter/module_path.h"
#include "eta/runtime/port.h"
#include "eta/runtime/vm/bytecode_serializer.h"
#include "eta/semantics/passes/constant_folding.h"
#include "eta/semantics/passes/dead_code_elimination.h"
#include "eta/semantics/passes/primitive_specialisation.h"

namespace fs = std::filesystem;

/// Path discovery (same as example_runner_tests.cpp)

#ifndef ETA_COOKBOOK_DIR
#define ETA_COOKBOOK_DIR ""
#endif

#ifndef ETA_STDLIB_DIR
#define ETA_STDLIB_DIR ""
#endif

static std::string etac_binary_path() {
#ifdef ETA_ETAC_PATH
    return ETA_ETAC_PATH;
#else
    return {};
#endif
}

struct TempFileGuard {
    fs::path path;

    explicit TempFileGuard(const fs::path& p)
        : path(p)
    {}

    ~TempFileGuard() {
        std::error_code ec;
        fs::remove(path, ec);
    }

    TempFileGuard(const TempFileGuard&) = delete;
    TempFileGuard& operator=(const TempFileGuard&) = delete;
};

static fs::path unique_temp_file_path(const std::string& prefix,
                                      const std::string& extension) {
    static std::atomic<uint64_t> sequence{0};
    const auto now_ns =
        std::chrono::high_resolution_clock::now().time_since_epoch().count();
    const auto tid_hash =
        static_cast<uint64_t>(std::hash<std::thread::id>{}(std::this_thread::get_id()));

    for (int attempt = 0; attempt < 32; ++attempt) {
        const auto seq =
            sequence.fetch_add(1, std::memory_order_relaxed);
        const auto candidate = fs::temp_directory_path()
            / (prefix + "_" + std::to_string(now_ns)
                + "_" + std::to_string(tid_hash)
                + "_" + std::to_string(seq)
                + extension);
        std::error_code ec;
        if (!fs::exists(candidate, ec)) return candidate;
    }

    const auto fallback_seq =
        sequence.fetch_add(1, std::memory_order_relaxed);
    return fs::temp_directory_path()
        / (prefix + "_fallback_" + std::to_string(fallback_seq) + extension);
}

[[maybe_unused]] static std::string shell_quote(const std::string& s) {
#ifdef _WIN32
    std::string out = "\"";
    for (char c : s) {
        if (c == '"') out += "\\\"";
        else out += c;
    }
    out += "\"";
    return out;
#else
    std::string out = "'";
    for (char c : s) {
        if (c == '\'') out += "'\\''";
        else out += c;
    }
    out += "'";
    return out;
#endif
}

static fs::path cookbook_dir() {
    fs::path p(ETA_COOKBOOK_DIR);
    if (!p.empty() && fs::is_directory(p)) return fs::canonical(p);
    return {};
}

static fs::path stdlib_dir() {
    fs::path p(ETA_STDLIB_DIR);
    if (!p.empty() && fs::is_directory(p)) return p;
    auto cwd = fs::current_path();
    for (auto& candidate : {
        cwd / "stdlib",
        cwd / ".." / "stdlib",
        cwd / ".." / ".." / "stdlib",
        cwd / ".." / ".." / ".." / "stdlib",
        cwd / ".." / ".." / ".." / ".." / "stdlib",
    }) {
        if (fs::is_directory(candidate)) return fs::canonical(candidate);
    }
    return {};
}

static std::vector<fs::path> collect_examples() {
    std::vector<fs::path> files;
    auto dir = cookbook_dir();
    if (dir.empty() || !fs::is_directory(dir)) return files;

    for (auto& entry : fs::recursive_directory_iterator(dir)) {
        if (entry.is_regular_file() && entry.path().extension() == ".eta") {
            files.push_back(entry.path());
        }
    }
    std::sort(files.begin(), files.end());
    return files;
}

/**
 * Output normalization
 * Logic variables print as _G<heap_id>.  Heap IDs differ between interpreted
 * and compiled runs because the two Drivers allocate objects independently.
 * first appearance so that structural equality is preserved.
 */

static std::string normalize_logic_vars(const std::string& s) {
    static const std::regex re(R"(_G\d+)");
    std::unordered_map<std::string, std::string> mapping;
    std::string result;
    result.reserve(s.size());
    auto it  = std::sregex_iterator(s.begin(), s.end(), re);
    auto end = std::sregex_iterator();
    std::size_t prev = 0;
    uint32_t next_id = 0;
    for (; it != end; ++it) {
        auto& m = *it;
        result.append(s, prev, static_cast<size_t>(m.position()) - prev);
        auto [mit, inserted] = mapping.try_emplace(m.str(),
            "_G" + std::to_string(next_id));
        if (inserted) ++next_id;
        result.append(mit->second);
        prev = static_cast<size_t>(m.position()) + m.length();
    }
    result.append(s, prev, s.size() - prev);
    return result;
}

/// Torch-dependent example filtering

static bool requires_torch([[maybe_unused]] const fs::path& file) {
    auto stem = file.stem().string();
    if (stem == "torch") return true;
    std::ifstream ifs(file);
    std::string line;
    while (std::getline(ifs, line)) {
        if (line.find("std.torch") != std::string::npos) return true;
    }
    return false;
}

/**
 * Networking example filtering
 * Three kinds of files need to be skipped:
 *      parent.  Running standalone returns Nil and the first recv! errors out.
 *      (import std.net).  Without a real etai binary the spawn blocks forever
 *      waiting for the child to connect.
 *      directly without importing std.net (e.g. distributed-compute.eta,
 *      echo-server.eta).  These need a live peer and cannot run standalone.
 *
 * Only non-comment lines are scanned so doc-comment mentions of these symbols
 * (e.g. message-passing.eta header) do not falsely trigger the filter.
 */
static bool requires_net(const fs::path& file) {
    std::ifstream ifs(file);
    std::string line;
    while (std::getline(ifs, line)) {
        auto pos = line.find_first_not_of(" \t");
        if (pos != std::string::npos && line[pos] == ';') continue;
        if (line.find("current-mailbox") != std::string::npos) return true;
        if (line.find("std.net")         != std::string::npos) return true;
        if (line.find("nng-socket")      != std::string::npos) return true;
        if (line.find("nng-dial")        != std::string::npos) return true;
        if (line.find("nng-listen")      != std::string::npos) return true;
    }
    return false;
}

/// Test fixture

struct CompiledExampleFixture {
    fs::path stdlib;
    fs::path cookbook;

    CompiledExampleFixture()
        : stdlib(stdlib_dir())
        , cookbook(cookbook_dir())
    {}

    /**
     * Run a .eta file with interpreted execution, capturing output.
     * Returns {success, captured_output}.
     */
    std::pair<bool, std::string> run_interpreted(const fs::path& file) {
        if (stdlib.empty()) return {false, ""};

        eta::interpreter::ModulePathResolver resolver({stdlib});
        resolver.add_dir(file.parent_path());
        eta::session::Driver driver(std::move(resolver), 8 * 1024 * 1024);

        auto out_port = std::make_shared<eta::runtime::StringPort>(
            eta::runtime::StringPort::Mode::Output);
        auto err_port = std::make_shared<eta::runtime::StringPort>(
            eta::runtime::StringPort::Mode::Output);
        driver.set_output_port(out_port);
        driver.set_error_port(err_port);

        bool ok = driver.run_file(file);
        return {ok, out_port->get_string()};
    }

    /**
     * Compile a .eta file to .etac bytecode, then load and execute the .etac
     * on a fresh Driver, capturing output.
     * Returns {success, captured_output}.
     *
     * Uses compile_file() to avoid executing code during compilation.
     */
    std::pair<bool, std::string> run_compiled(const fs::path& file, bool optimize) {
        if (stdlib.empty()) return {false, ""};

        eta::interpreter::ModulePathResolver comp_resolver({stdlib});
        comp_resolver.add_dir(file.parent_path());
        eta::session::Driver compiler(std::move(comp_resolver), 8 * 1024 * 1024);

        if (optimize) {
            auto& pipeline = compiler.optimization_pipeline();
            pipeline.add_pass(std::make_unique<eta::semantics::passes::ConstantFolding>());
            pipeline.add_pass(std::make_unique<eta::semantics::passes::PrimitiveSpecialisation>());
            pipeline.add_pass(std::make_unique<eta::semantics::passes::DeadCodeElimination>());
        }

        auto compile_result = compiler.compile_file(file);
        if (!compile_result) return {false, ""};

        auto& cr = *compile_result;

        /// Build sub-registry and module entries from CompileResult
        eta::semantics::BytecodeFunctionRegistry file_registry;
        std::vector<eta::runtime::vm::ModuleEntry> module_entries;

        const auto& all_funcs = compiler.registry().all();
        for (uint32_t i = cr.base_func_idx; i < cr.end_func_idx; ++i) {
            auto func = eta::runtime::vm::BytecodeFunction(all_funcs[i]);
            func.rebase_func_indices(-static_cast<int32_t>(cr.base_func_idx));
            file_registry.add(std::move(func));
        }

        for (const auto& cme : cr.modules) {
            eta::runtime::vm::ModuleEntry entry;
            entry.name = cme.name;
            entry.init_func_index = cme.init_func_index;
            entry.total_globals = cme.total_globals;
            entry.main_func_slot = cme.main_func_slot;
            entry.first_func_index = cme.first_func_index;
            entry.func_count = cme.func_count;
            entry.owned_global_slots = cme.owned_global_slots;
            entry.import_bindings.reserve(cme.import_bindings.size());
            for (const auto& imp : cme.import_bindings) {
                eta::runtime::vm::ModuleEntry::ImportBinding out_imp;
                out_imp.local_slot = imp.local_slot;
                out_imp.from_module = imp.from_module;
                out_imp.remote_name = imp.remote_name;
                entry.import_bindings.push_back(std::move(out_imp));
            }
            entry.export_bindings.reserve(cme.export_bindings.size());
            for (const auto& ex : cme.export_bindings) {
                eta::runtime::vm::ModuleEntry::ExportBinding out_ex;
                out_ex.name = ex.name;
                out_ex.slot = ex.slot;
                entry.export_bindings.push_back(std::move(out_ex));
            }
            module_entries.push_back(std::move(entry));
        }

        if (module_entries.empty()) return {false, ""};

        /// Compute source hash
        std::ifstream src_in(file, std::ios::in | std::ios::binary);
        if (!src_in) return {false, ""};
        std::ostringstream src_buf;
        src_buf << src_in.rdbuf();
        uint64_t source_hash = eta::runtime::vm::BytecodeSerializer::hash_source(src_buf.str());

        /// Serialize to a temp file
        auto temp_etac = unique_temp_file_path(
            optimize ? "eta_roundtrip_test_O" : "eta_roundtrip_test_O0",
            ".etac");
        [[maybe_unused]] TempFileGuard temp_etac_guard(temp_etac);
        {
            eta::runtime::vm::BytecodeSerializer serializer(
                compiler.heap(), compiler.intern_table());
            std::ofstream out(temp_etac, std::ios::out | std::ios::binary);
            if (!out) return {false, ""};
            auto num_builtins = static_cast<uint32_t>(compiler.builtin_count());
            if (!serializer.serialize(module_entries, file_registry,
                                      source_hash, /*include_debug=*/true, out,
                                      cr.imports, num_builtins)) {
                return {false, ""};
            }
        }

        /// Step 2: Load and run the .etac (replicating etai logic)
        eta::interpreter::ModulePathResolver run_resolver({stdlib});
        run_resolver.add_dir(file.parent_path());
        eta::session::Driver runner(std::move(run_resolver), 8 * 1024 * 1024);

        auto out_port = std::make_shared<eta::runtime::StringPort>(
            eta::runtime::StringPort::Mode::Output);
        auto err_port = std::make_shared<eta::runtime::StringPort>(
            eta::runtime::StringPort::Mode::Output);
        runner.set_output_port(out_port);
        runner.set_error_port(err_port);

        bool ok = runner.run_etac_file(temp_etac);

        if (!ok) {
            /// Collect diagnostic messages for debugging
            std::string diag_msg;
            for (const auto& diag : runner.diagnostics().diagnostics()) {
                diag_msg += diag.message + "\n";
            }
            BOOST_TEST_MESSAGE("  etac diagnostics: " << diag_msg);
            return {false, out_port->get_string()};
        }
        return {ok, out_port->get_string()};
    }

    [[nodiscard]] bool can_run_etac_cli() const {
        const auto etac = etac_binary_path();
        if (etac.empty()) return false;
        if (etac.find("$<") != std::string::npos) return false;

        std::error_code ec;
        const fs::path p(etac);
        if (fs::exists(p, ec) && fs::is_regular_file(p, ec)) return true;
        if (p.is_absolute()) return false;

        const fs::path resolved = fs::current_path(ec) / p;
        return fs::exists(resolved, ec) && fs::is_regular_file(resolved, ec);
    }

    /**
     * Compile via external etac CLI, then load and execute the emitted .etac.
     */
    std::pair<bool, std::string> run_compiled_via_etac_cli(const fs::path& file, bool optimize) {
        if (stdlib.empty()) return {false, ""};
        const auto etac = etac_binary_path();
        if (etac.empty()) return {false, "missing ETA_ETAC_PATH"};

        auto temp_etac = unique_temp_file_path(
            optimize ? "eta_roundtrip_cli_O" : "eta_roundtrip_cli_O0",
            ".etac");
        [[maybe_unused]] TempFileGuard temp_etac_guard(temp_etac);
#ifdef _WIN32
        std::vector<std::wstring> args;
        args.reserve(7);
        args.push_back(fs::path(etac).wstring());
        if (optimize) args.emplace_back(L"-O");
        args.emplace_back(L"--path");
        args.push_back(stdlib.wstring());
        args.push_back(file.wstring());
        args.emplace_back(L"-o");
        args.push_back(temp_etac.wstring());

        std::vector<const wchar_t*> argv;
        argv.reserve(args.size() + 1);
        for (const auto& arg : args) argv.push_back(arg.c_str());
        argv.push_back(nullptr);

        errno = 0;
        const int rc = _wspawnvp(_P_WAIT, args.front().c_str(), argv.data());
        if (rc != 0) {
            if (rc == -1) {
                return {false,
                        "etac CLI failed (spawn error: "
                            + std::to_string(errno) + ")"};
            }
            return {false, "etac CLI failed (rc=" + std::to_string(rc) + ")"};
        }
#else
        auto temp_log = unique_temp_file_path(
            optimize ? "eta_roundtrip_cli_O" : "eta_roundtrip_cli_O0",
            ".log");
        [[maybe_unused]] TempFileGuard temp_log_guard(temp_log);

        const std::string mode_flag = optimize ? "-O " : "";
        const std::string cmd =
            shell_quote(etac) + " " + mode_flag
            + "--path " + shell_quote(stdlib.string()) + " "
            + shell_quote(file.string()) + " -o " + shell_quote(temp_etac.string())
            + " > " + shell_quote(temp_log.string()) + " 2>&1";

        const int rc = std::system(cmd.c_str());
        if (rc != 0) {
            std::string log_text;
            std::ifstream in(temp_log);
            if (in) {
                std::ostringstream buf;
                buf << in.rdbuf();
                log_text = buf.str();
            }
            return {false, "etac CLI failed (rc=" + std::to_string(rc) + ")\n" + log_text};
        }
#endif

        eta::interpreter::ModulePathResolver run_resolver({stdlib});
        run_resolver.add_dir(file.parent_path());
        eta::session::Driver runner(std::move(run_resolver), 8 * 1024 * 1024);

        auto out_port = std::make_shared<eta::runtime::StringPort>(
            eta::runtime::StringPort::Mode::Output);
        auto err_port = std::make_shared<eta::runtime::StringPort>(
            eta::runtime::StringPort::Mode::Output);
        runner.set_output_port(out_port);
        runner.set_error_port(err_port);

        const bool ok = runner.run_etac_file(temp_etac);

        if (!ok) {
            std::string diag_msg;
            for (const auto& diag : runner.diagnostics().diagnostics()) {
                diag_msg += diag.message + "\n";
            }
            return {false, diag_msg.empty() ? out_port->get_string() : diag_msg};
        }
        return {ok, out_port->get_string()};
    }

    void check_compiled_examples_match_interpreted_output(bool optimize, bool via_etac_cli) {
        auto files = collect_examples();
        if (files.empty()) {
            BOOST_TEST_MESSAGE("No example files found  -  skipping. "
                               "Set ETA_COOKBOOK_DIR and ETA_STDLIB_DIR compile definitions.");
            return;
        }

        BOOST_TEST_MESSAGE("Found " << files.size() << " example files for compiled round-trip test");
        BOOST_TEST_MESSAGE("stdlib: " << stdlib.string());
        BOOST_TEST_MESSAGE("cookbook: " << cookbook.string());
        BOOST_TEST_MESSAGE("mode: " << (optimize ? "-O" : "-O0/default"));
        BOOST_TEST_MESSAGE("backend: " << (via_etac_cli ? "etac CLI" : "in-process compile"));

        int passed = 0, failed = 0;
        std::vector<std::string> failures;

        for (const auto& file : files) {
            auto rel = fs::relative(file, cookbook);
#if !defined(ETA_HAS_TORCH) || defined(ETA_TORCH_DEBUG_SKIP)
            if (requires_torch(file)) {
                BOOST_TEST_MESSAGE("  [SKIP] " << rel.string() << " (requires torch  -  skipped)");
                continue;
            }
#endif
            if (requires_net(file)) {
                BOOST_TEST_MESSAGE("  [SKIP] " << rel.string() << " (requires networking runtime  -  skipped)");
                continue;
            }
            BOOST_TEST_CONTEXT("Compiled round-trip (" << (optimize ? "-O" : "-O0/default")
                                                       << ", "
                                                       << (via_etac_cli ? "etac CLI" : "in-process")
                                                       << "): " << rel.string()) {
                /// Run interpreted
                auto [interp_ok, interp_output] = run_interpreted(file);
                if (!interp_ok) {
                    BOOST_TEST_MESSAGE("  [WARN] interpreted run failed  -  skipping: " << rel.string());
                    continue;
                }

                /// Compile and run
                auto [comp_ok, comp_output] = via_etac_cli
                    ? run_compiled_via_etac_cli(file, optimize)
                    : run_compiled(file, optimize);

                if (!comp_ok) {
                    ++failed;
                    failures.push_back(rel.string() + " (compiled run failed)");
                    BOOST_TEST_MESSAGE("  [FAIL] " << rel.string() << "  -  compiled run failed");
                    BOOST_TEST_MESSAGE("  partial output (" << comp_output.size() << " bytes): ["
                        << comp_output.substr(0, 500) << "]");
                    BOOST_TEST_MESSAGE("  interp output (" << interp_output.size() << " bytes): ["
                        << interp_output.substr(0, 500) << "]");
                    BOOST_CHECK_MESSAGE(false, "Compiled run failed for " << rel.string());
                    continue;
                }

                /**
                 * Compare output (normalize logic variable names which have
                 * non-deterministic heap IDs)
                 */
                auto norm_interp = normalize_logic_vars(interp_output);
                auto norm_comp   = normalize_logic_vars(comp_output);

                /**
                 * Torch examples involve random weight initialization so
                 * training loss values differ between runs.  For those files
                 * we only verify the compiled run succeeds without crashing.
                 */
                bool skip_output_compare = requires_torch(file);
                bool output_matches = skip_output_compare || (norm_interp == norm_comp);
                if (output_matches) {
                    ++passed;
                    BOOST_TEST_MESSAGE("  [OK] " << rel.string());
                } else {
                    ++failed;
                    failures.push_back(rel.string() + " (output mismatch)");
                    BOOST_TEST_MESSAGE("  [FAIL] " << rel.string() << "  -  output mismatch");

                    /// Show first difference for debugging
                    size_t diff_pos = 0;
                    while (diff_pos < norm_interp.size() &&
                           diff_pos < norm_comp.size() &&
                           norm_interp[diff_pos] == norm_comp[diff_pos]) {
                        ++diff_pos;
                    }
                    auto context_start = diff_pos > 40 ? diff_pos - 40 : 0;
                    auto interp_snippet = norm_interp.substr(
                        context_start, std::min<size_t>(80, norm_interp.size() - context_start));
                    auto comp_snippet = norm_comp.substr(
                        context_start, std::min<size_t>(80, norm_comp.size() - context_start));
                    BOOST_TEST_MESSAGE("    first diff at byte " << diff_pos);
                    BOOST_TEST_MESSAGE("    interpreted: ..." << interp_snippet << "...");
                    BOOST_TEST_MESSAGE("    compiled:    ..." << comp_snippet << "...");
                }
                BOOST_CHECK_MESSAGE(output_matches,
                    "Output mismatch for compiled " << rel.string());
            }
        }

        BOOST_TEST_MESSAGE("");
        BOOST_TEST_MESSAGE("Compiled round-trip results (" << (optimize ? "-O" : "-O0/default")
                                                           << "): "
                           << passed << " passed, " << failed << " failed out of "
                           << files.size());
        if (!failures.empty()) {
            BOOST_TEST_MESSAGE("Failures:");
            for (const auto& f : failures) {
                BOOST_TEST_MESSAGE("  - " << f);
            }
        }
    }
};

/// Test suite

BOOST_FIXTURE_TEST_SUITE(compiled_example_tests, CompiledExampleFixture)

BOOST_AUTO_TEST_CASE(compiled_examples_match_interpreted_output) {
    check_compiled_examples_match_interpreted_output(false, false);
}

BOOST_AUTO_TEST_CASE(compiled_examples_match_interpreted_output_optimized) {
    check_compiled_examples_match_interpreted_output(true, false);
}

BOOST_AUTO_TEST_CASE(compiled_examples_match_interpreted_output_optimized_etac_cli) {
    if (!can_run_etac_cli()) {
        BOOST_TEST_MESSAGE("ETA_ETAC_PATH not set (or etac binary missing) - skipping etac CLI round-trip test.");
        return;
    }
    check_compiled_examples_match_interpreted_output(true, true);
}

BOOST_AUTO_TEST_SUITE_END()



