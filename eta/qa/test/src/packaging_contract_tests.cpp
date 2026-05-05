/**
 * @file packaging_contract_tests.cpp
 * @brief Baseline contract tests for module path and .etac execution.
 */

#include <boost/test/unit_test.hpp>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include "eta/interpreter/module_path.h"
#include "eta/runtime/nanbox.h"
#include "eta/runtime/vm/bytecode_serializer.h"
#include "eta/session/driver.h"

namespace fs = std::filesystem;

#ifndef ETA_STDLIB_DIR
#define ETA_STDLIB_DIR ""
#endif

#ifndef ETA_STDLIB_ETAC_DIR
#define ETA_STDLIB_ETAC_DIR ""
#endif

namespace {

/// Temporary directory guard for integration-style file-based tests.
struct TempDir {
    fs::path path;

    TempDir() {
        auto base = fs::temp_directory_path()
                  / ("eta_packaging_contract_" + std::to_string(
                         std::chrono::steady_clock::now().time_since_epoch().count()));
        fs::create_directories(base);
        path = std::move(base);
    }

    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }

    [[nodiscard]] fs::path create_file(const std::string& rel,
                                       const std::string& content = "") const {
        auto full = path / rel;
        fs::create_directories(full.parent_path());
        std::ofstream out(full, std::ios::out | std::ios::binary);
        out << content;
        return full;
    }
};

[[nodiscard]] std::string diagnostics_to_string(const eta::session::Driver& driver) {
    std::ostringstream oss;
    driver.diagnostics().print_all(oss, /*use_color=*/false, driver.file_resolver());
    return oss.str();
}

[[nodiscard]] int64_t decode_fixnum(eta::runtime::nanbox::LispVal value) {
    auto decoded = eta::runtime::nanbox::ops::decode<int64_t>(value);
    if (!decoded.has_value()) {
        BOOST_FAIL("expected fixnum result");
        return 0;
    }
    return *decoded;
}

[[nodiscard]] std::optional<fs::path> configured_stdlib_root() {
    fs::path root(ETA_STDLIB_DIR);
    if (root.empty()) return std::nullopt;
    std::error_code ec;
    if (!fs::is_directory(root, ec) || ec) return std::nullopt;
    auto canonical = fs::weakly_canonical(root, ec);
    if (!ec) return canonical;
    return root.lexically_normal();
}

[[nodiscard]] std::optional<fs::path> configured_stdlib_etac_root() {
    fs::path root(ETA_STDLIB_ETAC_DIR);
    if (root.empty()) return std::nullopt;
    std::error_code ec;
    if (!fs::is_directory(root, ec) || ec) return std::nullopt;
    auto canonical = fs::weakly_canonical(root, ec);
    if (!ec) return canonical;
    return root.lexically_normal();
}

