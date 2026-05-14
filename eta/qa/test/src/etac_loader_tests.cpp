/**
 * @file etac_loader_tests.cpp
 * @brief Unit tests for eta::session::EtacLoader.
 */

#include <boost/test/unit_test.hpp>

#include <chrono>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "eta/diagnostic/diagnostic.h"
#include "eta/reader/module_linker.h"
#include "eta/runtime/builtin_env.h"
#include "eta/runtime/error.h"
#include "eta/runtime/extension_env.h"
#include "eta/runtime/memory/heap.h"
#include "eta/runtime/memory/intern_table.h"
#include "eta/runtime/nanbox.h"
#include "eta/runtime/types/primitive.h"
#include "eta/runtime/vm/bytecode_serializer.h"
#include "eta/runtime/vm/vm.h"
#include "eta/semantics/optimization_pipeline.h"
#include "eta/session/compilation_session.h"
#include "eta/session/etac_loader.h"
#include "eta/session/runtime_primitives.h"

namespace fs = std::filesystem;

namespace {

struct TempDir {
    fs::path path;

    TempDir() {
        const auto stamp = std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count());
        path = fs::temp_directory_path() / ("eta_etac_loader_test_" + stamp);
        fs::create_directories(path);
    }

    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }

    fs::path write_file(const std::string& rel, const std::string& content) const {
        const auto full = path / rel;
        fs::create_directories(full.parent_path());
        std::ofstream out(full, std::ios::out | std::ios::binary | std::ios::trunc);
        out << content;
        return full;
    }
};

class EtacLoaderFixture final
    : public eta::session::CompilationSession::Host,
      public eta::session::EtacLoader::Host {
public:
    EtacLoaderFixture()
        : heap_(16ull * 1024ull * 1024ull),
          vm_(heap_, intern_table_),
          primitive_installer_(heap_, builtins_, extensions_),
          etac_loader_(*this, compilation_, *this) {
        vm_.set_function_resolver([this](uint32_t index) {
            return registry_.get(index);
        });
    }

    bool ensure_package_sidecars_loaded(std::optional<fs::path>) override {
        return true;
    }

    std::optional<fs::path> resolve_import_path(
        const std::string&,
        bool* shadow_conflict) override {
        if (shadow_conflict) *shadow_conflict = false;
        return std::nullopt;
    }

    bool run_module_file(const fs::path& path) override {
        if (path.extension() == ".etac") return etac_loader_.run_etac_file(path);
        return run_source_file(path);
    }

    bool run_source_file(const fs::path& path) override {
        std::ifstream in(path, std::ios::in | std::ios::binary);
        if (!in) {
            diagnostics_.emit_error(
                eta::diagnostic::DiagnosticCode::ModuleNotFound, {},
                "cannot open file: " + path.string());
            return false;
        }

        std::ostringstream buf;
        buf << in.rdbuf();
        return compilation_.run_source_impl(*this, buf.str(), /*file_id=*/0u);
    }

    void emit_link_error(const eta::reader::LinkError& error) override {
        diagnostics_.emit(eta::diagnostic::to_diagnostic(error));
    }

    void emit_runtime_error(const eta::runtime::error::RuntimeError& error) override {
        std::visit([this](auto&& e) {
            diagnostics_.emit(eta::diagnostic::to_diagnostic(e));
        }, error);
    }

    eta::diagnostic::DiagnosticEngine& diagnostics() noexcept override {
        return diagnostics_;
    }

    eta::runtime::vm::VM& vm() noexcept override {
        return vm_;
    }

    eta::runtime::memory::heap::Heap& heap() noexcept override {
        return heap_;
    }

    eta::runtime::memory::intern::InternTable& intern_table() noexcept override {
        return intern_table_;
    }

    eta::semantics::BytecodeFunctionRegistry& registry() noexcept override {
        return registry_;
    }

    eta::runtime::BuiltinEnvironment& builtins() noexcept override {
        return builtins_;
    }

    eta::runtime::ExtensionEnvironment& extensions() noexcept override {
        return extensions_;
    }

    eta::session::RuntimePrimitiveInstaller& primitive_installer() noexcept override {
        return primitive_installer_;
    }

    eta::semantics::OptimizationPipeline& optimization_pipeline() noexcept override {
        return optimization_pipeline_;
    }

    std::size_t total_primitive_count() const noexcept override {
        return primitive_installer_.total_primitive_count();
    }

    std::size_t builtin_count() const noexcept override {
        return primitive_installer_.builtin_count();
    }

    std::uint64_t extension_env_hash() const noexcept override {
        return extensions_.fingerprint();
    }

    bool run_etac_file(const fs::path& path) {
        return etac_loader_.run_etac_file(path);
    }

    void add_dummy_builtin() {
        builtins_.register_builtin(
            "fixture.dummy",
            0u,
            false,
            [](eta::runtime::types::PrimitiveArgs) {
                return std::expected<
                    eta::runtime::nanbox::LispVal,
                    eta::runtime::error::RuntimeError>(eta::runtime::nanbox::Nil);
            });
        primitive_installer_.invalidate();
    }

    std::optional<uint32_t> runtime_export_slot(
        std::string_view module_name,
        std::string_view export_name) const {
        return compilation_.runtime_export_slot(module_name, export_name);
    }

    std::string diagnostics_text() const {
        std::ostringstream out;
        diagnostics_.print_all(
            out,
            /*use_color=*/false,
            [](uint32_t) { return std::string{}; });
        return out.str();
    }

    bool compile_source_to_etac(const fs::path& source_file,
                                const fs::path& etac_file,
                                std::string* error_message) {
        std::ifstream source_in(source_file, std::ios::in | std::ios::binary);
        if (!source_in) {
            if (error_message) *error_message = "failed to read source file";
            return false;
        }
        std::ostringstream source_buf;
        source_buf << source_in.rdbuf();
        const std::string source = source_buf.str();

        eta::session::CompilationSession::CompileResult compile_result;
        if (!compilation_.run_source_impl(
                *this,
                source,
                /*file_id=*/0u,
                /*result=*/nullptr,
                /*result_binding=*/{},
                /*execute=*/false,
                &compile_result)) {
            if (error_message) *error_message = diagnostics_text();
            return false;
        }
        if (compile_result.modules.empty()) {
            if (error_message) *error_message = "compile result had no modules";
            return false;
        }

        eta::semantics::BytecodeFunctionRegistry file_registry;
        const auto& all_funcs = registry_.all();
        for (uint32_t i = compile_result.base_func_idx;
             i < compile_result.end_func_idx;
             ++i) {
            auto func = eta::runtime::vm::BytecodeFunction(all_funcs[i]);
            func.rebase_func_indices(-static_cast<int32_t>(compile_result.base_func_idx));
            file_registry.add(std::move(func));
        }

        std::vector<eta::runtime::vm::ModuleEntry> module_entries;
        module_entries.reserve(compile_result.modules.size());
        for (const auto& module : compile_result.modules) {
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

        eta::runtime::vm::BytecodeSerializer serializer(heap_, intern_table_);
        std::ofstream out(etac_file, std::ios::out | std::ios::binary | std::ios::trunc);
        if (!out) {
            if (error_message) *error_message = "failed to open output .etac";
            return false;
        }

        const uint64_t source_hash =
            eta::runtime::vm::BytecodeSerializer::hash_source(source);
        if (!serializer.serialize(
                module_entries,
                file_registry,
                source_hash,
                /*include_debug=*/true,
                out,
                compile_result.imports,
                static_cast<uint32_t>(builtin_count()),
                std::nullopt,
                {},
                nullptr,
                extension_env_hash())) {
            if (error_message) *error_message = "bytecode serializer failed";
            return false;
        }

        return true;
    }

private:
    eta::runtime::memory::heap::Heap heap_;
    eta::runtime::memory::intern::InternTable intern_table_;
    eta::runtime::vm::VM vm_;
    eta::semantics::BytecodeFunctionRegistry registry_;
    eta::runtime::BuiltinEnvironment builtins_;
    eta::runtime::ExtensionEnvironment extensions_;
    eta::session::RuntimePrimitiveInstaller primitive_installer_;
    eta::diagnostic::DiagnosticEngine diagnostics_;
    eta::semantics::OptimizationPipeline optimization_pipeline_;
    eta::session::CompilationSession compilation_;
    eta::session::EtacLoader etac_loader_;
};

} // namespace

