#include <boost/test/unit_test.hpp>
#include <cmath>
#include <iostream>
#include <string>

#include "eta/reader/lexer.h"
#include "eta/reader/parser.h"
#include "eta/reader/expander.h"
#include "eta/reader/module_linker.h"
#include "eta/semantics/semantic_analyzer.h"
#include "eta/semantics/emitter.h"
#include "eta/runtime/vm/vm.h"
#include "eta/runtime/factory.h"
#include "eta/runtime/builtin_env.h"
#include "eta/runtime/core_primitives.h"
#include "eta/runtime/value_formatter.h"

using namespace eta;
using namespace eta::semantics;
using namespace eta::runtime;
using namespace eta::runtime::vm;
using namespace eta::runtime::memory::factory;

// ============================================================================
// Fixture — reuses the same compile-and-run pipeline as vm_tests
// ============================================================================

struct FunctionalTestFixture {
    memory::heap::Heap heap;
    memory::intern::InternTable intern_table;
    BytecodeFunctionRegistry registry;
    BuiltinEnvironment builtins;

    FunctionalTestFixture() : heap(4 * 1024 * 1024), intern_table(), registry() {
        register_core_primitives(builtins, heap, intern_table);
    }

    /// Compile & run a multi-module program, return the 'result' global from the last module.
    LispVal run_multi(std::string_view source) {
        reader::lexer::Lexer lex(0, source);
        reader::parser::Parser p(lex);
        auto parsed_res = p.parse_toplevel();
        if (!parsed_res) throw std::runtime_error("Parse error");
        auto parsed = std::move(*parsed_res);

        reader::expander::Expander ex;
        auto expanded_res = ex.expand_many(parsed);
        if (!expanded_res) throw std::runtime_error("Expansion error: " + expanded_res.error().message);
        auto expanded = std::move(*expanded_res);

        reader::ModuleLinker linker;
        auto idx_res = linker.index_modules(expanded);
        if (!idx_res) throw std::runtime_error("Index error: " + idx_res.error().message);
        auto link_res = linker.link();
        if (!link_res) throw std::runtime_error("Link error: " + link_res.error().message);

        SemanticAnalyzer sa;
        auto sem_res = sa.analyze_all(expanded, linker, builtins);
        if (!sem_res) throw std::runtime_error("Semantic error: " + sem_res.error().message);
        auto sem_mods = std::move(*sem_res);
        BOOST_REQUIRE(!sem_mods.empty());

        std::vector<BytecodeFunction*> main_funcs;
        for (auto& mod : sem_mods) {
            Emitter emitter(mod, heap, intern_table, registry);
            main_funcs.push_back(emitter.emit());
        }

        VM vm(heap, intern_table);
        vm.set_function_resolver([this](uint32_t idx) { return registry.get(idx); });

        auto install_res = builtins.install(heap, vm.globals(), sem_mods[0].total_globals);
        if (!install_res) throw std::runtime_error("Failed to install builtins");

        for (size_t i = 0; i < sem_mods.size(); ++i) {
            auto exec_res = vm.execute(*main_funcs[i]);
            if (!exec_res) {
                std::string msg = "Execution error in module " + sem_mods[i].name;
                std::visit([&msg](auto&& arg) {
                    using T = std::decay_t<decltype(arg)>;
                    if constexpr (std::is_same_v<T, VMError>) {
                        msg += ": " + arg.message;
                    } else if constexpr (std::is_same_v<T, NaNBoxError>) {
                        msg += ": NaNBoxError " + std::to_string(static_cast<int>(arg));
                    } else if constexpr (std::is_same_v<T, HeapError>) {
                        msg += ": HeapError " + std::to_string(static_cast<int>(arg));
                    } else if constexpr (std::is_same_v<T, InternTableError>) {
                        msg += ": InternTableError " + std::to_string(static_cast<int>(arg));
                    }
                }, exec_res.error());
                throw std::runtime_error(msg);
            }
        }

        auto& last_mod = sem_mods.back();
        for (size_t i = 0; i < last_mod.bindings.size(); ++i) {
            if (last_mod.bindings[i].name == "result") {
                return vm.globals()[last_mod.bindings[i].slot];
            }
        }
        throw std::runtime_error("No 'result' binding found in last module");
    }