[[nodiscard]] bool compile_to_etac(const fs::path& source_file,
                                   const fs::path& module_root,
                                   const fs::path& out_etac,
                                   std::string* error_message) {
    eta::interpreter::ModulePathResolver resolver({module_root});
    eta::session::Driver compiler(std::move(resolver), 8 * 1024 * 1024);

    auto compile_result = compiler.compile_file(source_file);
    if (!compile_result) {
        if (error_message) *error_message = diagnostics_to_string(compiler);
        return false;
    }
    auto& cr = *compile_result;

    eta::semantics::BytecodeFunctionRegistry file_registry;
    const auto& all_funcs = compiler.registry().all();
    for (uint32_t i = cr.base_func_idx; i < cr.end_func_idx; ++i) {
        auto func = eta::runtime::vm::BytecodeFunction(all_funcs[i]);
        func.rebase_func_indices(-static_cast<int32_t>(cr.base_func_idx));
        file_registry.add(std::move(func));
    }

    std::vector<eta::runtime::vm::ModuleEntry> module_entries;
    module_entries.reserve(cr.modules.size());
    for (const auto& module : cr.modules) {
        eta::runtime::vm::ModuleEntry entry;
        entry.name = module.name;
        entry.init_func_index = module.init_func_index;
        entry.total_globals = module.total_globals;
        entry.main_func_slot = module.main_func_slot;
        entry.first_func_index = module.first_func_index;
        entry.func_count = module.func_count;
        entry.owned_global_slots = module.owned_global_slots;
        entry.import_bindings.reserve(module.import_bindings.size());
        for (const auto& imp : module.import_bindings) {
            eta::runtime::vm::ModuleEntry::ImportBinding out_imp;
            out_imp.local_slot = imp.local_slot;
            out_imp.from_module = imp.from_module;
            out_imp.remote_name = imp.remote_name;
            entry.import_bindings.push_back(std::move(out_imp));
        }
        entry.export_bindings.reserve(module.export_bindings.size());
        for (const auto& ex : module.export_bindings) {
            eta::runtime::vm::ModuleEntry::ExportBinding out_ex;
            out_ex.name = ex.name;
            out_ex.slot = ex.slot;
            entry.export_bindings.push_back(std::move(out_ex));
        }
        module_entries.push_back(std::move(entry));
    }
    if (module_entries.empty()) {
        if (error_message) *error_message = "compile_file produced no modules";
        return false;
    }

    std::ifstream source_in(source_file, std::ios::in | std::ios::binary);
    if (!source_in) {
        if (error_message) *error_message = "failed to read source for source hash";
        return false;
    }
    std::ostringstream source_buf;
    source_buf << source_in.rdbuf();
    const uint64_t source_hash =
        eta::runtime::vm::BytecodeSerializer::hash_source(source_buf.str());

    eta::runtime::vm::BytecodeSerializer serializer(compiler.heap(), compiler.intern_table());
    std::ofstream out(out_etac, std::ios::out | std::ios::binary);
    if (!out) {
        if (error_message) *error_message = "failed to open output .etac path";
        return false;
    }

    const auto num_builtins = static_cast<uint32_t>(compiler.builtin_count());
    if (!serializer.serialize(module_entries, file_registry,
                              source_hash, /*include_debug=*/true, out,
                              cr.imports, num_builtins)) {
        if (error_message) *error_message = "bytecode serializer failed";
        return false;
    }
    return true;
}

} // namespace

BOOST_AUTO_TEST_SUITE(packaging_contract_tests)

BOOST_AUTO_TEST_CASE(source_run_with_explicit_std_import_succeeds) {
    auto stdlib_root = configured_stdlib_root();
    BOOST_REQUIRE_MESSAGE(stdlib_root.has_value(),
                          "ETA_STDLIB_DIR is not set to a valid stdlib root");

    eta::interpreter::ModulePathResolver resolver({*stdlib_root});
    eta::session::Driver driver(std::move(resolver), 8 * 1024 * 1024);

    eta::runtime::nanbox::LispVal value{eta::runtime::nanbox::Nil};
    const bool run_ok = driver.run_source(R"eta(
(module explicit.import.contract
  (import std.core)
  (define result (identity 42)))
)eta", &value, "result");
    BOOST_REQUIRE_MESSAGE(run_ok, diagnostics_to_string(driver));
    BOOST_TEST(decode_fixnum(value) == 42);
}

BOOST_AUTO_TEST_CASE(run_etac_file_auto_loads_imports_and_executes_modules) {
    TempDir temp;
    (void)temp.create_file("etac/contract/dep.eta", R"eta(
(module etac.contract.dep
  (export dep-value)
  (begin
    (define dep-value 41)))
)eta");
    const auto main_source = temp.create_file("etac/contract/main.eta", R"eta(
(module etac.contract.main
  (export answer)
  (import etac.contract.dep)
  (begin
    (define answer (+ dep-value 1))))
)eta");

    const auto etac_file = temp.path / "main.etac";
    std::string compile_error;
    BOOST_REQUIRE_MESSAGE(
        compile_to_etac(main_source, temp.path, etac_file, &compile_error),
        "compile_to_etac failed: " + compile_error);

    eta::interpreter::ModulePathResolver run_resolver({temp.path});
    eta::session::Driver runner(std::move(run_resolver), 8 * 1024 * 1024);

    const bool ok = runner.run_etac_file(etac_file);
    BOOST_REQUIRE_MESSAGE(ok, diagnostics_to_string(runner));
    BOOST_TEST(runner.has_module("etac.contract.dep"));
    BOOST_TEST(runner.has_module("etac.contract.main"));
}

