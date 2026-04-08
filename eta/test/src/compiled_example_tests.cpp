/**
 * @file compiled_example_tests.cpp
 * @brief Integration tests that compile each .eta example to bytecode (.etac),
 *        then run the compiled bytecode on a fresh Driver, and verify the
 *        output matches the interpreted (direct) execution.
 *
 * This is the round-trip test for the bytecode serializer: it exercises the
 * full etac→etai pipeline programmatically.
 *
 * Paths are injected via CMake compile definitions:
 *   -DETA_EXAMPLES_DIR="..."
 *   -DETA_STDLIB_DIR="..."
 */

#include <boost/test/unit_test.hpp>

#include <filesystem>
#include <fstream>
#include <memory>
#include <regex>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

#include "eta/interpreter/driver.h"
#include "eta/interpreter/module_path.h"
#include "eta/runtime/port.h"
#include "eta/runtime/vm/bytecode_serializer.h"

namespace fs = std::filesystem;

// ── Path discovery (same as example_runner_tests.cpp) ───────────────────────

#ifndef ETA_EXAMPLES_DIR
#define ETA_EXAMPLES_DIR ""
#endif

#ifndef ETA_STDLIB_DIR
#define ETA_STDLIB_DIR ""
#endif

static fs::path examples_dir() {
    fs::path p(ETA_EXAMPLES_DIR);
    if (!p.empty() && fs::is_directory(p)) return p;
    auto cwd = fs::current_path();
    for (auto& candidate : {
        cwd / "examples",
        cwd / ".." / "examples",
        cwd / ".." / ".." / "examples",
        cwd / ".." / ".." / ".." / "examples",
        cwd / ".." / ".." / ".." / ".." / "examples",
    }) {
        if (fs::is_directory(candidate)) return fs::canonical(candidate);
    }
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
    auto dir = examples_dir();
    if (dir.empty() || !fs::is_directory(dir)) return files;

    for (auto& entry : fs::recursive_directory_iterator(dir)) {
        if (entry.is_regular_file() && entry.path().extension() == ".eta") {
            files.push_back(entry.path());
        }
    }
    std::sort(files.begin(), files.end());
    return files;
}

// ── Output normalization ────────────────────────────────────────────────────
// Logic variables print as _G<heap_id>.  Heap IDs differ between interpreted
// and compiled runs because the two Drivers allocate objects independently.
// Normalize by replacing each distinct _G<N> with _G0, _G1, … in order of
// first appearance so that structural equality is preserved.

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

// ── Test fixture ────────────────────────────────────────────────────────────

struct CompiledExampleFixture {
    fs::path stdlib;
    fs::path examples;

    CompiledExampleFixture()
        : stdlib(stdlib_dir())
        , examples(examples_dir())
    {}