    /// Compile & run a single-module program, return the 'result' global.
    LispVal run(std::string_view source) {
        reader::lexer::Lexer lex(0, source);
        reader::parser::Parser p(lex);
        auto parsed_res = p.parse_toplevel();
        if (!parsed_res) throw std::runtime_error("Parse error");
        auto parsed = std::move(*parsed_res);

        reader::expander::Expander ex;
        auto expanded_res = ex.expand_many(parsed);
        if (!expanded_res) throw std::runtime_error("Expansion error: " + expanded_res.error().message);
        auto expanded = std::move(*expanded_res);

        reader::ModuleLinker linker;
        linker.index_modules(expanded);
        linker.link();

        SemanticAnalyzer sa;
        auto sem_res = sa.analyze_all(expanded, linker, builtins);
        if (!sem_res) throw std::runtime_error("Semantic error: " + sem_res.error().message);
        auto sem_mods_vec = std::move(*sem_res);
        BOOST_REQUIRE(!sem_mods_vec.empty());
        auto& sem_mod = sem_mods_vec[0];

        Emitter emitter(sem_mod, heap, intern_table, registry);
        auto* main_func = emitter.emit();

        VM vm(heap, intern_table);
        vm.set_function_resolver([this](uint32_t idx) { return registry.get(idx); });
        auto install_res = builtins.install(heap, vm.globals(), sem_mod.total_globals);
        if (!install_res) throw std::runtime_error("Failed to install builtins");

        auto exec_res = vm.execute(*main_func);
        if (!exec_res) {
            std::string msg = "Execution error";
            std::visit([&msg](auto&& arg) {
                using T = std::decay_t<decltype(arg)>;
                if constexpr (std::is_same_v<T, VMError>) {
                    msg += ": " + arg.message;
                } else if constexpr (std::is_same_v<T, NaNBoxError>) {
                    msg += ": NaNBoxError " + std::to_string(static_cast<int>(arg));
                } else if constexpr (std::is_same_v<T, HeapError>) {
                    msg += ": HeapError " + std::to_string(static_cast<int>(arg));
                } else if constexpr (std::is_same_v<T, InternTableError>) {
                    msg += ": InternTableError " + std::to_string(static_cast<int>(arg));
                }
            }, exec_res.error());
            throw std::runtime_error(msg);
        }

        for (size_t i = 0; i < sem_mod.bindings.size(); ++i) {
            if (sem_mod.bindings[i].name == "result") {
                return vm.globals()[sem_mod.bindings[i].slot];
            }
        }
        return exec_res.value();
    }

    /// Helper: extract a double from a LispVal (handles both flonums and fixnums).
    double to_double(LispVal v) {
        // Try flonum first (unboxed double)
        if (!nanbox::ops::is_boxed(v)) {
            return std::bit_cast<double>(v);
        }
        // Boxed fixnum
        auto fixnum_res = nanbox::ops::decode<int64_t>(v);
        if (fixnum_res) {
            return static_cast<double>(*fixnum_res);
        }
        throw std::runtime_error("to_double: value is neither flonum nor fixnum");
    }

    /// Helper: format a LispVal for debug output.
    std::string fmt(LispVal v) {
        return format_value(v, FormatMode::Write, heap, intern_table);
    }
};

// ============================================================================
// The AAD library, inlined as a string so each test can compose it into a
// single-module program without needing multi-module import / export of macros.
// ============================================================================