BOOST_AUTO_TEST_CASE(source_run_hydrates_sibling_source_for_etac_imports) {
    TempDir temp;
    const auto dep_source = temp.create_file("hydration/dep.eta", R"eta(
(module hydration.dep
  (export dep-value)
  (begin
    (define dep-value 41)))
)eta");
    const auto main_source = temp.create_file("hydration/main.eta", R"eta(
(module hydration.main
  (import hydration.dep)
  (export answer)
  (begin
    (define answer (+ dep-value 1))))
)eta");

    const auto dep_etac = temp.path / "hydration" / "dep.etac";
    std::string compile_error;
    BOOST_REQUIRE_MESSAGE(
        compile_to_etac(dep_source, temp.path, dep_etac, &compile_error),
        "compile_to_etac failed: " + compile_error);

    eta::interpreter::ModulePathResolver run_resolver({temp.path});
    eta::session::Driver runner(std::move(run_resolver), 8 * 1024 * 1024);

    const bool run_ok = runner.run_file(main_source);
    BOOST_REQUIRE_MESSAGE(run_ok, diagnostics_to_string(runner));
    BOOST_TEST(runner.has_module("hydration.dep"));
    BOOST_TEST(runner.has_module("hydration.main"));

    eta::runtime::nanbox::LispVal value{eta::runtime::nanbox::Nil};
    const bool verify_ok = runner.run_source(R"eta(
(module hydration.verify
  (import hydration.main)
  (define result answer))
)eta", &value, "result");
    BOOST_REQUIRE_MESSAGE(verify_ok, diagnostics_to_string(runner));
    BOOST_TEST(decode_fixnum(value) == 42);
}

BOOST_AUTO_TEST_CASE(run_etac_file_emits_clear_diagnostic_for_missing_import) {
    TempDir temp;
    const auto dep_source = temp.create_file("etac/contract/dep.eta", R"eta(
(module etac.contract.dep
  (export dep-value)
  (begin
    (define dep-value 41)))
)eta");
    const auto main_source = temp.create_file("etac/contract/main.eta", R"eta(
(module etac.contract.main
  (export answer)
  (import etac.contract.dep)
  (begin
    (define answer (+ dep-value 1))))
)eta");

    const auto etac_file = temp.path / "main.etac";
    std::string compile_error;
    BOOST_REQUIRE_MESSAGE(
        compile_to_etac(main_source, temp.path, etac_file, &compile_error),
        "compile_to_etac failed: " + compile_error);

    std::error_code ec;
    fs::remove(dep_source, ec);
    BOOST_REQUIRE_MESSAGE(!ec, "failed to remove dep source before run_etac_file");

    eta::interpreter::ModulePathResolver run_resolver({temp.path});
    eta::session::Driver runner(std::move(run_resolver), 8 * 1024 * 1024);

    const bool ok = runner.run_etac_file(etac_file);
    BOOST_TEST(!ok);
    const auto diagnostics = diagnostics_to_string(runner);
    BOOST_TEST(diagnostics.find("cannot resolve import 'etac.contract.dep' required by .etac")
               != std::string::npos);
}

BOOST_AUTO_TEST_CASE(run_etac_file_stale_source_hash_falls_back_to_source) {
    TempDir temp;
    const auto main_source = temp.create_file("stale/main.eta", R"eta(
(module stale.main
  (export answer)
  (begin
    (define answer 42)))
)eta");

    const auto etac_file = temp.path / "stale/main.etac";
    std::string compile_error;
    BOOST_REQUIRE_MESSAGE(
        compile_to_etac(main_source, temp.path, etac_file, &compile_error),
        "compile_to_etac failed: " + compile_error);

    (void)temp.create_file("stale/main.eta", R"eta(
(module stale.main
  (export answer)
  (begin
    (define answer 99)))
)eta");

    eta::interpreter::ModulePathResolver run_resolver({temp.path});
    eta::session::Driver runner(std::move(run_resolver), 8 * 1024 * 1024);

    const bool ok = runner.run_etac_file(etac_file);
    BOOST_REQUIRE_MESSAGE(ok, diagnostics_to_string(runner));
    const auto stale_diagnostics = diagnostics_to_string(runner);
    BOOST_TEST(stale_diagnostics.find("stale .etac detected") != std::string::npos);

    eta::runtime::nanbox::LispVal value{eta::runtime::nanbox::Nil};
    const bool read_ok = runner.run_source(R"eta(
(module stale.verify
  (import stale.main)
  (define result answer))
)eta", &value, "result");
    BOOST_REQUIRE_MESSAGE(read_ok, diagnostics_to_string(runner));
    BOOST_TEST(decode_fixnum(value) == 99);
}