    /**
     * Run a .eta file with interpreted execution, capturing output.
     * Returns {success, captured_output}.
     */
    std::pair<bool, std::string> run_interpreted(const fs::path& file) {
        if (stdlib.empty()) return {false, ""};

        eta::interpreter::ModulePathResolver resolver({stdlib});
        resolver.add_dir(file.parent_path());
        eta::interpreter::Driver driver(std::move(resolver), 8 * 1024 * 1024);

        auto out_port = std::make_shared<eta::runtime::StringPort>(
            eta::runtime::StringPort::Mode::Output);
        auto err_port = std::make_shared<eta::runtime::StringPort>(
            eta::runtime::StringPort::Mode::Output);
        driver.set_output_port(out_port);
        driver.set_error_port(err_port);

        auto prelude = driver.load_prelude();
        if (!prelude.loaded) return {false, ""};

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
    std::pair<bool, std::string> run_compiled(const fs::path& file) {
        if (stdlib.empty()) return {false, ""};

        // ── Step 1: Compile (compile-only — no execution) ───────────
        eta::interpreter::ModulePathResolver comp_resolver({stdlib});
        comp_resolver.add_dir(file.parent_path());
        eta::interpreter::Driver compiler(std::move(comp_resolver), 8 * 1024 * 1024);

        auto prelude = compiler.load_prelude();
        if (!prelude.loaded) return {false, ""};

        auto compile_result = compiler.compile_file(file);
        if (!compile_result) return {false, ""};

        auto& cr = *compile_result;

        // Build sub-registry and module entries from CompileResult
        eta::semantics::BytecodeFunctionRegistry file_registry;
        std::vector<eta::runtime::vm::ModuleEntry> module_entries;

        const auto& all_funcs = compiler.registry().all();
        for (uint32_t i = cr.base_func_idx; i < cr.end_func_idx; ++i) {
            file_registry.add(eta::runtime::vm::BytecodeFunction(all_funcs[i]));
        }

        for (const auto& cme : cr.modules) {
            eta::runtime::vm::ModuleEntry entry;
            entry.name = cme.name;
            entry.init_func_index = cme.init_func_index;
            entry.total_globals = cme.total_globals;
            entry.main_func_slot = cme.main_func_slot;
            module_entries.push_back(std::move(entry));
        }

        if (module_entries.empty()) return {false, ""};

        // Compute source hash
        std::ifstream src_in(file, std::ios::in | std::ios::binary);
        if (!src_in) return {false, ""};
        std::ostringstream src_buf;
        src_buf << src_in.rdbuf();
        uint64_t source_hash = eta::runtime::vm::BytecodeSerializer::hash_source(src_buf.str());

        // Serialize to a temp file
        auto temp_etac = fs::temp_directory_path() / "eta_roundtrip_test.etac";
        {
            eta::runtime::vm::BytecodeSerializer serializer(
                compiler.heap(), compiler.intern_table());
            std::ofstream out(temp_etac, std::ios::out | std::ios::binary);
            if (!out) return {false, ""};
            if (!serializer.serialize(module_entries, file_registry,
                                      source_hash, /*include_debug=*/true, out)) {
                return {false, ""};
            }
        }

        // ── Step 2: Load and run the .etac (replicating etai logic) ─
        eta::interpreter::ModulePathResolver run_resolver({stdlib});
        run_resolver.add_dir(file.parent_path());
        eta::interpreter::Driver runner(std::move(run_resolver), 8 * 1024 * 1024);

        auto out_port = std::make_shared<eta::runtime::StringPort>(
            eta::runtime::StringPort::Mode::Output);
        auto err_port = std::make_shared<eta::runtime::StringPort>(
            eta::runtime::StringPort::Mode::Output);
        runner.set_output_port(out_port);
        runner.set_error_port(err_port);

        auto run_prelude = runner.load_prelude();
        if (!run_prelude.loaded) {
            fs::remove(temp_etac);
            return {false, ""};
        }

        bool ok = runner.run_etac_file(temp_etac);
        fs::remove(temp_etac);

        if (!ok) {
            // Collect diagnostic messages for debugging
            std::string diag_msg;
            for (const auto& diag : runner.diagnostics().diagnostics()) {
                diag_msg += diag.message + "\n";
            }
            BOOST_TEST_MESSAGE("  etac diagnostics: " << diag_msg);
            return {false, out_port->get_string()};
        }
        return {ok, out_port->get_string()};
    }
};

// ── Test suite ──────────────────────────────────────────────────────────────

BOOST_FIXTURE_TEST_SUITE(compiled_example_tests, CompiledExampleFixture)

BOOST_AUTO_TEST_CASE(compiled_examples_match_interpreted_output) {
    auto files = collect_examples();
    if (files.empty()) {
        BOOST_TEST_MESSAGE("No example files found — skipping. "
                           "Set ETA_EXAMPLES_DIR and ETA_STDLIB_DIR compile definitions.");
        return;
    }

    BOOST_TEST_MESSAGE("Found " << files.size() << " example files for compiled round-trip test");
    BOOST_TEST_MESSAGE("stdlib: " << stdlib.string());
    BOOST_TEST_MESSAGE("examples: " << examples.string());

    int passed = 0, failed = 0;
    std::vector<std::string> failures;

    for (const auto& file : files) {
        auto rel = fs::relative(file, examples);
        BOOST_TEST_CONTEXT("Compiled round-trip: " << rel.string()) {
            // Run interpreted
            auto [interp_ok, interp_output] = run_interpreted(file);
            if (!interp_ok) {
                BOOST_TEST_MESSAGE("  ⚠ interpreted run failed — skipping: " << rel.string());
                continue;
            }

            // Compile and run
            auto [comp_ok, comp_output] = run_compiled(file);

            if (!comp_ok) {
                ++failed;
                failures.push_back(rel.string() + " (compiled run failed)");
                BOOST_TEST_MESSAGE("  ✗ " << rel.string() << " — compiled run failed");
                BOOST_CHECK_MESSAGE(false, "Compiled run failed for " << rel.string());
                continue;
            }

            // Compare output (normalize logic variable names which have
            // non-deterministic heap IDs)
            auto norm_interp = normalize_logic_vars(interp_output);
            auto norm_comp   = normalize_logic_vars(comp_output);
            bool output_matches = (norm_interp == norm_comp);
            if (output_matches) {
                ++passed;
                BOOST_TEST_MESSAGE("  ✓ " << rel.string());
            } else {
                ++failed;
                failures.push_back(rel.string() + " (output mismatch)");
                BOOST_TEST_MESSAGE("  ✗ " << rel.string() << " — output mismatch");

                // Show first difference for debugging
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
    BOOST_TEST_MESSAGE("Compiled round-trip results: "
                       << passed << " passed, " << failed << " failed out of "
                       << files.size());
    if (!failures.empty()) {
        BOOST_TEST_MESSAGE("Failures:");
        for (const auto& f : failures) {
            BOOST_TEST_MESSAGE("  - " << f);
        }
    }
}

BOOST_AUTO_TEST_SUITE_END()