BOOST_AUTO_TEST_SUITE(etac_loader_tests)

BOOST_AUTO_TEST_CASE(run_etac_file_stale_source_hash_falls_back_to_source) {
    TempDir temp;
    const auto source_file = temp.write_file(
        "etac/loader/sample.eta",
        "(module etac.loader.sample\n"
        "  (export answer)\n"
        "  (define answer 1))\n");
    const auto etac_file = source_file.parent_path() / "sample.etac";

    EtacLoaderFixture compiler_fixture;
    std::string compile_error;
    BOOST_REQUIRE_MESSAGE(
        compiler_fixture.compile_source_to_etac(source_file, etac_file, &compile_error),
        compile_error);

    (void)temp.write_file(
        "etac/loader/sample.eta",
        "(module etac.loader.sample\n"
        "  (export answer)\n"
        "  (define answer 2))\n");

    EtacLoaderFixture runner_fixture;
    const bool ok = runner_fixture.run_etac_file(etac_file);
    BOOST_REQUIRE(ok);

    const auto diagnostics = runner_fixture.diagnostics_text();
    BOOST_TEST(diagnostics.find("stale .etac detected") != std::string::npos);
    BOOST_TEST(diagnostics.find("falling back to source") != std::string::npos);

    const auto answer_slot =
        runner_fixture.runtime_export_slot("etac.loader.sample", "answer");
    BOOST_REQUIRE(answer_slot.has_value());
    const auto decoded =
        eta::runtime::nanbox::ops::decode<int64_t>(runner_fixture.vm().globals()[*answer_slot]);
    BOOST_REQUIRE(decoded.has_value());
    BOOST_TEST(*decoded == 2);
}

BOOST_AUTO_TEST_CASE(run_etac_file_stale_without_source_reports_error) {
    TempDir temp;
    const auto source_file = temp.write_file(
        "etac/loader/stale.eta",
        "(module etac.loader.stale\n"
        "  (export answer)\n"
        "  (define answer 1))\n");
    const auto etac_file = source_file.parent_path() / "stale.etac";

    EtacLoaderFixture compiler_fixture;
    std::string compile_error;
    BOOST_REQUIRE_MESSAGE(
        compiler_fixture.compile_source_to_etac(source_file, etac_file, &compile_error),
        compile_error);

    std::error_code ec;
    fs::remove(source_file, ec);
    BOOST_REQUIRE_MESSAGE(!ec, "failed to remove sibling source before stale run");

    EtacLoaderFixture runner_fixture;
    runner_fixture.add_dummy_builtin();

    const bool ok = runner_fixture.run_etac_file(etac_file);
    BOOST_TEST(!ok);

    const auto diagnostics = runner_fixture.diagnostics_text();
    BOOST_TEST(diagnostics.find("stale .etac detected") != std::string::npos);
    BOOST_TEST(diagnostics.find("no sibling source found for fallback") != std::string::npos);
}

BOOST_AUTO_TEST_SUITE_END()
