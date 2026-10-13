/**
 * @file compilation_session_tests.cpp
 * @brief Unit tests for eta::session::CompilationSession.
 */

#include <boost/test/unit_test.hpp>

#include <cstdint>
#include <filesystem>
#include <optional>
#include <sstream>
#include <span>
#include <string>
#include <vector>

#include "eta/diagnostic/diagnostic.h"
#include "eta/runtime/builtin_env.h"
#include "eta/runtime/error.h"
#include "eta/runtime/extension_env.h"
#include "eta/runtime/memory/heap.h"
#include "eta/runtime/memory/intern_table.h"
#include "eta/runtime/nanbox.h"
#include "eta/runtime/string_view.h"
#include "eta/runtime/types/types.h"
#include "eta/runtime/vm/bytecode_serializer.h"
#include "eta/runtime/vm/vm.h"
#include "eta/semantics/optimization_pipeline.h"
#include "eta/session/compilation_session.h"
#include "eta/session/runtime_primitives.h"

namespace fs = std::filesystem;

namespace {

class CompilationHostFixture final : public eta::session::CompilationSession::Host {
public:
    explicit CompilationHostFixture(std::vector<std::string> command_line_arguments = {})
        : heap_(16ull * 1024ull * 1024ull),
          vm_(heap_, intern_table_),
          primitive_installer_(heap_, builtins_, extensions_),
          command_line_arguments_(std::move(command_line_arguments)) {
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

    std::span<const std::string> command_line_arguments() const noexcept override {
        return std::span<const std::string>(command_line_arguments_);
    }

    std::string diagnostics_text() const {
        std::ostringstream out;
        diagnostics_.print_all(
            out,
            /*use_color=*/false,
            [](uint32_t) { return std::string{}; });
        return out.str();
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
    std::vector<std::string> command_line_arguments_;
};

std::vector<std::string> decode_string_list(
    eta::runtime::memory::heap::Heap& heap,
    eta::runtime::memory::intern::InternTable& intern_table,
    eta::runtime::nanbox::LispVal list) {
    using eta::runtime::memory::heap::ObjectKind;
    using eta::runtime::nanbox::Nil;
    using eta::runtime::nanbox::ops::is_boxed;
    using eta::runtime::nanbox::ops::payload;
    using eta::runtime::nanbox::ops::tag;
    std::vector<std::string> out;
    auto cur = list;
    while (cur != Nil) {
        BOOST_REQUIRE(is_boxed(cur));
        BOOST_REQUIRE(tag(cur) == eta::runtime::nanbox::Tag::HeapObject);
        auto* cell = heap.try_get_as<ObjectKind::Cons, eta::runtime::types::Cons>(payload(cur));
        BOOST_REQUIRE(cell != nullptr);
        auto text = eta::runtime::StringView::try_from(cell->car, intern_table);
        BOOST_REQUIRE(text.has_value());
        out.emplace_back(text->view());
        cur = cell->cdr;
    }
    return out;
}

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

BOOST_AUTO_TEST_CASE(run_source_impl_invokes_zero_arity_main) {
    eta::session::CompilationSession session;
    CompilationHostFixture host;
    eta::runtime::nanbox::LispVal seen = eta::runtime::nanbox::Nil;

    const bool ok = session.run_source_impl(
        host,
        "(module stage6.main.zero\n"
        "  (define seen 0)\n"
        "  (defun main ()\n"
        "    (set! seen 7)))",
        /*file_id=*/0u,
        &seen,
        "seen");
    BOOST_REQUIRE(ok);
    auto decoded = eta::runtime::nanbox::ops::decode<std::int64_t>(seen);
    BOOST_REQUIRE(decoded.has_value());
    BOOST_TEST(*decoded == 7);
}

BOOST_AUTO_TEST_CASE(run_source_impl_invokes_single_argument_main_with_cli_args) {
    eta::session::CompilationSession session;
    CompilationHostFixture host({"foo", "bar"});
    eta::runtime::nanbox::LispVal seen = eta::runtime::nanbox::Nil;

    const bool ok = session.run_source_impl(
        host,
        "(module stage6.main.args\n"
        "  (define seen '())\n"
        "  (defun main (args)\n"
        "    (set! seen args)))",
        /*file_id=*/0u,
        &seen,
        "seen");
    BOOST_REQUIRE(ok);
    const auto decoded = decode_string_list(host.heap(), host.intern_table(), seen);
    BOOST_REQUIRE_EQUAL(decoded.size(), 2u);
    BOOST_TEST(decoded[0] == "foo");
    BOOST_TEST(decoded[1] == "bar");
}

BOOST_AUTO_TEST_CASE(run_source_impl_reports_invalid_entrypoint_arity) {
    eta::session::CompilationSession session;
    CompilationHostFixture host;

    const bool ok = session.run_source_impl(
        host,
        "(module stage6.main.bad\n"
        "  (defun main (a b)\n"
        "    (+ a b)))",
        /*file_id=*/0u);
    BOOST_TEST(!ok);
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
