/**
 * @file eval_engine_tests.cpp
 * @brief Unit tests for eta::session::EvalEngine.
 */

#include <boost/test/unit_test.hpp>

#include <cstdint>
#include <filesystem>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>

#include "eta/diagnostic/diagnostic.h"
#include "eta/interpreter/all_primitives.h"
#include "eta/reader/module_linker.h"
#include "eta/runtime/builtin_env.h"
#include "eta/runtime/error.h"
#include "eta/runtime/extension_env.h"
#include "eta/runtime/memory/heap.h"
#include "eta/runtime/memory/intern_table.h"
#include "eta/runtime/nanbox.h"
#include "eta/runtime/value_formatter.h"
#include "eta/runtime/vm/vm.h"
#include "eta/semantics/emitter.h"
#include "eta/semantics/optimization_pipeline.h"
#include "eta/session/compilation_session.h"
#include "eta/session/eval_engine.h"
#include "eta/session/runtime_primitives.h"

namespace fs = std::filesystem;

namespace {

class EvalEngineFixture final
    : public eta::session::CompilationSession::Host,
      public eta::session::EvalEngine::Host {
public:
    EvalEngineFixture()
        : heap_(16ull * 1024ull * 1024ull),
          vm_(heap_, intern_table_),
          primitive_installer_(heap_, builtins_, extensions_),
          eval_engine_(*this, compilation_, *this, vm_) {
        vm_.set_function_resolver([this](uint32_t index) {
            return registry_.get(index);
        });
        eta::interpreter::register_all_primitives(
            builtins_, heap_, intern_table_, vm_, {});
        eval_engine_.install_builtin(builtins_);
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

    std::string format_value(eta::runtime::nanbox::LispVal value,
                             eta::runtime::FormatMode mode) override {
        return eta::runtime::format_value(value, mode, heap_, intern_table_);
    }

    std::string diagnostics_to_string() const override {
        std::ostringstream out;
        diagnostics_.print_all(
            out,
            /*use_color=*/false,
            [](uint32_t) { return std::string{}; });
        return out.str();
    }

    eta::runtime::nanbox::LispVal run_module(std::string_view source,
                                             const std::string& result_binding = "result") {
        eta::runtime::nanbox::LispVal result{eta::runtime::nanbox::Nil};
        const bool ok = compilation_.run_source_impl(
            *this,
            std::string(source),
            /*file_id=*/0u,
            &result,
            result_binding);
        BOOST_REQUIRE_MESSAGE(ok, "run_source_impl failed:\n" + diagnostics_to_string());
        return result;
    }

    int64_t as_int(eta::runtime::nanbox::LispVal value) const {
        auto decoded = eta::runtime::nanbox::ops::decode<int64_t>(value);
        BOOST_REQUIRE(decoded.has_value());
        return *decoded;
    }

    std::string as_symbol_name(eta::runtime::nanbox::LispVal value) {
        if (!eta::runtime::nanbox::ops::is_boxed(value) ||
            eta::runtime::nanbox::ops::tag(value) != eta::runtime::nanbox::Tag::Symbol) {
            BOOST_FAIL("expected symbol result");
        }
        auto sv = intern_table_.get_string(eta::runtime::nanbox::ops::payload(value));
        BOOST_REQUIRE(sv.has_value());
        return std::string(*sv);
    }

    std::expected<eta::runtime::nanbox::LispVal, eta::runtime::error::RuntimeError>
    call_eval(eta::runtime::nanbox::LispVal value) {
        const auto eval_slot = builtins_.lookup("eval");
        BOOST_REQUIRE(eval_slot.has_value());
        const auto& eval_spec = builtins_.specs()[*eval_slot];
        BOOST_REQUIRE(static_cast<bool>(eval_spec.func));
        return eval_spec.func({value});
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
    eta::session::EvalEngine eval_engine_;
};

} // namespace

BOOST_AUTO_TEST_SUITE(eval_engine_tests)

BOOST_AUTO_TEST_CASE(synthetic_eval_binding_name_filter_matches_expected_patterns) {
    BOOST_TEST(eta::session::EvalEngine::is_synthetic_eval_binding_name("%1"));
    BOOST_TEST(eta::session::EvalEngine::is_synthetic_eval_binding_name("&42"));
    BOOST_TEST(!eta::session::EvalEngine::is_synthetic_eval_binding_name("%"));
    BOOST_TEST(!eta::session::EvalEngine::is_synthetic_eval_binding_name("%x"));
    BOOST_TEST(!eta::session::EvalEngine::is_synthetic_eval_binding_name("name"));
}

BOOST_AUTO_TEST_CASE(eval_builtin_returns_non_expression_argument_unchanged) {
    EvalEngineFixture fixture;
    const auto encoded = eta::runtime::nanbox::ops::encode<int64_t>(123);
    BOOST_REQUIRE(encoded.has_value());
    auto result = fixture.call_eval(*encoded);
    BOOST_REQUIRE(result.has_value());
    BOOST_TEST(*result == *encoded);
}

BOOST_AUTO_TEST_CASE(eval_builtin_reads_lexical_binding_from_module_execution) {
    EvalEngineFixture fixture;
    const auto result = fixture.run_module(R"eta(
(module eval.engine.lexical
  (define result
    (let ((x 21))
      (eval '(+ x 21)))))
)eta");
    BOOST_TEST(fixture.as_int(result) == 42);
}

BOOST_AUTO_TEST_CASE(eval_compile_errors_are_reported_as_runtime_user_error) {
    EvalEngineFixture fixture;
    const auto result = fixture.run_module(R"eta(
(module eval.engine.user-error
  (define payload-tag (lambda (p) (car (cdr p))))
  (define result
    (payload-tag (catch 'runtime.user-error
                   (eval '(undefined-eval-function 1))))))
)eta");
    BOOST_TEST(fixture.as_symbol_name(result) == "runtime.user-error");
}

BOOST_AUTO_TEST_SUITE_END()
