/**
 * @file repl_redefine_tests.cpp
 * @brief REPL redefinition regression tests for shadowing and import selection.
 */

#include <boost/test/unit_test.hpp>

#include <filesystem>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "eta/session/driver.h"
#include "eta/interpreter/module_path.h"
#include "eta/interpreter/repl_wrap.h"
#include "eta/runtime/nanbox.h"

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

struct ReplHarness {
    eta::session::Driver driver;
    bool prelude_available{false};
    int repl_counter{0};
    std::vector<eta::interpreter::PriorModule> prior_modules;

    ReplHarness() : driver(make_resolver()) {
        auto pr = driver.load_prelude();
        BOOST_REQUIRE_MESSAGE(pr.found, "prelude.eta was not found for REPL tests");
        BOOST_REQUIRE_MESSAGE(pr.loaded, "prelude.eta failed to load for REPL tests");
        prelude_available = driver.has_module("std.prelude");
    }

    static eta::interpreter::ModulePathResolver make_resolver() {
        auto stdlib = stdlib_dir();
        if (stdlib.empty()) return eta::interpreter::ModulePathResolver{};
        return eta::interpreter::ModulePathResolver({stdlib});
    }

    std::string diagnostics_string() const {
        std::ostringstream oss;
        driver.diagnostics().print_all(oss, /*use_color=*/false, driver.file_resolver());
        return oss.str();
    }

    bool submit(const std::vector<std::string>& forms,
                eta::runtime::nanbox::LispVal* out_result = nullptr) {
        auto wrapped = eta::interpreter::wrap_repl_submission(
            forms, repl_counter++, prelude_available, prior_modules);

        if (wrapped.source.empty()) return true;

        eta::runtime::nanbox::LispVal tmp_result{};
        auto* result_ptr = out_result ? out_result : &tmp_result;

        bool ok = wrapped.last_is_expr
            ? driver.run_source(wrapped.source, result_ptr, wrapped.result_name)
            : driver.run_source(wrapped.source);

        if (ok) {
            prior_modules.push_back(eta::interpreter::PriorModule{
                wrapped.module_name, wrapped.user_defines});
        }
        return ok;
    }

    bool submit(std::string_view form,
                eta::runtime::nanbox::LispVal* out_result = nullptr) {
        return submit(std::vector<std::string>{std::string(form)}, out_result);
    }

    void require_submit(const std::vector<std::string>& forms) {
        const bool ok = submit(forms);
        BOOST_REQUIRE_MESSAGE(ok, "REPL submission failed:\n" + diagnostics_string());
    }

    void require_submit(std::string_view form) {
        const bool ok = submit(form);
        BOOST_REQUIRE_MESSAGE(ok, "REPL submission failed:\n" + diagnostics_string());
    }

    int64_t eval_int(std::string_view expr) {
        eta::runtime::nanbox::LispVal value{};
        const bool ok = submit(expr, &value);
        BOOST_REQUIRE_MESSAGE(ok, "REPL expression failed:\n" + diagnostics_string());

        auto decoded = eta::runtime::nanbox::ops::decode<int64_t>(value);
        BOOST_REQUIRE_MESSAGE(decoded.has_value(), "expression did not evaluate to fixnum");
        return *decoded;
    }
};

BOOST_AUTO_TEST_SUITE(repl_redefine_tests)

BOOST_AUTO_TEST_CASE(simple_redefinition_uses_latest_definition) {
    ReplHarness repl;
    repl.require_submit("(defun f (x) x)");
    repl.require_submit("(defun f (x) (+ x 1))");
    BOOST_TEST(repl.eval_int("(f 10)") == 11);
}

BOOST_AUTO_TEST_CASE(redefinition_after_use_updates_future_calls) {
    ReplHarness repl;
    repl.require_submit("(defun f (x) x)");
    BOOST_TEST(repl.eval_int("(f 5)") == 5);
    repl.require_submit("(defun f (x) (* x 2))");
    BOOST_TEST(repl.eval_int("(f 5)") == 10);
}

BOOST_AUTO_TEST_CASE(triple_redefinition_keeps_single_live_binding) {
    ReplHarness repl;
    repl.require_submit("(define x 1)");
    repl.require_submit("(define x 2)");
    repl.require_submit("(define x 3)");
    BOOST_TEST(repl.eval_int("x") == 3);
}

BOOST_AUTO_TEST_CASE(existing_functions_keep_original_bindings) {
    ReplHarness repl;
    repl.require_submit("(defun f (x) x)");
    repl.require_submit("(defun g (x) (f x))");
    repl.require_submit("(defun f (x) (* x 2))");

    BOOST_TEST(repl.eval_int("(g 5)") == 5);
    BOOST_TEST(repl.eval_int("(f 5)") == 10);
}

BOOST_AUTO_TEST_CASE(selective_import_preserves_unshadowed_names) {
    ReplHarness repl;
    repl.require_submit(std::vector<std::string>{"(define a 1)", "(define b 2)"});
    repl.require_submit("(define a 10)");
    BOOST_TEST(repl.eval_int("(+ a b)") == 12);
}

BOOST_AUTO_TEST_CASE(define_record_type_persists_across_submissions) {
    ReplHarness repl;
    repl.require_submit(
        "(define-record-type <counter> "
        "(make-counter value) "
        "counter? "
        "(value counter-value set-counter-value!))");

    BOOST_TEST(repl.eval_int("(counter-value (make-counter 41))") == 41);
    BOOST_TEST(repl.eval_int("(if (counter? (make-counter 0)) 1 0)") == 1);

    repl.require_submit("(define c (make-counter 1))");
    repl.require_submit("(set-counter-value! c 7)");
    BOOST_TEST(repl.eval_int("(counter-value c)") == 7);
}

BOOST_AUTO_TEST_CASE(set_mutates_prior_binding_across_submissions) {
    ReplHarness repl;
    repl.require_submit("(define x 1)");
    repl.require_submit("(set! x 2)");
    BOOST_TEST(repl.eval_int("x") == 2);
}

BOOST_AUTO_TEST_CASE(dynamic_wind_cleanup_runs_with_cross_submission_set) {
    ReplHarness repl;
    repl.require_submit("(define cleanup-called #f)");
    repl.require_submit(
        "(catch 'resource-error "
        "  (dynamic-wind "
        "    (lambda () (set! cleanup-called #f)) "
        "    (lambda () (raise 'resource-error \"boom\")) "
        "    (lambda () (set! cleanup-called #t))))");
    BOOST_TEST(repl.eval_int("(if cleanup-called 1 0)") == 1);
}

BOOST_AUTO_TEST_SUITE_END()

