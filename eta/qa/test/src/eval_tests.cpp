/**
 * @file eval_tests.cpp
 * @brief Regression tests for eval lexical execution and error semantics.
 */

#include <boost/test/unit_test.hpp>

#include <filesystem>
#include <sstream>
#include <string>
#include <string_view>

#include "eta/interpreter/module_path.h"
#include "eta/runtime/nanbox.h"
#include "eta/session/driver.h"

namespace fs = std::filesystem;

#ifndef ETA_STDLIB_DIR
#define ETA_STDLIB_DIR ""
#endif

static fs::path stdlib_dir() {
    fs::path p(ETA_STDLIB_DIR);
    if (!p.empty() && fs::is_directory(p)) return p;

    const auto cwd = fs::current_path();
    for (const auto& candidate : {
        cwd / "stdlib",
        cwd / ".." / "stdlib",
        cwd / ".." / ".." / "stdlib",
        cwd / ".." / ".." / ".." / "stdlib",
    }) {
        if (fs::is_directory(candidate)) return fs::canonical(candidate);
    }
    return {};
}

static eta::interpreter::ModulePathResolver make_resolver() {
    auto stdlib = stdlib_dir();
    if (stdlib.empty()) return eta::interpreter::ModulePathResolver{};
    return eta::interpreter::ModulePathResolver({stdlib});
}

struct EvalHarness {
    eta::session::Driver driver;

    EvalHarness() : driver(make_resolver()) {}

    [[nodiscard]] std::string diagnostics_string() const {
        std::ostringstream oss;
        driver.diagnostics().print_all(oss, /*use_color=*/false, driver.file_resolver());
        return oss.str();
    }

    eta::runtime::nanbox::LispVal run_module(std::string_view source) {
        eta::runtime::nanbox::LispVal result{eta::runtime::nanbox::Nil};
        const bool ok = driver.run_source(source, &result, "result");
        BOOST_REQUIRE_MESSAGE(ok, "run_source failed:\n" + diagnostics_string());
        return result;
    }

    int64_t as_int(eta::runtime::nanbox::LispVal value) const {
        auto decoded = eta::runtime::nanbox::ops::decode<int64_t>(value);
        BOOST_REQUIRE_MESSAGE(decoded.has_value(), "expected fixnum result");
        return *decoded;
    }

    std::string as_symbol_name(eta::runtime::nanbox::LispVal value) {
        if (!eta::runtime::nanbox::ops::is_boxed(value) ||
            eta::runtime::nanbox::ops::tag(value) != eta::runtime::nanbox::Tag::Symbol) {
            BOOST_FAIL("expected symbol result");
        }
        auto sv = driver.intern_table().get_string(eta::runtime::nanbox::ops::payload(value));
        BOOST_REQUIRE_MESSAGE(sv.has_value(), "symbol intern id was not found");
        return std::string(*sv);
    }
};

BOOST_AUTO_TEST_SUITE(eval_tests)

BOOST_AUTO_TEST_CASE(eval_reads_lexical_local_binding) {
    EvalHarness harness;
    auto result = harness.run_module(R"eta(
(module eval.cpp.local
  (define result
    (let ((x 10))
      (eval '(+ x 5)))))
)eta");
    BOOST_TEST(harness.as_int(result) == 15);
}

BOOST_AUTO_TEST_CASE(eval_reads_lexical_upvalue_binding) {
    EvalHarness harness;
    auto result = harness.run_module(R"eta(
(module eval.cpp.upvalue
  (define result
    (let ((x 10))
      (let ((f (lambda (y) (eval '(+ x y)))))
        (f 7)))))
)eta");
    BOOST_TEST(harness.as_int(result) == 17);
}

BOOST_AUTO_TEST_CASE(eval_preserves_runtime_error_tag) {
    EvalHarness harness;
    auto result = harness.run_module(R"eta(
(module eval.cpp.runtime-tag
  (define payload-tag (lambda (p) (car (cdr p))))
  (define result
    (payload-tag (catch 'runtime.type-error (eval '(car 1))))))
)eta");
    BOOST_TEST(harness.as_symbol_name(result) == "runtime.type-error");
}

BOOST_AUTO_TEST_CASE(eval_preserves_user_raise_tag) {
    EvalHarness harness;
    auto result = harness.run_module(R"eta(
(module eval.cpp.user-tag
  (define result
    (catch 'my-tag (eval '(raise 'my-tag 11)))))
)eta");
    BOOST_TEST(harness.as_int(result) == 11);
}

BOOST_AUTO_TEST_CASE(eval_compile_errors_surface_as_runtime_user_error) {
    EvalHarness harness;
    auto result = harness.run_module(R"eta(
(module eval.cpp.compile-error
  (define payload-tag (lambda (p) (car (cdr p))))
  (define result
    (payload-tag (catch 'runtime.user-error (eval '(undefined-eval-function 1))))))
)eta");
    BOOST_TEST(harness.as_symbol_name(result) == "runtime.user-error");
}

BOOST_AUTO_TEST_CASE(eval_reentrant_recursion_remains_stable) {
    EvalHarness harness;
    auto result = harness.run_module(R"eta(
(module eval.cpp.reentrant
  (define result
    (letrec ((step (lambda (n acc)
                     (if (= n 0)
                         acc
                         (step (- n 1) (eval `(+ ,acc 1)))))))
      (step 64 0))))
)eta");
    BOOST_TEST(harness.as_int(result) == 64);
}

BOOST_AUTO_TEST_CASE(vm_execution_snapshot_stack_is_lifo) {
    eta::runtime::memory::heap::Heap heap(1024 * 1024);
    eta::runtime::memory::intern::InternTable intern_table;
    eta::runtime::vm::VM vm(heap, intern_table);

    BOOST_TEST(vm.saved_executions().empty());
    vm.save_execution_state();
    vm.save_execution_state();
    BOOST_TEST(vm.saved_executions().size() == 2u);

    vm.restore_execution_state();
    BOOST_TEST(vm.saved_executions().size() == 1u);

    vm.restore_execution_state();
    BOOST_TEST(vm.saved_executions().empty());
}

BOOST_AUTO_TEST_SUITE_END()
