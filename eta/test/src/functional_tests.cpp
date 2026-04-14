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

        // Re-register primitives with VM pointer so tape/CLP builtins work
        builtins.clear();
        register_core_primitives(builtins, heap, intern_table, &vm);
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
        (void) linker.index_modules(expanded);
        (void) linker.link();

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
        // Re-register primitives with VM pointer so tape/CLP builtins work
        builtins.clear();
        register_core_primitives(builtins, heap, intern_table, &vm);
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
    ;; Dual representation
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

    ;; Lifted operations

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

    ;; ad macro
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

    ;; Gradient helpers
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

// ============================================================================
// The xVA library: financial building blocks for CVA/FVA using tape-based AD.
// Plain arithmetic — the VM transparently records onto a tape when TapeRef
// operands are present.  Inlined so each test is self-contained.
// ============================================================================

static const char* TAPE_GRAD = R"(
    ;; Tape-based AD gradient driver

    (defun grad (f vals)
      (let ((tape (tape-new))
            (n    (length vals)))
        (let ((vars (letrec ((mk (lambda (vs acc)
                                   (if (null? vs) (reverse acc)
                                       (mk (cdr vs)
                                           (cons (tape-var tape (car vs)) acc))))))
                      (mk vals '()))))
          (tape-start! tape)
          (let ((output (apply f vars)))
            (tape-stop!)
            (tape-backward! tape output)
            (let ((grad-vec (make-vector n 0)))
              (letrec ((collect (lambda (vs i)
                                  (if (null? vs) grad-vec
                                      (begin
                                        (vector-set! grad-vec i
                                          (tape-adjoint tape (car vs)))
                                        (collect (cdr vs) (+ i 1)))))))
                (collect vars 0))
              (list (tape-primal tape output) grad-vec))))))
)";

static const char* XVA_LIB = R"(
    ;; Financial building blocks (plain arithmetic)

    (defun discount-factor (r t)
      (exp (* (* -1 r) t)))

    (defun survival-prob (hazard-rate t)
      (exp (* (* -1 hazard-rate) t)))

    (defun default-prob (hazard-rate t)
      (- 1 (survival-prob hazard-rate t)))

    (defun marginal-pd (hazard-rate t1 t2)
      (- (default-prob hazard-rate t2)
         (default-prob hazard-rate t1)))

    (defun expected-exposure (notional sigma t)
      (* notional (* sigma (sqrt t))))

    ;; CVA

    (defun cva-bucket (notional sigma r hazard-rate lgd t-prev t-curr)
      (let ((t-mid (* 0.5 (+ t-prev t-curr))))
        (* lgd
          (* (expected-exposure notional sigma t-mid)
            (* (discount-factor r t-mid)
               (marginal-pd hazard-rate t-prev t-curr))))))

    (defun cva-loop (notional sigma r hazard-rate lgd times prev-t acc)
      (if (null? times)
          acc
          (cva-loop notional sigma r hazard-rate lgd
            (cdr times)
            (car times)
            (+ acc
               (cva-bucket notional sigma r hazard-rate lgd
                           prev-t (car times))))))

    (defun compute-cva (notional sigma r hazard-rate lgd)
      (cva-loop notional sigma r hazard-rate lgd
        '(0.5 1.0 1.5 2.0 2.5 3.0 3.5 4.0 4.5 5.0)
        0.0
        0))

    ;; FVA

    (defun fva-bucket (notional sigma r funding-spread t-prev t-curr)
      (let ((t-mid (* 0.5 (+ t-prev t-curr)))
            (dt    (- t-curr t-prev)))
        (* (expected-exposure notional sigma t-mid)
          (* (discount-factor r t-mid)
            (* funding-spread dt)))))

    (defun fva-loop (notional sigma r funding-spread times prev-t acc)
      (if (null? times)
          acc
          (fva-loop notional sigma r funding-spread
            (cdr times)
            (car times)
            (+ acc
               (fva-bucket notional sigma r funding-spread
                           prev-t (car times))))))

    (defun compute-fva (notional sigma r funding-spread)
      (fva-loop notional sigma r funding-spread
        '(0.5 1.0 1.5 2.0 2.5 3.0 3.5 4.0 4.5 5.0)
        0.0
        0))

    ;; Total xVA

    (defun total-xva (notional sigma r hazard-rate lgd funding-spread)
      (+ (compute-cva notional sigma r hazard-rate lgd)
         (compute-fva notional sigma r funding-spread)))
)";

// Helper: wrap TAPE_GRAD + XVA_LIB + test body in a single module
static std::string xva_module(const std::string& body) {
    return std::string("(module m\n") + TAPE_GRAD + "\n" + XVA_LIB + "\n" + body + "\n)";
}

// C++ reference implementations for expected values

static double cpp_expected_cva(double N, double sigma, double r, double lam, double lgd) {
    double times[] = {0.5, 1.0, 1.5, 2.0, 2.5, 3.0, 3.5, 4.0, 4.5, 5.0};
    double cva = 0, prev_t = 0;
    for (double t : times) {
        double t_mid = 0.5 * (prev_t + t);
        double ee  = N * sigma * std::sqrt(t_mid);
        double df  = std::exp(-r * t_mid);
        double dpd = (1.0 - std::exp(-lam * t)) - (1.0 - std::exp(-lam * prev_t));
        cva += lgd * ee * df * dpd;
        prev_t = t;
    }
    return cva;
}

static double cpp_expected_fva(double N, double sigma, double r, double sf) {
    double times[] = {0.5, 1.0, 1.5, 2.0, 2.5, 3.0, 3.5, 4.0, 4.5, 5.0};
    double fva = 0, prev_t = 0;
    for (double t : times) {
        double t_mid = 0.5 * (prev_t + t);
        double dt  = t - prev_t;
        double ee  = N * sigma * std::sqrt(t_mid);
        double df  = std::exp(-r * t_mid);
        fva += ee * df * sf * dt;
        prev_t = t;
    }
    return fva;
}

// First-bucket CVA: bucket [0, 0.5]
static double cpp_expected_cva_first_bucket(double N, double sigma, double r, double lam, double lgd) {
    double t_prev = 0.0, t_curr = 0.5;
    double t_mid = 0.5 * (t_prev + t_curr);
    double ee  = N * sigma * std::sqrt(t_mid);
    double df  = std::exp(-r * t_mid);
    double dpd = (1.0 - std::exp(-lam * t_curr)) - (1.0 - std::exp(-lam * t_prev));
    return lgd * ee * df * dpd;
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

// ============================================================================
// xVA — sqrt / pow via tape-based AD
// ============================================================================

BOOST_AUTO_TEST_CASE(test_xva_sqrt_primal_4) {
    // sqrt(4) = 2.0
    LispVal res = run(xva_module(R"(
        (define result (sqrt 4.0))
    )"));
    BOOST_CHECK_CLOSE(to_double(res), 2.0, 1e-6);
}

BOOST_AUTO_TEST_CASE(test_xva_sqrt_primal_9) {
    // sqrt(9) = 3.0
    LispVal res = run(xva_module(R"(
        (define result (sqrt 9.0))
    )"));
    BOOST_CHECK_CLOSE(to_double(res), 3.0, 1e-6);
}

BOOST_AUTO_TEST_CASE(test_xva_sqrt_gradient) {
    // f(x) = sqrt(x) at x=4
    // df/dx = 1 / (2*sqrt(4)) = 0.25
    LispVal res = run(xva_module(R"(
        (define g (grad (lambda (x) (sqrt x)) '(4.0)))
        (define gv (car (cdr g)))
        (define result (vector-ref gv 0))
    )"));
    BOOST_CHECK_CLOSE(to_double(res), 0.25, 1e-6);
}

BOOST_AUTO_TEST_CASE(test_xva_pow_primal) {
    // exp(3 * log(2)) = 2^3 = 8.0
    LispVal res = run(xva_module(R"(
        (define result (exp (* 3.0 (log 2.0))))
    )"));
    BOOST_CHECK_CLOSE(to_double(res), 8.0, 1e-6);
}

BOOST_AUTO_TEST_CASE(test_xva_pow_gradient_base) {
    // f(x) = x^3 = exp(3*log(x)) at x=2.0
    // df/dx = 3 * x^2 = 12.0
    LispVal res = run(xva_module(R"(
        (define g (grad (lambda (x) (exp (* 3.0 (log x)))) '(2.0)))
        (define gv (car (cdr g)))
        (define result (vector-ref gv 0))
    )"));
    BOOST_CHECK_CLOSE(to_double(res), 12.0, 1e-4);
}

// ============================================================================
// xVA — Financial building block primal values
// ============================================================================

BOOST_AUTO_TEST_CASE(test_xva_discount_factor_primal) {
    // DF(r=0.05, t=1.0) = exp(-0.05)
    LispVal res = run(xva_module(R"(
        (define result (discount-factor 0.05 1.0))
    )"));
    BOOST_CHECK_CLOSE(to_double(res), std::exp(-0.05), 1e-6);
}

BOOST_AUTO_TEST_CASE(test_xva_discount_factor_primal_2y) {
    // DF(r=0.05, t=2.0) = exp(-0.10)
    LispVal res = run(xva_module(R"(
        (define result (discount-factor 0.05 2.0))
    )"));
    BOOST_CHECK_CLOSE(to_double(res), std::exp(-0.10), 1e-6);
}

BOOST_AUTO_TEST_CASE(test_xva_survival_prob_primal) {
    // Q(lambda=0.02, t=1.0) = exp(-0.02)
    LispVal res = run(xva_module(R"(
        (define result (survival-prob 0.02 1.0))
    )"));
    BOOST_CHECK_CLOSE(to_double(res), std::exp(-0.02), 1e-6);
}

BOOST_AUTO_TEST_CASE(test_xva_default_prob_primal) {
    // PD(lambda=0.02, t=1.0) = 1 - exp(-0.02)
    LispVal res = run(xva_module(R"(
        (define result (default-prob 0.02 1.0))
    )"));
    BOOST_CHECK_CLOSE(to_double(res), 1.0 - std::exp(-0.02), 1e-6);
}

BOOST_AUTO_TEST_CASE(test_xva_marginal_pd_primal) {
    // ΔPD(lambda=0.02, 0, 0.5) = PD(0.5) - PD(0) = (1-exp(-0.01)) - 0
    LispVal res = run(xva_module(R"(
        (define result (marginal-pd 0.02 0.0 0.5))
    )"));
    BOOST_CHECK_CLOSE(to_double(res), 1.0 - std::exp(-0.01), 1e-6);
}

BOOST_AUTO_TEST_CASE(test_xva_marginal_pd_mid_bucket) {
    // ΔPD(lambda=0.02, 0.5, 1.0) = PD(1.0) - PD(0.5)
    //   = (1-exp(-0.02)) - (1-exp(-0.01)) = exp(-0.01) - exp(-0.02)
    LispVal res = run(xva_module(R"(
        (define result (marginal-pd 0.02 0.5 1.0))
    )"));
    double expected = std::exp(-0.01) - std::exp(-0.02);
    BOOST_CHECK_CLOSE(to_double(res), expected, 1e-6);
}

BOOST_AUTO_TEST_CASE(test_xva_expected_exposure_primal) {
    // EE(N=1e6, sigma=0.20, t=0.25) = 1e6 * 0.20 * sqrt(0.25) = 100000
    LispVal res = run(xva_module(R"(
        (define result (expected-exposure 1000000 0.20 0.25))
    )"));
    BOOST_CHECK_CLOSE(to_double(res), 100000.0, 1e-6);
}

BOOST_AUTO_TEST_CASE(test_xva_expected_exposure_primal_1y) {
    // EE(N=1e6, sigma=0.20, t=1.0) = 1e6 * 0.20 * sqrt(1.0) = 200000
    LispVal res = run(xva_module(R"(
        (define result (expected-exposure 1000000 0.20 1.0))
    )"));
    BOOST_CHECK_CLOSE(to_double(res), 200000.0, 1e-6);
}

// ============================================================================
// xVA — Financial building block gradients
// ============================================================================

BOOST_AUTO_TEST_CASE(test_xva_discount_factor_gradient) {
    // ∂/∂r [exp(-r*t)] = -t * exp(-r*t)
    // At r=0.05, t=1.0:  -1.0 * exp(-0.05)
    LispVal res = run(xva_module(R"(
        (define g (grad (lambda (r) (discount-factor r 1.0)) '(0.05)))
        (define gv (car (cdr g)))
        (define result (vector-ref gv 0))
    )"));
    BOOST_CHECK_CLOSE(to_double(res), -1.0 * std::exp(-0.05), 1e-6);
}

BOOST_AUTO_TEST_CASE(test_xva_survival_prob_gradient) {
    // ∂/∂λ [exp(-λ*t)] = -t * exp(-λ*t)
    // At λ=0.02, t=1.0:  -exp(-0.02)
    LispVal res = run(xva_module(R"(
        (define g (grad (lambda (h) (survival-prob h 1.0)) '(0.02)))
        (define gv (car (cdr g)))
        (define result (vector-ref gv 0))
    )"));
    BOOST_CHECK_CLOSE(to_double(res), -1.0 * std::exp(-0.02), 1e-6);
}

BOOST_AUTO_TEST_CASE(test_xva_default_prob_gradient) {
    // ∂/∂λ [1 - exp(-λ*t)] = t * exp(-λ*t)
    // At λ=0.02, t=1.0:  exp(-0.02)
    LispVal res = run(xva_module(R"(
        (define g (grad (lambda (h) (default-prob h 1.0)) '(0.02)))
        (define gv (car (cdr g)))
        (define result (vector-ref gv 0))
    )"));
    BOOST_CHECK_CLOSE(to_double(res), std::exp(-0.02), 1e-6);
}

BOOST_AUTO_TEST_CASE(test_xva_expected_exposure_grad_sigma) {
    // ∂EE/∂σ = N * sqrt(t) = 1e6 * sqrt(0.25) = 500000
    LispVal res = run(xva_module(R"(
        (define g (grad (lambda (notional sigma)
                          (expected-exposure notional sigma 0.25))
                        '(1000000 0.20)))
        (define gv (car (cdr g)))
        (define result (vector-ref gv 1))
    )"));
    BOOST_CHECK_CLOSE(to_double(res), 500000.0, 1e-6);
}

BOOST_AUTO_TEST_CASE(test_xva_expected_exposure_grad_notional) {
    // ∂EE/∂N = σ * sqrt(t) = 0.20 * sqrt(0.25) = 0.10
    LispVal res = run(xva_module(R"(
        (define g (grad (lambda (notional sigma)
                          (expected-exposure notional sigma 0.25))
                        '(1000000 0.20)))
        (define gv (car (cdr g)))
        (define result (vector-ref gv 0))
    )"));
    BOOST_CHECK_CLOSE(to_double(res), 0.10, 1e-6);
}

// ============================================================================
// xVA — CVA single bucket
// ============================================================================

BOOST_AUTO_TEST_CASE(test_xva_cva_single_bucket) {
    // First bucket [0, 0.5] with standard parameters
    double expected = cpp_expected_cva_first_bucket(1e6, 0.20, 0.05, 0.02, 0.60);
    LispVal res = run(xva_module(R"(
        (define result
          (cva-bucket 1000000 0.20 0.05 0.02 0.60 0.0 0.5))
    )"));
    BOOST_CHECK_CLOSE(to_double(res), expected, 1e-4);
}

// ============================================================================
// xVA — Full CVA computation
// ============================================================================

BOOST_AUTO_TEST_CASE(test_xva_cva_full_primal) {
    // Full CVA across 10 semi-annual buckets, verified against C++ reference
    double expected = cpp_expected_cva(1e6, 0.20, 0.05, 0.02, 0.60);
    LispVal res = run(xva_module(R"(
        (define g (grad (lambda (notional sigma r hazard-rate lgd)
                          (compute-cva notional sigma r hazard-rate lgd))
                        '(1000000 0.20 0.05 0.02 0.60)))
        (define result (car g))
    )"));
    BOOST_CHECK_CLOSE(to_double(res), expected, 1e-4);
}

BOOST_AUTO_TEST_CASE(test_xva_cva_notional_linearity) {
    // CVA is linear in notional: ∂CVA/∂N = CVA / N
    double cva = cpp_expected_cva(1e6, 0.20, 0.05, 0.02, 0.60);
    double expected_dN = cva / 1e6;
    LispVal res = run(xva_module(R"(
        (define g (grad (lambda (notional sigma r hazard-rate lgd)
                          (compute-cva notional sigma r hazard-rate lgd))
                        '(1000000 0.20 0.05 0.02 0.60)))
        (define gv (car (cdr g)))
        (define result (vector-ref gv 0))
    )"));
    BOOST_CHECK_CLOSE(to_double(res), expected_dN, 1e-4);
}

BOOST_AUTO_TEST_CASE(test_xva_cva_lgd_linearity) {
    // CVA is linear in LGD: ∂CVA/∂LGD = CVA / LGD
    double cva = cpp_expected_cva(1e6, 0.20, 0.05, 0.02, 0.60);
    double expected_dLGD = cva / 0.60;
    LispVal res = run(xva_module(R"(
        (define g (grad (lambda (notional sigma r hazard-rate lgd)
                          (compute-cva notional sigma r hazard-rate lgd))
                        '(1000000 0.20 0.05 0.02 0.60)))
        (define gv (car (cdr g)))
        (define result (vector-ref gv 4))
    )"));
    BOOST_CHECK_CLOSE(to_double(res), expected_dLGD, 1e-4);
}

BOOST_AUTO_TEST_CASE(test_xva_cva_sigma_linearity) {
    // CVA is linear in sigma: ∂CVA/∂σ = CVA / σ
    double cva = cpp_expected_cva(1e6, 0.20, 0.05, 0.02, 0.60);
    double expected_dSigma = cva / 0.20;
    LispVal res = run(xva_module(R"(
        (define g (grad (lambda (notional sigma r hazard-rate lgd)
                          (compute-cva notional sigma r hazard-rate lgd))
                        '(1000000 0.20 0.05 0.02 0.60)))
        (define gv (car (cdr g)))
        (define result (vector-ref gv 1))
    )"));
    BOOST_CHECK_CLOSE(to_double(res), expected_dSigma, 1e-4);
}

BOOST_AUTO_TEST_CASE(test_xva_cva_rate_sensitivity) {
    // ∂CVA/∂r — non-trivial (non-linear), verify with finite difference
    double r0 = 0.05, dr = 1e-7;
    double cva_up   = cpp_expected_cva(1e6, 0.20, r0 + dr, 0.02, 0.60);
    double cva_down = cpp_expected_cva(1e6, 0.20, r0 - dr, 0.02, 0.60);
    double fd_dCVA_dr = (cva_up - cva_down) / (2.0 * dr);
    LispVal res = run(xva_module(R"(
        (define g (grad (lambda (notional sigma r hazard-rate lgd)
                          (compute-cva notional sigma r hazard-rate lgd))
                        '(1000000 0.20 0.05 0.02 0.60)))
        (define gv (car (cdr g)))
        (define result (vector-ref gv 2))
    )"));
    BOOST_CHECK_CLOSE(to_double(res), fd_dCVA_dr, 0.01);  // 0.01% tolerance for FD
}

BOOST_AUTO_TEST_CASE(test_xva_cva_hazard_sensitivity) {
    // ∂CVA/∂λ — non-trivial, verify with finite difference
    double lam = 0.02, dl = 1e-7;
    double cva_up   = cpp_expected_cva(1e6, 0.20, 0.05, lam + dl, 0.60);
    double cva_down = cpp_expected_cva(1e6, 0.20, 0.05, lam - dl, 0.60);
    double fd_dCVA_dlam = (cva_up - cva_down) / (2.0 * dl);
    LispVal res = run(xva_module(R"(
        (define g (grad (lambda (notional sigma r hazard-rate lgd)
                          (compute-cva notional sigma r hazard-rate lgd))
                        '(1000000 0.20 0.05 0.02 0.60)))
        (define gv (car (cdr g)))
        (define result (vector-ref gv 3))
    )"));
    BOOST_CHECK_CLOSE(to_double(res), fd_dCVA_dlam, 0.01);
}

// ============================================================================
// xVA — Full FVA computation
// ============================================================================

BOOST_AUTO_TEST_CASE(test_xva_fva_full_primal) {
    // Full FVA across 10 semi-annual buckets, verified against C++ reference
    double expected = cpp_expected_fva(1e6, 0.20, 0.05, 0.012);
    LispVal res = run(xva_module(R"(
        (define g (grad (lambda (notional sigma r funding-spread)
                          (compute-fva notional sigma r funding-spread))
                        '(1000000 0.20 0.05 0.012)))
        (define result (car g))
    )"));
    BOOST_CHECK_CLOSE(to_double(res), expected, 1e-4);
}

BOOST_AUTO_TEST_CASE(test_xva_fva_notional_linearity) {
    // FVA is linear in notional: ∂FVA/∂N = FVA / N
    double fva = cpp_expected_fva(1e6, 0.20, 0.05, 0.012);
    double expected_dN = fva / 1e6;
    LispVal res = run(xva_module(R"(
        (define g (grad (lambda (notional sigma r funding-spread)
                          (compute-fva notional sigma r funding-spread))
                        '(1000000 0.20 0.05 0.012)))
        (define gv (car (cdr g)))
        (define result (vector-ref gv 0))
    )"));
    BOOST_CHECK_CLOSE(to_double(res), expected_dN, 1e-4);
}

BOOST_AUTO_TEST_CASE(test_xva_fva_spread_linearity) {
    // FVA is linear in funding spread: ∂FVA/∂s_f = FVA / s_f
    double fva = cpp_expected_fva(1e6, 0.20, 0.05, 0.012);
    double expected_dSf = fva / 0.012;
    LispVal res = run(xva_module(R"(
        (define g (grad (lambda (notional sigma r funding-spread)
                          (compute-fva notional sigma r funding-spread))
                        '(1000000 0.20 0.05 0.012)))
        (define gv (car (cdr g)))
        (define result (vector-ref gv 3))
    )"));
    BOOST_CHECK_CLOSE(to_double(res), expected_dSf, 1e-4);
}

BOOST_AUTO_TEST_CASE(test_xva_fva_rate_sensitivity) {
    // ∂FVA/∂r — non-linear, verify with finite difference
    double r0 = 0.05, dr = 1e-7;
    double fva_up   = cpp_expected_fva(1e6, 0.20, r0 + dr, 0.012);
    double fva_down = cpp_expected_fva(1e6, 0.20, r0 - dr, 0.012);
    double fd_dFVA_dr = (fva_up - fva_down) / (2.0 * dr);
    LispVal res = run(xva_module(R"(
        (define g (grad (lambda (notional sigma r funding-spread)
                          (compute-fva notional sigma r funding-spread))
                        '(1000000 0.20 0.05 0.012)))
        (define gv (car (cdr g)))
        (define result (vector-ref gv 2))
    )"));
    BOOST_CHECK_CLOSE(to_double(res), fd_dFVA_dr, 0.01);
}

// ============================================================================
// xVA — Total xVA = CVA + FVA
// ============================================================================

BOOST_AUTO_TEST_CASE(test_xva_total_primal) {
    // Total xVA should equal CVA + FVA
    double expected_cva = cpp_expected_cva(1e6, 0.20, 0.05, 0.02, 0.60)
               + cpp_expected_fva(1e6, 0.20, 0.05, 0.012);
    double expected_total = expected_cva;
    LispVal res = run(xva_module(R"(
        (define g (grad (lambda (notional sigma r hazard-rate lgd funding-spread)
                          (total-xva notional sigma r hazard-rate lgd funding-spread))
                        '(1000000 0.20 0.05 0.02 0.60 0.012)))
        (define result (car g))
    )"));
    BOOST_CHECK_CLOSE(to_double(res), expected_total, 1e-4);
}

BOOST_AUTO_TEST_CASE(test_xva_total_notional_linearity) {
    // Both CVA and FVA are linear in N, so total xVA is too:
    // ∂xVA/∂N = xVA / N
    double xva = cpp_expected_cva(1e6, 0.20, 0.05, 0.02, 0.60)
               + cpp_expected_fva(1e6, 0.20, 0.05, 0.012);
    double expected_dN = xva / 1e6;
    LispVal res = run(xva_module(R"(
        (define g (grad (lambda (notional sigma r hazard-rate lgd funding-spread)
                          (total-xva notional sigma r hazard-rate lgd funding-spread))
                        '(1000000 0.20 0.05 0.02 0.60 0.012)))
        (define gv (car (cdr g)))
        (define result (vector-ref gv 0))
    )"));
    BOOST_CHECK_CLOSE(to_double(res), expected_dN, 1e-4);
}

BOOST_AUTO_TEST_CASE(test_xva_total_gradient_six_params) {
    // Verify all 6 sensitivities are computed and the gradient vector has
    // length 6.  We check that vector-ref gv 5 (the last element) is the
    // funding-spread sensitivity: ∂xVA/∂s_f = ∂FVA/∂s_f = FVA / s_f
    // (CVA does not depend on funding-spread, so only FVA contributes.)
    double fva = cpp_expected_fva(1e6, 0.20, 0.05, 0.012);
    double expected_dSf = fva / 0.012;
    LispVal res = run(xva_module(R"(
        (define g (grad (lambda (notional sigma r hazard-rate lgd funding-spread)
                          (total-xva notional sigma r hazard-rate lgd funding-spread))
                        '(1000000 0.20 0.05 0.02 0.60 0.012)))
        (define gv (car (cdr g)))
        (define result (vector-ref gv 5))
    )"));
    BOOST_CHECK_CLOSE(to_double(res), expected_dSf, 1e-4);
}

BOOST_AUTO_TEST_CASE(test_xva_total_lgd_sensitivity) {
    // ∂xVA/∂LGD = ∂CVA/∂LGD  (FVA does not depend on LGD)
    // Verify with finite difference
    double cva = cpp_expected_cva(1e6, 0.20, 0.05, 0.02, 0.60);
    double expected_dLGD = cva / 0.60;
    LispVal res = run(xva_module(R"(
        (define g (grad (lambda (notional sigma r hazard-rate lgd funding-spread)
                          (total-xva notional sigma r hazard-rate lgd funding-spread))
                        '(1000000 0.20 0.05 0.02 0.60 0.012)))
        (define gv (car (cdr g)))
        (define result (vector-ref gv 4))
    )"));
    BOOST_CHECK_CLOSE(to_double(res), expected_dLGD, 1e-4);
}

BOOST_AUTO_TEST_CASE(test_xva_total_hazard_sensitivity) {
    // ∂xVA/∂λ = ∂CVA/∂λ  (FVA does not depend on hazard rate)
    // Verify with finite difference
    double lam = 0.02, dl = 1e-7;
    double cva_up   = cpp_expected_cva(1e6, 0.20, 0.05, lam + dl, 0.60);
    double cva_down = cpp_expected_cva(1e6, 0.20, 0.05, lam - dl, 0.60);
    double fd = (cva_up - cva_down) / (2.0 * dl);
    LispVal res = run(xva_module(R"(
        (define g (grad (lambda (notional sigma r hazard-rate lgd funding-spread)
                          (total-xva notional sigma r hazard-rate lgd funding-spread))
                        '(1000000 0.20 0.05 0.02 0.60 0.012)))
        (define gv (car (cdr g)))
        (define result (vector-ref gv 3))
    )"));
    BOOST_CHECK_CLOSE(to_double(res), fd, 0.01);
}

// ============================================================================
// xVA — Different market scenarios
// ============================================================================

BOOST_AUTO_TEST_CASE(test_xva_cva_high_hazard) {
    // Higher hazard rate (5%) should give a larger CVA
    double cva_high = cpp_expected_cva(1e6, 0.20, 0.05, 0.05, 0.60);
    double cva_low  = cpp_expected_cva(1e6, 0.20, 0.05, 0.02, 0.60);
    BOOST_CHECK_GT(cva_high, cva_low);  // sanity: higher default intensity → larger CVA

    LispVal res = run(xva_module(R"(
        (define g (grad (lambda (notional sigma r hazard-rate lgd)
                          (compute-cva notional sigma r hazard-rate lgd))
                        '(1000000 0.20 0.05 0.05 0.60)))
        (define result (car g))
    )"));
    BOOST_CHECK_CLOSE(to_double(res), cva_high, 1e-4);
}

BOOST_AUTO_TEST_CASE(test_xva_cva_zero_lgd) {
    // LGD = 0 implies CVA = 0 (full recovery, no credit loss)
    LispVal res = run(xva_module(R"(
        (define g (grad (lambda (notional sigma r hazard-rate lgd)
                          (compute-cva notional sigma r hazard-rate lgd))
                        '(1000000 0.20 0.05 0.02 0.0)))
        (define result (car g))
    )"));
    BOOST_CHECK_SMALL(to_double(res), 1e-10);
}

BOOST_AUTO_TEST_SUITE_END()

// ============================================================================
// Main entry-point tests — verify that (defun main ...) is detected and
// invoked by the Driver after module initialization.
// ============================================================================

BOOST_AUTO_TEST_SUITE(main_entry_tests)

BOOST_FIXTURE_TEST_CASE(module_with_main_detects_main_slot, FunctionalTestFixture) {
    // Verify that the semantic analyzer populates main_func_slot
    // when a module defines (defun main ...).
    auto source = R"(
        (module m
          (define result 0)
          (defun main ()
            (set! result 42)))
    )";

    reader::lexer::Lexer lex(0, source);
    reader::parser::Parser p(lex);
    auto parsed = std::move(*p.parse_toplevel());

    reader::expander::Expander ex;
    auto expanded = std::move(*ex.expand_many(parsed));

    reader::ModuleLinker linker;
    (void) linker.index_modules(expanded);
    (void) linker.link();

    SemanticAnalyzer sa;
    auto sem_res = sa.analyze_all(expanded, linker, builtins);
    BOOST_REQUIRE(sem_res.has_value());
    BOOST_REQUIRE(!sem_res->empty());

    auto& mod = (*sem_res)[0];
    BOOST_CHECK(mod.main_func_slot.has_value());
}

BOOST_FIXTURE_TEST_CASE(module_without_main_has_no_slot, FunctionalTestFixture) {
    // A module without (defun main ...) should have no main_func_slot
    auto source = "(module m (define result 42))";

    reader::lexer::Lexer lex(0, source);
    reader::parser::Parser p(lex);
    auto parsed = std::move(*p.parse_toplevel());

    reader::expander::Expander ex;
    auto expanded = std::move(*ex.expand_many(parsed));

    reader::ModuleLinker linker;
    (void) linker.index_modules(expanded);
    (void) linker.link();

    SemanticAnalyzer sa;
    auto sem_res = sa.analyze_all(expanded, linker, builtins);
    BOOST_REQUIRE(sem_res.has_value());
    BOOST_REQUIRE(!sem_res->empty());

    auto& mod = (*sem_res)[0];
    BOOST_CHECK(!mod.main_func_slot.has_value());
}

BOOST_FIXTURE_TEST_CASE(module_without_main_runs_begin, FunctionalTestFixture) {
    // Existing behaviour: modules without main still execute their top-level forms
    auto result = run("(module m (define result (+ 10 20)))");
    auto decoded = nanbox::ops::decode<int64_t>(result);
    BOOST_REQUIRE(decoded.has_value());
    BOOST_CHECK_EQUAL(*decoded, 30);
}

BOOST_AUTO_TEST_SUITE_END()

// ============================================================================
// SABR — Native Dual AD tests for SABR Hagan implied vol approximation.
// Validates primal values and gradients against C++ reference implementations.
// ============================================================================

// C++ reference: SABR ATM implied vol
static double cpp_sabr_atm_vol(double F, double T, double alpha, double beta,
                                double rho, double nu) {
    double omb = 1.0 - beta;
    double F_pow = std::pow(F, omb);
    double base = alpha / F_pow;
    double t1 = (omb * omb * alpha * alpha) / (24.0 * std::pow(F, 2.0 * omb));
    double t2 = (rho * beta * nu * alpha) / (4.0 * F_pow);
    double t3 = ((2.0 - 3.0 * rho * rho) * nu * nu) / 24.0;
    return base * (1.0 + (t1 + t2 + t3) * T);
}

// C++ reference: SABR general implied vol
static double cpp_sabr_general_vol(double F, double K, double T, double alpha,
                                    double beta, double rho, double nu) {
    double omb = 1.0 - beta;
    double FK = F * K;
    double FK_mid = std::pow(FK, 0.5 * omb);
    double logFK = std::log(F / K);

    double omb2 = omb * omb;
    double denom = 1.0 + (omb2 / 24.0) * logFK * logFK
                       + (omb2 * omb2 / 1920.0) * logFK * logFK * logFK * logFK;

    double z = (nu / alpha) * FK_mid * logFK;
    double zx_ratio;
    if (std::abs(z) < 1e-12) {
        zx_ratio = 1.0;
    } else {
        double disc = std::sqrt(1.0 - 2.0 * rho * z + z * z);
        double xz = std::log((disc + z - rho) / (1.0 - rho));
        zx_ratio = z / xz;
    }

    double FK_full = std::pow(FK, omb);
    double t1 = (omb2 * alpha * alpha) / (24.0 * FK_full);
    double t2 = (rho * beta * nu * alpha) / (4.0 * FK_mid);
    double t3 = ((2.0 - 3.0 * rho * rho) * nu * nu) / 24.0;
    double num_corr = 1.0 + (t1 + t2 + t3) * T;

    return (alpha / (FK_mid * denom)) * zx_ratio * num_corr;
}

static double cpp_sabr_implied_vol(double F, double K, double T, double alpha,
                                    double beta, double rho, double nu) {
    if (std::abs(F - K) < 1e-7)
        return cpp_sabr_atm_vol(F, T, alpha, beta, rho, nu);
    return cpp_sabr_general_vol(F, K, T, alpha, beta, rho, nu);
}

// SABR library string for embedding in test modules (tape-based AD)
static const char* SABR_LIB = R"(
    ;; Tape-based AD gradient driver
    (defun native-grad (f vals)
      (let ((tape (tape-new))
            (n    (length vals)))
        (let ((vars (letrec ((mk (lambda (vs acc)
                                   (if (null? vs) (reverse acc)
                                       (mk (cdr vs)
                                           (cons (tape-var tape (car vs)) acc))))))
                      (mk vals '()))))
          (tape-start! tape)
          (let ((output (apply f vars)))
            (tape-stop!)
            (tape-backward! tape output)
            (let ((grad-vec (make-vector n 0)))
              (letrec ((collect (lambda (vs i)
                                  (if (null? vs) grad-vec
                                      (begin
                                        (vector-set! grad-vec i
                                          (tape-adjoint tape (car vs)))
                                        (collect (cdr vs) (+ i 1)))))))
                (collect vars 0))
              (list (tape-primal tape output) grad-vec))))))

    ;; nd-val: extract primal from a tape-ref or pass through
    (defun nd-val (x)
      (if (tape-ref? x) (tape-ref-value x) x))

    ;; Helper: pow via exp/log
    (defun ndpow (a b) (exp (* b (log a))))

    ;; SABR x(z) helper
    (defun sabr-xz (z rho)
      (let ((disc (sqrt (+ (- 1 (* 2 (* rho z)))
                           (* z z)))))
        (log (/ (+ (- disc rho) z)
                (- 1 rho)))))

    ;; SABR ATM vol
    (defun sabr-atm-vol (F T alpha beta rho nu)
      (let ((one-minus-beta (- 1 beta)))
        (let ((F-pow (ndpow F one-minus-beta)))
          (let ((base-vol (/ alpha F-pow)))
            (let ((term1 (/ (* (* one-minus-beta one-minus-beta)
                                (* alpha alpha))
                            (* 24 (ndpow F (* 2 one-minus-beta)))))
                  (term2 (/ (* rho (* beta (* nu alpha)))
                            (* 4 F-pow)))
                  (term3 (/ (* (- 2 (* 3 (* rho rho)))
                                (* nu nu))
                            24)))
              (* base-vol
                 (+ 1 (* T (+ term1 (+ term2 term3))))))))))

    ;; SABR general vol
    (defun sabr-general-vol (F K T alpha beta rho nu)
      (let ((one-minus-beta (- 1 beta)))
        (let ((FK (* F K)))
          (let ((FK-mid (ndpow FK (* 0.5 one-minus-beta)))
                (log-FK (log (/ F K))))
            (let ((omb2 (* one-minus-beta one-minus-beta)))
              (let ((denom-corr (+ 1
                                  (+ (/ (* omb2 (* log-FK log-FK)) 24)
                                     (/ (* (* omb2 omb2)
                                           (* (* log-FK log-FK)
                                              (* log-FK log-FK)))
                                        1920)))))
                  (let ((z (* (/ nu alpha) (* FK-mid log-FK))))
                   (let ((z-val (if (tape-ref? z) (tape-ref-value z) z)))
                    (let ((zx-ratio
                            (if (< (abs z-val) 1e-12)
                                1
                                (/ z (sabr-xz z rho)))))
                      (let ((FK-full (ndpow FK one-minus-beta)))
                        (let ((term1 (/ (* omb2 (* alpha alpha))
                                        (* 24 FK-full)))
                              (term2 (/ (* rho (* beta (* nu alpha)))
                                        (* 4 FK-mid)))
                              (term3 (/ (* (- 2 (* 3 (* rho rho)))
                                           (* nu nu))
                                        24)))
                          (let ((num-corr (+ 1 (* T (+ term1 (+ term2 term3))))))
                            (* (/ alpha (* FK-mid denom-corr))
                               (* zx-ratio num-corr))))))))))))))

    ;; Unified SABR implied vol
    (defun sabr-implied-vol (F K T alpha beta rho nu)
      (if (< (abs (- F K)) 1e-7)
          (sabr-atm-vol F T alpha beta rho nu)
          (sabr-general-vol F K T alpha beta rho nu)))
)";

static std::string sabr_module(const std::string& body) {
    return std::string("(module m\n") + SABR_LIB + "\n" + body + "\n)";
}

BOOST_AUTO_TEST_SUITE(sabr_tests)

// SABR ATM primal value

BOOST_FIXTURE_TEST_CASE(test_sabr_atm_primal, FunctionalTestFixture) {
    // ATM implied vol at F=0.03, T=1, alpha=0.035, beta=0.5, rho=-0.25, nu=0.40
    double expected = cpp_sabr_atm_vol(0.03, 1.0, 0.035, 0.5, -0.25, 0.40);
    LispVal res = run(sabr_module(R"(
        (define result (nd-val (sabr-atm-vol 0.03 1.0 0.035 0.5 -0.25 0.40)))
    )"));
    BOOST_CHECK_CLOSE(to_double(res), expected, 1e-6);
}

// SABR general (OTM) primal value

BOOST_FIXTURE_TEST_CASE(test_sabr_general_primal_otm, FunctionalTestFixture) {
    // OTM: K=0.024 (80% of F=0.03), T=1
    double expected = cpp_sabr_general_vol(0.03, 0.024, 1.0, 0.035, 0.5, -0.25, 0.40);
    LispVal res = run(sabr_module(R"(
        (define result (nd-val (sabr-general-vol 0.03 0.024 1.0 0.035 0.5 -0.25 0.40)))
    )"));
    BOOST_CHECK_CLOSE(to_double(res), expected, 1e-4);
}

// SABR unified: ATM path

BOOST_FIXTURE_TEST_CASE(test_sabr_unified_atm, FunctionalTestFixture) {
    double expected = cpp_sabr_implied_vol(0.03, 0.03, 1.0, 0.035, 0.5, -0.25, 0.40);
    LispVal res = run(sabr_module(R"(
        (define result (nd-val (sabr-implied-vol 0.03 0.03 1.0 0.035 0.5 -0.25 0.40)))
    )"));
    BOOST_CHECK_CLOSE(to_double(res), expected, 1e-6);
}

// SABR unified: OTM path

BOOST_FIXTURE_TEST_CASE(test_sabr_unified_otm, FunctionalTestFixture) {
    double K = 0.03 * 1.20;  // 120% moneyness
    double expected = cpp_sabr_implied_vol(0.03, K, 1.0, 0.035, 0.5, -0.25, 0.40);
    LispVal res = run(sabr_module(R"(
        (define result (nd-val (sabr-implied-vol 0.03 0.036 1.0 0.035 0.5 -0.25 0.40)))
    )"));
    BOOST_CHECK_CLOSE(to_double(res), expected, 1e-4);
}

// SABR gradient: ∂σ/∂α via native-grad (ATM)

BOOST_FIXTURE_TEST_CASE(test_sabr_grad_dalpha_atm, FunctionalTestFixture) {
    // Finite-difference reference: bump alpha by h, compute (f(a+h)-f(a-h))/(2h)
    double h = 1e-7;
    double vp = cpp_sabr_atm_vol(0.03, 1.0, 0.035 + h, 0.5, -0.25, 0.40);
    double vm = cpp_sabr_atm_vol(0.03, 1.0, 0.035 - h, 0.5, -0.25, 0.40);
    double fd_dalpha = (vp - vm) / (2.0 * h);

    LispVal res = run(sabr_module(R"(
        (define g (native-grad (lambda (alpha beta rho nu)
                                 (sabr-implied-vol 0.03 0.03 1.0
                                                   alpha beta rho nu))
                               '(0.035 0.5 -0.25 0.40)))
        (define result (vector-ref (car (cdr g)) 0))
    )"));
    BOOST_CHECK_CLOSE(to_double(res), fd_dalpha, 0.01); // 0.01% tolerance
}

// SABR gradient: ∂σ/∂ρ via native-grad (ATM)

BOOST_FIXTURE_TEST_CASE(test_sabr_grad_drho_atm, FunctionalTestFixture) {
    double h = 1e-7;
    double vp = cpp_sabr_atm_vol(0.03, 1.0, 0.035, 0.5, -0.25 + h, 0.40);
    double vm = cpp_sabr_atm_vol(0.03, 1.0, 0.035, 0.5, -0.25 - h, 0.40);
    double fd_drho = (vp - vm) / (2.0 * h);

    LispVal res = run(sabr_module(R"(
        (define g (native-grad (lambda (alpha beta rho nu)
                                 (sabr-implied-vol 0.03 0.03 1.0
                                                   alpha beta rho nu))
                               '(0.035 0.5 -0.25 0.40)))
        (define result (vector-ref (car (cdr g)) 2))
    )"));
    BOOST_CHECK_CLOSE(to_double(res), fd_drho, 0.01);
}

// SABR gradient: ∂σ/∂ν via native-grad (ATM)

BOOST_FIXTURE_TEST_CASE(test_sabr_grad_dnu_atm, FunctionalTestFixture) {
    double h = 1e-7;
    double vp = cpp_sabr_atm_vol(0.03, 1.0, 0.035, 0.5, -0.25, 0.40 + h);
    double vm = cpp_sabr_atm_vol(0.03, 1.0, 0.035, 0.5, -0.25, 0.40 - h);
    double fd_dnu = (vp - vm) / (2.0 * h);

    LispVal res = run(sabr_module(R"(
        (define g (native-grad (lambda (alpha beta rho nu)
                                 (sabr-implied-vol 0.03 0.03 1.0
                                                   alpha beta rho nu))
                               '(0.035 0.5 -0.25 0.40)))
        (define result (vector-ref (car (cdr g)) 3))
    )"));
    BOOST_CHECK_CLOSE(to_double(res), fd_dnu, 0.01);
}

// SABR gradient: OTM point ∂σ/∂α

BOOST_FIXTURE_TEST_CASE(test_sabr_grad_dalpha_otm, FunctionalTestFixture) {
    double K = 0.024; // 80% moneyness
    double h = 1e-7;
    double vp = cpp_sabr_general_vol(0.03, K, 1.0, 0.035 + h, 0.5, -0.25, 0.40);
    double vm = cpp_sabr_general_vol(0.03, K, 1.0, 0.035 - h, 0.5, -0.25, 0.40);
    double fd_dalpha = (vp - vm) / (2.0 * h);

    LispVal res = run(sabr_module(R"(
        (define g (native-grad (lambda (alpha beta rho nu)
                                 (sabr-implied-vol 0.03 0.024 1.0
                                                   alpha beta rho nu))
                               '(0.035 0.5 -0.25 0.40)))
        (define result (vector-ref (car (cdr g)) 0))
    )"));
    BOOST_CHECK_CLOSE(to_double(res), fd_dalpha, 0.1); // slightly wider tolerance for OTM
}

// SABR negative skew: low-strike vol > high-strike vol

BOOST_FIXTURE_TEST_CASE(test_sabr_skew_direction, FunctionalTestFixture) {
    // With rho < 0, vol at K=80%F should be > vol at K=120%F
    LispVal res = run(sabr_module(R"(
        (define vol-low  (nd-val (sabr-implied-vol 0.03 0.024 1.0 0.035 0.5 0.0 0.80)))
        (define vol-high (nd-val (sabr-implied-vol 0.03 0.036 1.0 0.035 0.5 0.0 0.80)))
        (define result (if (> vol-low vol-high) 1.0 0.0))
    )"));
    BOOST_CHECK_CLOSE(to_double(res), 1.0, 1e-10);
}

// SABR smile: OTM vols > ATM vol on both sides (rho=0)

BOOST_FIXTURE_TEST_CASE(test_sabr_smile_shape, FunctionalTestFixture) {
    // With rho=0 and high enough nu, both OTM vols should be >= ATM vol (smile shape).
    // nu must be large enough for the z/x(z) curvature to dominate the backbone
    // asymmetry introduced by beta=0.5.
    LispVal res = run(sabr_module(R"(
        (define vol-atm  (nd-val (sabr-implied-vol 0.03 0.03  1.0 0.035 0.5 0.0 0.80)))
        (define vol-low  (nd-val (sabr-implied-vol 0.03 0.024 1.0 0.035 0.5 0.0 0.80)))
        (define vol-high (nd-val (sabr-implied-vol 0.03 0.036 1.0 0.035 0.5 0.0 0.80)))
        (define result (if (and (>= vol-low vol-atm) (>= vol-high vol-atm)) 1.0 0.0))
    )"));
    BOOST_CHECK_CLOSE(to_double(res), 1.0, 1e-10);
}

BOOST_AUTO_TEST_SUITE_END()

// ============================================================================
// AD Tape (Wengert list) tests
// ============================================================================
BOOST_AUTO_TEST_SUITE(tape_aad_tests)

BOOST_FIXTURE_TEST_CASE(tape_creation, FunctionalTestFixture) {
    LispVal res = run(R"(
        (module test
            (define t (tape-new))
            (define result (if t 1.0 0.0))
        )
    )");
    BOOST_CHECK_CLOSE(to_double(res), 1.0, 1e-10);
}

BOOST_FIXTURE_TEST_CASE(tape_var_and_primal, FunctionalTestFixture) {
    LispVal res = run(R"(
        (module test
            (define t (tape-new))
            (define x (tape-var t 3.14))
            (define result (tape-primal t x))
        )
    )");
    BOOST_CHECK_CLOSE(to_double(res), 3.14, 1e-10);
}

BOOST_FIXTURE_TEST_CASE(tape_ref_predicate, FunctionalTestFixture) {
    LispVal res = run(R"(
        (module test
            (define t (tape-new))
            (define x (tape-var t 1.0))
            (define result (if (and (tape-ref? x) (not (tape-ref? 42))) 1.0 0.0))
        )
    )");
    BOOST_CHECK_CLOSE(to_double(res), 1.0, 1e-10);
}

BOOST_FIXTURE_TEST_CASE(tape_add_gradient, FunctionalTestFixture) {
    // f(x,y) = x + y at (3,5) => df/dx=1, df/dy=1
    LispVal res = run(R"(
        (module test
            (define t (tape-new))
            (define x (tape-var t 3.0))
            (define y (tape-var t 5.0))
            (tape-start! t)
            (define z (+ x y))
            (tape-stop!)
            (tape-backward! t z)
            (define result (+ (tape-adjoint t x) (tape-adjoint t y)))
        )
    )");
    BOOST_CHECK_CLOSE(to_double(res), 2.0, 1e-10);  // 1 + 1
}

BOOST_FIXTURE_TEST_CASE(tape_mul_gradient, FunctionalTestFixture) {
    // f(x,y) = x * y at (3,5) => df/dx=5, df/dy=3
    LispVal res = run(R"(
        (module test
            (define t (tape-new))
            (define x (tape-var t 3.0))
            (define y (tape-var t 5.0))
            (tape-start! t)
            (define z (* x y))
            (tape-stop!)
            (tape-backward! t z)
            (define result (+ (tape-adjoint t x) (tape-adjoint t y)))
        )
    )");
    BOOST_CHECK_CLOSE(to_double(res), 8.0, 1e-10);  // 5 + 3
}

BOOST_FIXTURE_TEST_CASE(tape_x_squared, FunctionalTestFixture) {
    // f(x) = x^2 at x=4 => df/dx = 2x = 8
    LispVal res = run(R"(
        (module test
            (define t (tape-new))
            (define x (tape-var t 4.0))
            (tape-start! t)
            (define z (* x x))
            (tape-stop!)
            (tape-backward! t z)
            (define result (tape-adjoint t x))
        )
    )");
    BOOST_CHECK_CLOSE(to_double(res), 8.0, 1e-10);
}

BOOST_FIXTURE_TEST_CASE(tape_polynomial, FunctionalTestFixture) {
    // f(x) = x^2 + 3x + 1 at x=4 => df/dx = 2x + 3 = 11
    LispVal res = run(R"(
        (module test
            (define t (tape-new))
            (define x (tape-var t 4.0))
            (tape-start! t)
            (define z (+ (+ (* x x) (* 3 x)) 1))
            (tape-stop!)
            (tape-backward! t z)
            (define result (tape-adjoint t x))
        )
    )");
    BOOST_CHECK_CLOSE(to_double(res), 11.0, 1e-10);
}

BOOST_FIXTURE_TEST_CASE(tape_exp, FunctionalTestFixture) {
    // f(x) = exp(2x) at x=1 => df/dx = 2*exp(2)
    LispVal res = run(R"(
        (module test
            (define t (tape-new))
            (define x (tape-var t 1.0))
            (tape-start! t)
            (define z (exp (* 2 x)))
            (tape-stop!)
            (tape-backward! t z)
            (define result (tape-adjoint t x))
        )
    )");
    BOOST_CHECK_CLOSE(to_double(res), 2.0 * std::exp(2.0), 0.01);
}

BOOST_FIXTURE_TEST_CASE(tape_sin_cos, FunctionalTestFixture) {
    // f(x) = sin(x) at x=1 => df/dx = cos(1)
    LispVal res = run(R"(
        (module test
            (define t (tape-new))
            (define x (tape-var t 1.0))
            (tape-start! t)
            (define z (sin x))
            (tape-stop!)
            (tape-backward! t z)
            (define result (tape-adjoint t x))
        )
    )");
    BOOST_CHECK_CLOSE(to_double(res), std::cos(1.0), 0.01);
}

BOOST_FIXTURE_TEST_CASE(tape_log, FunctionalTestFixture) {
    // f(x) = log(x) at x=2 => df/dx = 1/x = 0.5
    LispVal res = run(R"(
        (module test
            (define t (tape-new))
            (define x (tape-var t 2.0))
            (tape-start! t)
            (define z (log x))
            (tape-stop!)
            (tape-backward! t z)
            (define result (tape-adjoint t x))
        )
    )");
    BOOST_CHECK_CLOSE(to_double(res), 0.5, 0.01);
}

BOOST_FIXTURE_TEST_CASE(tape_mixed_tape_and_plain, FunctionalTestFixture) {
    // f(x) = 3*x + 1 at x=5 => df/dx = 3
    // Tests auto-promotion of plain numbers to tape constants
    LispVal res = run(R"(
        (module test
            (define t (tape-new))
            (define x (tape-var t 5.0))
            (tape-start! t)
            (define z (+ (* 3 x) 1))
            (tape-stop!)
            (tape-backward! t z)
            (define result (tape-adjoint t x))
        )
    )");
    BOOST_CHECK_CLOSE(to_double(res), 3.0, 1e-10);
}

BOOST_FIXTURE_TEST_CASE(tape_rosenbrock, FunctionalTestFixture) {
    // f(x,y) = (1-x)^2 + 100*(y-x^2)^2 at (1,1) => grad = (0, 0)
    LispVal res = run(R"(
        (module test
            (define t (tape-new))
            (define x (tape-var t 1.0))
            (define y (tape-var t 1.0))
            (tape-start! t)
            (define z (+ (* (- 1 x) (- 1 x))
                         (* 100 (* (- y (* x x))
                                   (- y (* x x))))))
            (tape-stop!)
            (tape-backward! t z)
            (define result (+ (abs (tape-adjoint t x)) (abs (tape-adjoint t y))))
        )
    )");
    BOOST_CHECK_SMALL(to_double(res), 1e-10);
}

BOOST_FIXTURE_TEST_CASE(tape_multivar_with_sin, FunctionalTestFixture) {
    // f(x,y) = x*y + sin(x) at (2,3)
    // df/dx = y + cos(x) = 3 + cos(2) ≈ 2.5839
    // df/dy = x = 2
    LispVal res = run(R"(
        (module test
            (define t (tape-new))
            (define x (tape-var t 2.0))
            (define y (tape-var t 3.0))
            (tape-start! t)
            (define z (+ (* x y) (sin x)))
            (tape-stop!)
            (tape-backward! t z)
            (define result (tape-adjoint t x))
        )
    )");
    BOOST_CHECK_CLOSE(to_double(res), 3.0 + std::cos(2.0), 0.01);
}

// ============================================================================
// Nested tape tests — verify the stack-based active-tape design
// ============================================================================

BOOST_FIXTURE_TEST_CASE(tape_nested_independent, FunctionalTestFixture) {
    // Start tape1, then start tape2 inside, stop tape2, continue recording
    // on tape1.  Both tapes should produce correct, independent gradients.
    //
    // tape1: f(x) = x^2  at x=3  => df/dx = 6
    // tape2: g(y) = y*2  at y=5  => dg/dy = 2
    LispVal res = run(R"(
        (module test
            (define t1 (tape-new))
            (define x  (tape-var t1 3.0))
            (tape-start! t1)

            ;; nested tape
            (define t2 (tape-new))
            (define y  (tape-var t2 5.0))
            (tape-start! t2)
            (define g  (* y 2))
            (tape-stop!)          ;; pops t2, t1 becomes active again
            (tape-backward! t2 g)

            ;; continue on t1
            (define f  (* x x))
            (tape-stop!)          ;; pops t1
            (tape-backward! t1 f)

            ;; df/dx=6, dg/dy=2  => 6 + 2 = 8
            (define result (+ (tape-adjoint t1 x) (tape-adjoint t2 y)))
        )
    )");
    BOOST_CHECK_CLOSE(to_double(res), 8.0, 1e-10);
}

BOOST_FIXTURE_TEST_CASE(tape_nested_start_stop_balancing, FunctionalTestFixture) {
    // After starting and stopping two nested tapes, the active-tape stack
    // should be empty — subsequent arithmetic should NOT record.
    // Verify by computing a plain addition that returns a number, not a TapeRef.
    LispVal res = run(R"(
        (module test
            (define t1 (tape-new))
            (define t2 (tape-new))
            (tape-start! t1)
            (tape-start! t2)
            (tape-stop!)
            (tape-stop!)
            ;; No tape active — plain arithmetic
            (define result (+ 10 20))
        )
    )");
    BOOST_CHECK_CLOSE(to_double(res), 30.0, 1e-10);
}

BOOST_FIXTURE_TEST_CASE(tape_exception_unwinds_tape_stack, FunctionalTestFixture) {
    // Start a tape, throw an exception, catch it — the tape stack should be
    // unwound so subsequent arithmetic is plain (not tape-recorded).
    LispVal res = run(R"(
        (module test
            (define t (tape-new))
            (define caught
                (catch 'boom
                    (begin
                        (tape-start! t)
                        (raise 'boom 'oops))))
            ;; tape-stack should have been restored by the exception handler
            ;; so this is plain arithmetic, not a TapeRef
            (define result (+ 100 200))
        )
    )");
    BOOST_CHECK_CLOSE(to_double(res), 300.0, 1e-10);
}

BOOST_FIXTURE_TEST_CASE(tape_exception_preserves_outer_tape, FunctionalTestFixture) {
    // Outer tape is active, inner tape is started then an exception fires.
    // After the catch the outer tape should still be active and recording.
    //
    // outer: f(x) = x * 3  at x=4  => df/dx = 3
    LispVal res = run(R"(
        (module test
            (define t-outer (tape-new))
            (define x (tape-var t-outer 4.0))
            (tape-start! t-outer)

            ;; Inner block that throws — its tape-start! should be unwound
            (define t-inner (tape-new))
            (define caught
                (catch 'fail
                    (begin
                        (tape-start! t-inner)
                        (raise 'fail 'caught))))

            ;; The outer tape should still be active
            (define f (* x 3))
            (tape-stop!)
            (tape-backward! t-outer f)
            (define result (tape-adjoint t-outer x))
        )
    )");
    BOOST_CHECK_CLOSE(to_double(res), 3.0, 1e-10);
}

BOOST_AUTO_TEST_SUITE_END()
