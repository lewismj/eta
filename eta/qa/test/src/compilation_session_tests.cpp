/**
 * @file compilation_session_tests.cpp
 * @brief Unit tests for eta::session::CompilationSession.
 */

#include <boost/test/unit_test.hpp>

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

#include "eta/diagnostic/diagnostic.h"
#include "eta/runtime/builtin_env.h"
#include "eta/runtime/error.h"
#include "eta/runtime/extension_env.h"
#include "eta/runtime/memory/heap.h"
#include "eta/runtime/memory/intern_table.h"
#include "eta/runtime/nanbox.h"
#include "eta/runtime/vm/bytecode_serializer.h"
#include "eta/runtime/vm/vm.h"
#include "eta/semantics/optimization_pipeline.h"
#include "eta/session/compilation_session.h"
#include "eta/session/runtime_primitives.h"

namespace fs = std::filesystem;

namespace {

class CompilationHostFixture final : public eta::session::CompilationSession::Host {
public:
    CompilationHostFixture()
        : heap_(16ull * 1024ull * 1024ull),
          vm_(heap_, intern_table_),
          primitive_installer_(heap_, builtins_, extensions_) {
        vm_.set_function_resolver([this](uint32_t index) {
            return registry_.get(index);
        });
    }

    std::optional<fs::path> resolve_import_path(
        const std::string&,
        bool* shadow_conflict) override {
        if (shadow_conflict) *shadow_conflict = false;
        return std::nullopt;
    }

    bool run_module_file(const fs::path&) override {
        return false;
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
};

} // namespace

BOOST_AUTO_TEST_SUITE(compilation_session_tests)

BOOST_AUTO_TEST_CASE(run_source_impl_executes_module_and_records_compile_metadata) {
    eta::session::CompilationSession session;
    CompilationHostFixture host;
    eta::session::CompilationSession::CompileResult compile_result;
    eta::runtime::nanbox::LispVal bound_value = eta::runtime::nanbox::Nil;

    const std::string source =
        "(module stage6.sample\n"
        "  (define value 42))";

    const bool ok = session.run_source_impl(
        host,
        source,
        /*file_id=*/0u,
        &bound_value,
        "value",
        /*execute=*/true,
        &compile_result);
    BOOST_REQUIRE(ok);
    BOOST_TEST(session.has_module("stage6.sample"));
    BOOST_REQUIRE_EQUAL(compile_result.modules.size(), 1u);
    BOOST_TEST(compile_result.modules.front().name == "stage6.sample");
    BOOST_TEST(compile_result.base_func_idx < compile_result.end_func_idx);

    bool found_global_name = false;
    for (const auto& [_, name] : session.global_names()) {
        if (name == "stage6.sample.value") {
            found_global_name = true;
            break;
        }
    }
    BOOST_TEST(found_global_name);

    auto decoded = eta::runtime::nanbox::ops::decode<std::int64_t>(bound_value);
    BOOST_REQUIRE(decoded.has_value());
    BOOST_TEST(*decoded == 42);
}

BOOST_AUTO_TEST_CASE(clear_module_cache_drops_execution_and_link_state) {
    eta::session::CompilationSession session;
    CompilationHostFixture host;

    BOOST_REQUIRE(session.run_source_impl(
        host,
        "(module stage6.cache (define value 10))",
        /*file_id=*/0u));

    eta::runtime::vm::ModuleEntry compiled_module;
    compiled_module.name = "stage6.cache";
    compiled_module.export_bindings.push_back({"value", 1u});
    session.record_compiled_link_exports_from_compiled_module(
        compiled_module,
        fs::path("stage6.cache.etac"));

    BOOST_REQUIRE(session.clear_module_cache("stage6.cache"));
    BOOST_TEST(!session.has_module("stage6.cache"));
    BOOST_TEST(session.runtime_module_info("stage6.cache") == nullptr);
    BOOST_TEST(!session.compiled_link_modules().contains("stage6.cache"));

    for (const auto& [_, name] : session.global_names()) {
        BOOST_TEST(!name.starts_with("stage6.cache"));
    }
}

BOOST_AUTO_TEST_CASE(etac_reservations_are_released_by_owner_module) {
    eta::session::CompilationSession session;
    CompilationHostFixture host;

    std::string reserve_module;
    BOOST_REQUIRE(session.append_etac_global_reservation(
        host,
        /*slots_to_reserve=*/3u,
        &reserve_module));
    BOOST_TEST(!reserve_module.empty());
    BOOST_TEST(session.has_module(reserve_module));

    session.add_etac_module_reservation("stage6.owner", reserve_module);
    BOOST_REQUIRE(session.release_etac_global_reservation("stage6.owner"));
    BOOST_TEST(!session.has_module(reserve_module));
}

BOOST_AUTO_TEST_SUITE_END()