BOOST_AUTO_TEST_CASE(source_run_with_stdlib_etac_root_executes_basic_module) {
    auto stdlib_root = configured_stdlib_etac_root();
    BOOST_REQUIRE_MESSAGE(stdlib_root.has_value(),
                          "ETA_STDLIB_ETAC_DIR is not set to a valid stdlib artifact root");

    eta::interpreter::ModulePathResolver resolver({*stdlib_root});
    eta::session::Driver driver(std::move(resolver), 16 * 1024 * 1024);

    eta::runtime::nanbox::LispVal value{eta::runtime::nanbox::Nil};
    const bool ok = driver.run_source(R"eta(
(module etac.root.source.contract
  (begin
    (define result (+ 40 2))))
)eta", &value, "result");
    BOOST_REQUIRE_MESSAGE(ok, diagnostics_to_string(driver));
    BOOST_TEST(decode_fixnum(value) == 42);
}

BOOST_AUTO_TEST_CASE(source_run_with_stdlib_etac_root_keeps_unrelated_imports_slot_stable) {
    auto stdlib_root = configured_stdlib_etac_root();
    BOOST_REQUIRE_MESSAGE(stdlib_root.has_value(),
                          "ETA_STDLIB_ETAC_DIR is not set to a valid stdlib artifact root");

    eta::interpreter::ModulePathResolver resolver({*stdlib_root});
    eta::session::Driver driver(std::move(resolver), 16 * 1024 * 1024);

    eta::runtime::nanbox::LispVal value{eta::runtime::nanbox::Nil};
    const bool ok = driver.run_source(R"eta(
(module etac.root.slot.contract
  (import std.test std.aad)
  (begin
    (define sample-test
      (make-test "no-op" (lambda () (assert-true #t))))
    (define result (if (> (ad-max 2.0 7.0) 6.0) 1 0))))
)eta", &value, "result");
    BOOST_REQUIRE_MESSAGE(ok, diagnostics_to_string(driver));
    BOOST_TEST(decode_fixnum(value) == 1);
}

BOOST_AUTO_TEST_CASE(stdlib_etac_artifacts_load_without_stale_fallback) {
    auto stdlib_root = configured_stdlib_etac_root();
    BOOST_REQUIRE_MESSAGE(stdlib_root.has_value(),
                          "ETA_STDLIB_ETAC_DIR is not set to a valid stdlib artifact root");

    std::vector<fs::path> artifacts;
    for (const auto& entry : fs::recursive_directory_iterator(*stdlib_root)) {
        if (!entry.is_regular_file()) continue;
        if (entry.path().extension() != ".etac") continue;
        artifacts.push_back(entry.path());
    }
    std::sort(artifacts.begin(), artifacts.end());
    BOOST_REQUIRE_MESSAGE(!artifacts.empty(), "expected prebuilt stdlib .etac artifacts");

    eta::interpreter::ModulePathResolver resolver({*stdlib_root});
    eta::session::Driver driver(std::move(resolver), 16 * 1024 * 1024);

    for (const auto& artifact : artifacts) {
        driver.diagnostics().clear();
        const bool ok = driver.run_etac_file(artifact);
        BOOST_REQUIRE_MESSAGE(ok,
                              "failed to load stdlib artifact: "
                                  + artifact.string() + "\n" + diagnostics_to_string(driver));
        const auto diagnostics = diagnostics_to_string(driver);
        BOOST_TEST(diagnostics.find("stale .etac detected") == std::string::npos);
    }
}

BOOST_AUTO_TEST_SUITE_END()