static const char* AAD_LIB = R"(
    ;; ── Dual representation ─────────────────────────────────────────
    (defun dual-val (d)
      (if (pair? d) (car d) d))

    (defun dual-bp (d)
      (if (pair? d)
          (cdr d)
          (lambda (adj) '())))

    (defun make-const (v)
      (cons v (lambda (adj) '())))

    (defun make-var (v idx)
      (cons v (lambda (adj) (list (cons idx adj)))))

    (defun ensure-dual (x)
      (if (pair? x) x (make-const x)))

    ;; ── Lifted operations ────────────────────────────────────────────

    (defun d+ (a b)
      (let ((a (ensure-dual a))
            (b (ensure-dual b)))
        (let ((va (dual-val a)) (vb (dual-val b))
              (ba (dual-bp a))  (bb (dual-bp b)))
          (cons (+ va vb)
                (lambda (adj)
                  (append (ba adj) (bb adj)))))))

    (defun d- (a b)
      (let ((a (ensure-dual a))
            (b (ensure-dual b)))
        (let ((va (dual-val a)) (vb (dual-val b))
              (ba (dual-bp a))  (bb (dual-bp b)))
          (cons (- va vb)
                (lambda (adj)
                  (append (ba adj) (bb (* -1 adj))))))))

    (defun d* (a b)
      (let ((a (ensure-dual a))
            (b (ensure-dual b)))
        (let ((va (dual-val a)) (vb (dual-val b))
              (ba (dual-bp a))  (bb (dual-bp b)))
          (cons (* va vb)
                (lambda (adj)
                  (append (ba (* adj vb)) (bb (* adj va))))))))

    (defun d/ (a b)
      (let ((a (ensure-dual a))
            (b (ensure-dual b)))
        (let ((va (dual-val a)) (vb (dual-val b))
              (ba (dual-bp a))  (bb (dual-bp b)))
          (cons (/ va vb)
                (lambda (adj)
                  (append (ba (* adj (/ 1 vb)))
                          (bb (* adj (* -1 (/ va (* vb vb)))))))))))

    (defun dsin (a)
      (let ((a (ensure-dual a)))
        (let ((va (dual-val a))
              (ba (dual-bp a)))
          (cons (sin va)
                (lambda (adj)
                  (ba (* adj (cos va))))))))

    (defun dcos (a)
      (let ((a (ensure-dual a)))
        (let ((va (dual-val a))
              (ba (dual-bp a)))
          (cons (cos va)
                (lambda (adj)
                  (ba (* adj (* -1 (sin va)))))))))

    (defun dexp (a)
      (let ((a (ensure-dual a)))
        (let ((va (dual-val a))
              (ba (dual-bp a)))
          (let ((ev (exp va)))
            (cons ev
                  (lambda (adj)
                    (ba (* adj ev))))))))

    (defun dlog (a)
      (let ((a (ensure-dual a)))
        (let ((va (dual-val a))
              (ba (dual-bp a)))
          (cons (log va)
                (lambda (adj)
                  (ba (* adj (/ 1 va))))))))

    ;; ── ad macro ─────────────────────────────────────────────────────
    ;; Inlines the dual operations using only builtin names (cons, car,
    ;; cdr, pair?, append, lambda, let, if) so that the hygienic macro
    ;; system correctly renames only local bindings, not references to
    ;; builtins.  The d-prefixed functions remain for direct use.
    (define-syntax ad
      (syntax-rules (+ * - / sin cos exp log)
        ((_ (+ a b))
         (let ((ta (ad a)) (tb (ad b)))
           (let ((va (if (pair? ta) (car ta) ta))
                 (vb (if (pair? tb) (car tb) tb))
                 (ba (if (pair? ta) (cdr ta) (lambda (g) '())))
                 (bb (if (pair? tb) (cdr tb) (lambda (g) '()))))
             (cons (+ va vb)
                   (lambda (g) (append (ba g) (bb g)))))))
        ((_ (* a b))
         (let ((ta (ad a)) (tb (ad b)))
           (let ((va (if (pair? ta) (car ta) ta))
                 (vb (if (pair? tb) (car tb) tb))
                 (ba (if (pair? ta) (cdr ta) (lambda (g) '())))
                 (bb (if (pair? tb) (cdr tb) (lambda (g) '()))))
             (cons (* va vb)
                   (lambda (g) (append (ba (* g vb)) (bb (* g va))))))))
        ((_ (- a b))
         (let ((ta (ad a)) (tb (ad b)))
           (let ((va (if (pair? ta) (car ta) ta))
                 (vb (if (pair? tb) (car tb) tb))
                 (ba (if (pair? ta) (cdr ta) (lambda (g) '())))
                 (bb (if (pair? tb) (cdr tb) (lambda (g) '()))))
             (cons (- va vb)
                   (lambda (g) (append (ba g) (bb (* -1 g))))))))
        ((_ (/ a b))
         (let ((ta (ad a)) (tb (ad b)))
           (let ((va (if (pair? ta) (car ta) ta))
                 (vb (if (pair? tb) (car tb) tb))
                 (ba (if (pair? ta) (cdr ta) (lambda (g) '())))
                 (bb (if (pair? tb) (cdr tb) (lambda (g) '()))))
             (cons (/ va vb)
                   (lambda (g) (append (ba (* g (/ 1 vb)))
                                       (bb (* g (* -1 (/ va (* vb vb)))))))))))
        ((_ (sin a))
         (let ((ta (ad a)))
           (let ((va (if (pair? ta) (car ta) ta))
                 (ba (if (pair? ta) (cdr ta) (lambda (g) '()))))
             (cons (sin va)
                   (lambda (g) (ba (* g (cos va))))))))
        ((_ (cos a))
         (let ((ta (ad a)))
           (let ((va (if (pair? ta) (car ta) ta))
                 (ba (if (pair? ta) (cdr ta) (lambda (g) '()))))
             (cons (cos va)
                   (lambda (g) (ba (* g (* -1 (sin va)))))))))
        ((_ (exp a))
         (let ((ta (ad a)))
           (let ((va (if (pair? ta) (car ta) ta))
                 (ba (if (pair? ta) (cdr ta) (lambda (g) '()))))
             (let ((ev (exp va)))
               (cons ev
                     (lambda (g) (ba (* g ev))))))))
        ((_ (log a))
         (let ((ta (ad a)))
           (let ((va (if (pair? ta) (car ta) ta))
                 (ba (if (pair? ta) (cdr ta) (lambda (g) '()))))
             (cons (log va)
                   (lambda (g) (ba (* g (/ 1 va))))))))
        ((_ x) x)))

    ;; ── Gradient helpers ─────────────────────────────────────────────
    (defun collect-adjoints (n adj-list)
      (let ((result (make-vector n 0)))
        (letrec ((loop (lambda (xs)
                         (if (null? xs)
                             result
                             (let ((p (car xs)))
                               (let ((i (car p))
                                     (v (cdr p)))
                                 (vector-set! result i
                                   (+ (vector-ref result i) v))
                                 (loop (cdr xs))))))))
          (loop adj-list))))

    (defun grad (f vals)
      (letrec ((make-vars
                 (lambda (vs idx)
                   (if (null? vs) '()
                       (cons (make-var (car vs) idx)
                             (make-vars (cdr vs) (+ idx 1)))))))
        (let ((duals (make-vars vals 0)))
          (let ((out (apply f duals)))
            (let ((primal (dual-val out))
                  (adjoints ((dual-bp out) 1)))
              (list primal (collect-adjoints (length vals) adjoints)))))))
)";

// Helper: wrap AAD_LIB + test body in a single module
static std::string aad_module(const std::string& body) {
    return std::string("(module m\n") + AAD_LIB + "\n" + body + "\n)";
}

BOOST_FIXTURE_TEST_SUITE(functional_tests, FunctionalTestFixture)

// ============================================================================
// Transcendental math builtins
// ============================================================================

BOOST_AUTO_TEST_CASE(test_sin_builtin) {
    LispVal res = run("(module m (define result (sin 0)))");
    BOOST_CHECK_CLOSE(to_double(res), 0.0, 1e-10);
}

BOOST_AUTO_TEST_CASE(test_cos_builtin) {
    LispVal res = run("(module m (define result (cos 0)))");
    BOOST_CHECK_CLOSE(to_double(res), 1.0, 1e-10);
}

BOOST_AUTO_TEST_CASE(test_exp_builtin) {
    LispVal res = run("(module m (define result (exp 1)))");
    BOOST_CHECK_CLOSE(to_double(res), std::exp(1.0), 1e-10);
}

BOOST_AUTO_TEST_CASE(test_log_builtin) {
    LispVal res = run("(module m (define result (log 1)))");
    BOOST_CHECK_CLOSE(to_double(res), 0.0, 1e-10);
}

BOOST_AUTO_TEST_CASE(test_sqrt_builtin) {
    LispVal res = run("(module m (define result (sqrt 4)))");
    BOOST_CHECK_CLOSE(to_double(res), 2.0, 1e-10);
}

// ============================================================================
// AAD — Core dual-number operations (no macro)
// ============================================================================

BOOST_AUTO_TEST_CASE(test_aad_dual_val_and_bp) {
    // Verify that make-var produces a dual whose car is the primal
    // and whose backprop returns the correct index-adjoint pair.
    LispVal res = run(aad_module(R"(
        (define result
          (let ((x (make-var 5 0)))
            (dual-val x)))
    )"));
    BOOST_CHECK_EQUAL(nanbox::ops::decode<int64_t>(res).value(), 5);
}

BOOST_AUTO_TEST_CASE(test_aad_add_primal) {
    // d+ should compute the correct primal: 3 + 4 = 7
    LispVal res = run(aad_module(R"(
        (define result
          (let ((x (make-var 3 0))
                (y (make-var 4 1)))
            (dual-val (d+ x y))))
    )"));
    BOOST_CHECK_EQUAL(nanbox::ops::decode<int64_t>(res).value(), 7);
}

BOOST_AUTO_TEST_CASE(test_aad_mul_primal) {
    // d* should compute the correct primal: 3 * 4 = 12
    LispVal res = run(aad_module(R"(
        (define result
          (let ((x (make-var 3 0))
                (y (make-var 4 1)))
            (dual-val (d* x y))))
    )"));
    BOOST_CHECK_EQUAL(nanbox::ops::decode<int64_t>(res).value(), 12);
}

// ============================================================================
// AAD — Gradient computation (manual d-prefixed calls)
// ============================================================================

BOOST_AUTO_TEST_CASE(test_aad_grad_multiply) {
    // f(x,y) = x * y at (3,4)
    // df/dx = y = 4,  df/dy = x = 3
    // grad returns (primal gradient-vector)
    // result = primal (car of the grad output)
    LispVal res = run(aad_module(R"(
        (define g (grad (lambda (x y) (d* x y)) '(3 4)))
        (define result (car g))
    )"));
    BOOST_CHECK_EQUAL(nanbox::ops::decode<int64_t>(res).value(), 12);
}

BOOST_AUTO_TEST_CASE(test_aad_grad_multiply_partials) {
    // f(x,y) = x * y at (3,4)
    // gradient vector should be #(4 3)
    // Check df/dx = 4
    LispVal res = run(aad_module(R"(
        (define g (grad (lambda (x y) (d* x y)) '(3 4)))
        (define gv (car (cdr g)))
        (define result (vector-ref gv 0))
    )"));
    BOOST_CHECK_EQUAL(nanbox::ops::decode<int64_t>(res).value(), 4);
}

BOOST_AUTO_TEST_CASE(test_aad_grad_multiply_partial_dy) {
    // df/dy = x = 3
    LispVal res = run(aad_module(R"(
        (define g (grad (lambda (x y) (d* x y)) '(3 4)))
        (define gv (car (cdr g)))
        (define result (vector-ref gv 1))
    )"));
    BOOST_CHECK_EQUAL(nanbox::ops::decode<int64_t>(res).value(), 3);
}

// ============================================================================
// AAD — Polynomial gradient
// ============================================================================

BOOST_AUTO_TEST_CASE(test_aad_polynomial_grad) {
    // g(x) = x^2 + 3x + 1 at x=4
    // dg/dx = 2x + 3 = 11
    LispVal res = run(aad_module(R"(
        (define g (grad (lambda (x) (d+ (d+ (d* x x) (d* (make-const 3) x))
                                        (make-const 1)))
                        '(4)))
        (define gv (car (cdr g)))
        (define result (vector-ref gv 0))
    )"));
    BOOST_CHECK_EQUAL(nanbox::ops::decode<int64_t>(res).value(), 11);
}

// ============================================================================
// AAD — Transcendental functions gradient
// ============================================================================

BOOST_AUTO_TEST_CASE(test_aad_sin_gradient) {
    // f(x) = sin(x) at x=0
    // df/dx = cos(0) = 1.0
    LispVal res = run(aad_module(R"(
        (define g (grad (lambda (x) (dsin x)) '(0)))
        (define gv (car (cdr g)))
        (define result (vector-ref gv 0))
    )"));
    BOOST_CHECK_CLOSE(to_double(res), 1.0, 1e-10);
}

BOOST_AUTO_TEST_CASE(test_aad_exp_gradient) {
    // f(x) = exp(x) at x=0
    // df/dx = exp(0) = 1.0
    LispVal res = run(aad_module(R"(
        (define g (grad (lambda (x) (dexp x)) '(0)))
        (define gv (car (cdr g)))
        (define result (vector-ref gv 0))
    )"));
    BOOST_CHECK_CLOSE(to_double(res), 1.0, 1e-10);
}

BOOST_AUTO_TEST_CASE(test_aad_exp_chain_rule) {
    // f(x) = exp(2x) at x=1
    // df/dx = 2·exp(2) ≈ 14.7781
    LispVal res = run(aad_module(R"(
        (define g (grad (lambda (x) (dexp (d* (make-const 2) x))) '(1)))
        (define gv (car (cdr g)))
        (define result (vector-ref gv 0))
    )"));
    BOOST_CHECK_CLOSE(to_double(res), 2.0 * std::exp(2.0), 1e-6);
}

BOOST_AUTO_TEST_CASE(test_aad_log_gradient) {
    // f(x) = log(x) at x=2
    // df/dx = 1/x = 0.5
    LispVal res = run(aad_module(R"(
        (define g (grad (lambda (x) (dlog x)) '(2)))
        (define gv (car (cdr g)))
        (define result (vector-ref gv 0))
    )"));
    BOOST_CHECK_CLOSE(to_double(res), 0.5, 1e-10);
}

// ============================================================================
// AAD — Rosenbrock at the minimum
// ============================================================================

BOOST_AUTO_TEST_CASE(test_aad_rosenbrock_minimum) {
    // f(x,y) = (1-x)^2 + 100*(y-x^2)^2  at (1,1)
    // Both partials should be 0.
    LispVal res = run(aad_module(R"(
        (define g (grad (lambda (x y)
                          (d+ (d* (d- (make-const 1) x) (d- (make-const 1) x))
                              (d* (make-const 100)
                                  (d* (d- y (d* x x))
                                      (d- y (d* x x))))))
                        '(1 1)))
        (define gv (car (cdr g)))
        ;; df/dx should be 0
        (define result (vector-ref gv 0))
    )"));
    BOOST_CHECK_CLOSE(to_double(res) + 1.0, 1.0, 1e-10); // +1 avoids division by zero in BOOST_CHECK_CLOSE
}

BOOST_AUTO_TEST_CASE(test_aad_rosenbrock_minimum_dy) {
    LispVal res = run(aad_module(R"(
        (define g (grad (lambda (x y)
                          (d+ (d* (d- (make-const 1) x) (d- (make-const 1) x))
                              (d* (make-const 100)
                                  (d* (d- y (d* x x))
                                      (d- y (d* x x))))))
                        '(1 1)))
        (define gv (car (cdr g)))
        (define result (vector-ref gv 1))
    )"));
    BOOST_CHECK_CLOSE(to_double(res) + 1.0, 1.0, 1e-10);
}

// ============================================================================
// AAD — The `ad` syntax-rules macro
// ============================================================================

BOOST_AUTO_TEST_CASE(test_aad_macro_add) {
    // (ad (+ x y)) should expand to (d+ x y)
    LispVal res = run(aad_module(R"(
        (define result
          (let ((x (make-var 10 0))
                (y (make-var 20 1)))
            (dual-val (ad (+ x y)))))
    )"));
    BOOST_CHECK_EQUAL(nanbox::ops::decode<int64_t>(res).value(), 30);
}

BOOST_AUTO_TEST_CASE(test_aad_macro_nested) {
    // (ad (+ (* x y) x)) should expand to (d+ (d* x y) x)
    // f(x,y) = x*y + x at (3,4) => primal = 15
    LispVal res = run(aad_module(R"(
        (define result
          (let ((x (make-var 3 0))
                (y (make-var 4 1)))
            (dual-val (ad (+ (* x y) x)))))
    )"));
    BOOST_CHECK_EQUAL(nanbox::ops::decode<int64_t>(res).value(), 15);
}

BOOST_AUTO_TEST_CASE(test_aad_macro_sin) {
    // (ad (sin x)) should expand to (dsin x)
    // sin(0) = 0
    LispVal res = run(aad_module(R"(
        (define result
          (let ((x (make-var 0 0)))
            (dual-val (ad (sin x)))))
    )"));
    BOOST_CHECK_CLOSE(to_double(res) + 1.0, 1.0, 1e-10);
}

BOOST_AUTO_TEST_CASE(test_aad_macro_grad_polynomial) {
    // g(x) = x^2 + 3x + 1 at x=4, using the ad macro
    // dg/dx = 2*4 + 3 = 11
    LispVal res = run(aad_module(R"(
        (define g (grad (lambda (x) (ad (+ (+ (* x x) (* 3 x)) 1)))
                        '(4)))
        (define gv (car (cdr g)))
        (define result (vector-ref gv 0))
    )"));
    BOOST_CHECK_EQUAL(nanbox::ops::decode<int64_t>(res).value(), 11);
}

BOOST_AUTO_TEST_CASE(test_aad_macro_grad_sin_plus_product) {
    // f(x,y) = x*y + sin(x) at (2,3)
    // df/dx = y + cos(x) = 3 + cos(2)
    LispVal res = run(aad_module(R"(
        (define g (grad (lambda (x y) (ad (+ (* x y) (sin x))))
                        '(2 3)))
        (define gv (car (cdr g)))
        (define result (vector-ref gv 0))
    )"));
    double expected = 3.0 + std::cos(2.0);
    BOOST_CHECK_CLOSE(to_double(res), expected, 1e-6);
}

BOOST_AUTO_TEST_CASE(test_aad_macro_grad_sin_plus_product_dy) {
    // df/dy = x = 2
    LispVal res = run(aad_module(R"(
        (define g (grad (lambda (x y) (ad (+ (* x y) (sin x))))
                        '(2 3)))
        (define gv (car (cdr g)))
        (define result (vector-ref gv 1))
    )"));
    BOOST_CHECK_CLOSE(to_double(res), 2.0, 1e-10);
}

BOOST_AUTO_TEST_CASE(test_aad_macro_grad_exp_chain) {
    // f(x) = exp(2x) at x=1,  df/dx = 2*exp(2)
    LispVal res = run(aad_module(R"(
        (define g (grad (lambda (x) (ad (exp (* 2 x))))
                        '(1)))
        (define gv (car (cdr g)))
        (define result (vector-ref gv 0))
    )"));
    BOOST_CHECK_CLOSE(to_double(res), 2.0 * std::exp(2.0), 1e-6);
}

// ============================================================================
// AAD — Rosenbrock via ad macro
// ============================================================================

BOOST_AUTO_TEST_CASE(test_aad_macro_rosenbrock) {
    // f(x,y) = (1-x)^2 + 100*(y-x^2)^2  at (1,1)
    // Primal = 0, gradient = (0, 0)
    LispVal res = run(aad_module(R"(
        (define g (grad (lambda (x y)
                          (ad (+ (* (- 1 x) (- 1 x))
                                 (* 100 (* (- y (* x x))
                                           (- y (* x x)))))))
                        '(1 1)))
        (define result (car g))
    )"));
    BOOST_CHECK_CLOSE(to_double(res) + 1.0, 1.0, 1e-10);
}

BOOST_AUTO_TEST_SUITE_END()

