#include <boost/test/unit_test.hpp>
#include <iostream>
#include <algorithm>
#include <functional>
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

using namespace eta;
using namespace eta::semantics;
using namespace eta::runtime;
using namespace eta::runtime::vm;
using namespace eta::runtime::memory::factory;

struct VMTestFixture {
    memory::heap::Heap heap;
    memory::intern::InternTable intern_table;
    BytecodeFunctionRegistry registry;
    BuiltinEnvironment builtins;

    VMTestFixture() : heap(1024 * 1024), intern_table(), registry() {
        register_core_primitives(builtins, heap, intern_table);
    }

    /**
     * @brief Compile and execute a multi-module program, returning the value
     * of the 'result' global from the last module.
     *
     * Processes ALL modules returned by analyze_all (not just the first),
     * emitting and executing each in order.
     */
    LispVal run_multi(std::string_view source) {
        reader::lexer::Lexer lex(0, source);
        reader::parser::Parser p(lex);
        auto parsed_res = p.parse_toplevel();
        if (!parsed_res) throw std::runtime_error("Parse error");
        auto parsed = std::move(*parsed_res);

        reader::expander::Expander ex;
        auto expanded_res = ex.expand_many(parsed);
        if (!expanded_res) throw std::runtime_error("Expansion error");
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

        // Emit bytecode for all modules
        std::vector<BytecodeFunction*> main_funcs;
        for (auto& mod : sem_mods) {
            Emitter emitter(mod, heap, intern_table, registry);
            main_funcs.push_back(emitter.emit());
        }

        // Execute each module in order on the same VM
        VM vm(heap, intern_table);
        vm.set_function_resolver([this](uint32_t idx) { return registry.get(idx); });

        for (size_t i = 0; i < sem_mods.size(); ++i) {
            auto install_res = builtins.install(heap, vm.globals(), sem_mods[i].bindings.size());
            if (!install_res) throw std::runtime_error("Failed to install builtins for module " + sem_mods[i].name);

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

        // Find 'result' in the last module
        auto& last_mod = sem_mods.back();
        for (size_t i = 0; i < last_mod.bindings.size(); ++i) {
            if (last_mod.bindings[i].name == "result") {
                return vm.globals()[last_mod.bindings[i].slot];
            }
        }
        throw std::runtime_error("No 'result' binding found in last module");
    }

    LispVal run(std::string_view source) {
        reader::lexer::Lexer lex(0, source);
        reader::parser::Parser p(lex);
        auto parsed_res = p.parse_toplevel();
        if (!parsed_res) throw std::runtime_error("Parse error");
        auto parsed = std::move(*parsed_res);

        reader::expander::Expander ex;
        auto expanded_res = ex.expand_many(parsed);
        if (!expanded_res) throw std::runtime_error("Expansion error");
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

        // Install builtins and size globals to accommodate all module bindings
        auto install_res = builtins.install(heap, vm.globals(), sem_mod.bindings.size());
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

        // Find 'result' global
        for (size_t i = 0; i < sem_mod.bindings.size(); ++i) {
            if (sem_mod.bindings[i].name == "result") {
                return vm.globals()[sem_mod.bindings[i].slot];
            }
        }

        return exec_res.value();
    }
};

BOOST_FIXTURE_TEST_SUITE(vm_tests, VMTestFixture)

// ============================================================================
// Basic arithmetic
// ============================================================================

BOOST_AUTO_TEST_CASE(test_add_simple) {
    LispVal res = run("(module m (define result (+ 1 2)))");
    BOOST_CHECK_EQUAL(nanbox::ops::decode<int64_t>(res).value(), 3);
}

BOOST_AUTO_TEST_CASE(test_add_variadic) {
    LispVal res = run("(module m (define result (+ 1 2 3 4 5)))");
    BOOST_CHECK_EQUAL(nanbox::ops::decode<int64_t>(res).value(), 15);
}

BOOST_AUTO_TEST_CASE(test_add_zero_args) {
    LispVal res = run("(module m (define result (+)))");
    BOOST_CHECK_EQUAL(nanbox::ops::decode<int64_t>(res).value(), 0);
}

BOOST_AUTO_TEST_CASE(test_sub_simple) {
    LispVal res = run("(module m (define result (- 10 3)))");
    BOOST_CHECK_EQUAL(nanbox::ops::decode<int64_t>(res).value(), 7);
}

BOOST_AUTO_TEST_CASE(test_sub_unary) {
    LispVal res = run("(module m (define result (- 5)))");
    BOOST_CHECK_EQUAL(nanbox::ops::decode<int64_t>(res).value(), -5);
}

BOOST_AUTO_TEST_CASE(test_mul_simple) {
    LispVal res = run("(module m (define result (* 6 7)))");
    BOOST_CHECK_EQUAL(nanbox::ops::decode<int64_t>(res).value(), 42);
}

BOOST_AUTO_TEST_CASE(test_div_exact) {
    LispVal res = run("(module m (define result (/ 10 2)))");
    BOOST_CHECK_EQUAL(nanbox::ops::decode<int64_t>(res).value(), 5);
}

// ============================================================================
// Comparison
// ============================================================================

BOOST_AUTO_TEST_CASE(test_equal_true) {
    LispVal res = run("(module m (define result (= 42 42)))");
    BOOST_CHECK_EQUAL(res, nanbox::True);
}

BOOST_AUTO_TEST_CASE(test_equal_false) {
    LispVal res = run("(module m (define result (= 1 2)))");
    BOOST_CHECK_EQUAL(res, nanbox::False);
}

BOOST_AUTO_TEST_CASE(test_less_than) {
    LispVal res = run("(module m (define result (< 1 2)))");
    BOOST_CHECK_EQUAL(res, nanbox::True);
}

BOOST_AUTO_TEST_CASE(test_greater_than) {
    LispVal res = run("(module m (define result (> 5 3)))");
    BOOST_CHECK_EQUAL(res, nanbox::True);
}

// ============================================================================
// Equivalence
// ============================================================================

BOOST_AUTO_TEST_CASE(test_eq_same) {
    LispVal res = run("(module m (define result (eq? 42 42)))");
    BOOST_CHECK_EQUAL(res, nanbox::True);
}

BOOST_AUTO_TEST_CASE(test_not_true) {
    LispVal res = run("(module m (define result (not #f)))");
    BOOST_CHECK_EQUAL(res, nanbox::True);
}

BOOST_AUTO_TEST_CASE(test_not_false) {
    LispVal res = run("(module m (define result (not #t)))");
    BOOST_CHECK_EQUAL(res, nanbox::False);
}

// ============================================================================
// Pairs / Lists
// ============================================================================

BOOST_AUTO_TEST_CASE(test_cons_car) {
    LispVal res = run("(module m (define result (car (cons 1 2))))");
    BOOST_CHECK_EQUAL(nanbox::ops::decode<int64_t>(res).value(), 1);
}

BOOST_AUTO_TEST_CASE(test_cons_cdr) {
    LispVal res = run("(module m (define result (cdr (cons 1 2))))");
    BOOST_CHECK_EQUAL(nanbox::ops::decode<int64_t>(res).value(), 2);
}

BOOST_AUTO_TEST_CASE(test_pair_true) {
    LispVal res = run("(module m (define result (pair? (cons 1 2))))");
    BOOST_CHECK_EQUAL(res, nanbox::True);
}

BOOST_AUTO_TEST_CASE(test_pair_false) {
    LispVal res = run("(module m (define result (pair? 42)))");
    BOOST_CHECK_EQUAL(res, nanbox::False);
}

BOOST_AUTO_TEST_CASE(test_null_true) {
    LispVal res = run("(module m (define result (null? (list))))");
    BOOST_CHECK_EQUAL(res, nanbox::True);
}

BOOST_AUTO_TEST_CASE(test_null_false) {
    LispVal res = run("(module m (define result (null? (cons 1 2))))");
    BOOST_CHECK_EQUAL(res, nanbox::False);
}

BOOST_AUTO_TEST_CASE(test_list_construction) {
    LispVal res = run("(module m (define result (car (cdr (list 1 2 3)))))");
    BOOST_CHECK_EQUAL(nanbox::ops::decode<int64_t>(res).value(), 2);
}

// ============================================================================
// Type predicates
// ============================================================================

BOOST_AUTO_TEST_CASE(test_number_pred) {
    LispVal res = run("(module m (define result (number? 42)))");
    BOOST_CHECK_EQUAL(res, nanbox::True);
}

BOOST_AUTO_TEST_CASE(test_boolean_pred) {
    LispVal res = run("(module m (define result (boolean? #t)))");
    BOOST_CHECK_EQUAL(res, nanbox::True);
}

BOOST_AUTO_TEST_CASE(test_boolean_pred_false_on_num) {
    LispVal res = run("(module m (define result (boolean? 42)))");
    BOOST_CHECK_EQUAL(res, nanbox::False);
}

BOOST_AUTO_TEST_CASE(test_procedure_pred) {
    LispVal res = run("(module m (define result (procedure? (lambda (x) x))))");
    BOOST_CHECK_EQUAL(res, nanbox::True);
}

BOOST_AUTO_TEST_CASE(test_procedure_pred_builtin) {
    LispVal res = run("(module m (define result (procedure? +)))");
    BOOST_CHECK_EQUAL(res, nanbox::True);
}

// ============================================================================
// Call/cc and control flow
// ============================================================================

BOOST_AUTO_TEST_CASE(test_call_cc_basic) {
    std::string src = "(module m (define result (call/cc (lambda (k) (k 42) 99))))";
    LispVal res = run(src);
    BOOST_CHECK_EQUAL(nanbox::ops::decode<int64_t>(res).value(), 42);
}

BOOST_AUTO_TEST_CASE(test_tail_call_recursion) {
    std::string src = 
        "(module m "
        "  (define (loop n) "
        "    (if (eq? n 0) "
        "        42 "
        "        (loop (- n 1)))) "
        "  (define result (loop 2000)))";
    LispVal res = run(src);
    BOOST_CHECK_EQUAL(nanbox::ops::decode<int64_t>(res).value(), 42);
}

BOOST_AUTO_TEST_CASE(test_dynamic_wind_basic) {
    std::string src = 
        "(module m (define result 0) "
        "  (define (before) (set! result 1)) "
        "  (define (body) 42) "
        "  (define (after) (set! result 3)) "
        "  (dynamic-wind before body after))";
    LispVal res = run(src);
    BOOST_CHECK_EQUAL(nanbox::ops::decode<int64_t>(res).value(), 3);
}

BOOST_AUTO_TEST_CASE(test_dynamic_wind_with_call_cc_clean) {
    std::string src =
        "(module m "
        "  (define result 0) "
        "  (define (before) (set! result (+ result 1))) "
        "  (define (after) (set! result (+ result 10))) "
        "  (define cont #f) "
        "  (dynamic-wind before "
        "    (lambda () (call/cc (lambda (k) (set! cont k)))) "
        "    after) "
        "  (if (eq? result 11) (cont #f) #f) "
        "  result)";

    LispVal res = run(src);
    BOOST_CHECK_EQUAL(nanbox::ops::decode<int64_t>(res).value(), 22);
}

// ============================================================================
// Closures and higher-order functions
// ============================================================================

BOOST_AUTO_TEST_CASE(test_closure_captures) {
    std::string src =
        "(module m "
        "  (define (make-adder n) (lambda (x) (+ n x))) "
        "  (define add5 (make-adder 5)) "
        "  (define result (add5 10)))";
    LispVal res = run(src);
    BOOST_CHECK_EQUAL(nanbox::ops::decode<int64_t>(res).value(), 15);
}

BOOST_AUTO_TEST_CASE(test_recursive_fibonacci) {
    std::string src =
        "(module m "
        "  (define (fib n) "
        "    (if (< n 2) n (+ (fib (- n 1)) (fib (- n 2))))) "
        "  (define result (fib 10)))";
    LispVal res = run(src);
    BOOST_CHECK_EQUAL(nanbox::ops::decode<int64_t>(res).value(), 55);
}

BOOST_AUTO_TEST_CASE(test_mutual_recursion) {
    std::string src =
        "(module m "
        "  (define (is-even n) (if (= n 0) #t (is-odd (- n 1)))) "
        "  (define (is-odd n) (if (= n 0) #f (is-even (- n 1)))) "
        "  (define result (is-even 10)))";
    LispVal res = run(src);
    BOOST_CHECK_EQUAL(res, nanbox::True);
}

// ============================================================================
// Arity checking
// ============================================================================

BOOST_AUTO_TEST_CASE(test_arity_error_too_few) {
    BOOST_CHECK_THROW(run("(module m (define result (cons 1)))"), std::runtime_error);
}

BOOST_AUTO_TEST_CASE(test_arity_error_too_many) {
    BOOST_CHECK_THROW(run("(module m (define result (car 1 2)))"), std::runtime_error);
}

// ============================================================================
// Immutable builtins
// ============================================================================

BOOST_AUTO_TEST_CASE(test_builtin_immutable) {
    BOOST_CHECK_THROW(run("(module m (set! + 42))"), std::runtime_error);
}

// ============================================================================
// and / or / when / unless
// ============================================================================

BOOST_AUTO_TEST_CASE(test_and_empty) {
    LispVal res = run("(module m (define result (and)))");
    BOOST_CHECK_EQUAL(res, nanbox::True);
}

BOOST_AUTO_TEST_CASE(test_and_single_true) {
    LispVal res = run("(module m (define result (and 42)))");
    BOOST_CHECK_EQUAL(nanbox::ops::decode<int64_t>(res).value(), 42);
}

BOOST_AUTO_TEST_CASE(test_and_short_circuit) {
    LispVal res = run("(module m (define result (and #f 42)))");
    BOOST_CHECK_EQUAL(res, nanbox::False);
}

BOOST_AUTO_TEST_CASE(test_and_all_true) {
    LispVal res = run("(module m (define result (and 1 2 3)))");
    BOOST_CHECK_EQUAL(nanbox::ops::decode<int64_t>(res).value(), 3);
}

BOOST_AUTO_TEST_CASE(test_or_empty) {
    LispVal res = run("(module m (define result (or)))");
    BOOST_CHECK_EQUAL(res, nanbox::False);
}

BOOST_AUTO_TEST_CASE(test_or_first_true) {
    LispVal res = run("(module m (define result (or 42 99)))");
    BOOST_CHECK_EQUAL(nanbox::ops::decode<int64_t>(res).value(), 42);
}

BOOST_AUTO_TEST_CASE(test_or_first_false) {
    LispVal res = run("(module m (define result (or #f 99)))");
    BOOST_CHECK_EQUAL(nanbox::ops::decode<int64_t>(res).value(), 99);
}

BOOST_AUTO_TEST_CASE(test_or_all_false) {
    LispVal res = run("(module m (define result (or #f #f)))");
    BOOST_CHECK_EQUAL(res, nanbox::False);
}

BOOST_AUTO_TEST_CASE(test_when_true) {
    LispVal res = run("(module m (define result 0) (when #t (set! result 42)))");
    BOOST_CHECK_EQUAL(nanbox::ops::decode<int64_t>(res).value(), 42);
}

BOOST_AUTO_TEST_CASE(test_when_false) {
    LispVal res = run("(module m (define result 0) (when #f (set! result 42)))");
    BOOST_CHECK_EQUAL(nanbox::ops::decode<int64_t>(res).value(), 0);
}

BOOST_AUTO_TEST_CASE(test_unless_false) {
    LispVal res = run("(module m (define result 0) (unless #f (set! result 42)))");
    BOOST_CHECK_EQUAL(nanbox::ops::decode<int64_t>(res).value(), 42);
}

BOOST_AUTO_TEST_CASE(test_unless_true) {
    LispVal res = run("(module m (define result 0) (unless #t (set! result 42)))");
    BOOST_CHECK_EQUAL(nanbox::ops::decode<int64_t>(res).value(), 0);
}

// ============================================================================
// do loop
// ============================================================================

BOOST_AUTO_TEST_CASE(test_do_loop_factorial) {
    std::string src =
        "(module m "
        "  (define result "
        "    (do ((i 1 (+ i 1)) "
        "         (acc 1 (* acc i))) "
        "        ((> i 5) acc))))";
    LispVal res = run(src);
    BOOST_CHECK_EQUAL(nanbox::ops::decode<int64_t>(res).value(), 120);
}

BOOST_AUTO_TEST_CASE(test_do_loop_sum) {
    std::string src =
        "(module m "
        "  (define result "
        "    (do ((i 0 (+ i 1)) "
        "         (sum 0 (+ sum i))) "
        "        ((= i 10) sum))))";
    LispVal res = run(src);
    BOOST_CHECK_EQUAL(nanbox::ops::decode<int64_t>(res).value(), 45);
}

// ============================================================================
// Quote
// ============================================================================

BOOST_AUTO_TEST_CASE(test_quote_number) {
    LispVal res = run("(module m (define result (quote 42)))");
    BOOST_CHECK_EQUAL(nanbox::ops::decode<int64_t>(res).value(), 42);
}

BOOST_AUTO_TEST_CASE(test_quote_list) {
    // (quote (1 2 3)) -> list, check car
    LispVal res = run("(module m (define result (car (quote (1 2 3)))))");
    BOOST_CHECK_EQUAL(nanbox::ops::decode<int64_t>(res).value(), 1);
}

BOOST_AUTO_TEST_CASE(test_quote_nil) {
    LispVal res = run("(module m (define result (null? (quote ()))))");
    BOOST_CHECK_EQUAL(res, nanbox::True);
}

// ============================================================================
// New primitives: numeric predicates
// ============================================================================

BOOST_AUTO_TEST_CASE(test_zero_pred) {
    LispVal res = run("(module m (define result (zero? 0)))");
    BOOST_CHECK_EQUAL(res, nanbox::True);
}

BOOST_AUTO_TEST_CASE(test_zero_pred_false) {
    LispVal res = run("(module m (define result (zero? 5)))");
    BOOST_CHECK_EQUAL(res, nanbox::False);
}

BOOST_AUTO_TEST_CASE(test_positive_pred) {
    LispVal res = run("(module m (define result (positive? 5)))");
    BOOST_CHECK_EQUAL(res, nanbox::True);
}

BOOST_AUTO_TEST_CASE(test_negative_pred) {
    LispVal res = run("(module m (define result (negative? (- 0 3))))");
    BOOST_CHECK_EQUAL(res, nanbox::True);
}

BOOST_AUTO_TEST_CASE(test_abs) {
    LispVal res = run("(module m (define result (abs (- 0 42))))");
    BOOST_CHECK_EQUAL(nanbox::ops::decode<int64_t>(res).value(), 42);
}

BOOST_AUTO_TEST_CASE(test_min) {
    LispVal res = run("(module m (define result (min 5 3 8 1 7)))");
    BOOST_CHECK_EQUAL(nanbox::ops::decode<int64_t>(res).value(), 1);
}

BOOST_AUTO_TEST_CASE(test_max) {
    LispVal res = run("(module m (define result (max 5 3 8 1 7)))");
    BOOST_CHECK_EQUAL(nanbox::ops::decode<int64_t>(res).value(), 8);
}

BOOST_AUTO_TEST_CASE(test_modulo) {
    LispVal res = run("(module m (define result (modulo 10 3)))");
    BOOST_CHECK_EQUAL(nanbox::ops::decode<int64_t>(res).value(), 1);
}

BOOST_AUTO_TEST_CASE(test_remainder) {
    LispVal res = run("(module m (define result (remainder 10 3)))");
    BOOST_CHECK_EQUAL(nanbox::ops::decode<int64_t>(res).value(), 1);
}

// ============================================================================
// New primitives: list operations
// ============================================================================

BOOST_AUTO_TEST_CASE(test_length) {
    LispVal res = run("(module m (define result (length (list 1 2 3))))");
    BOOST_CHECK_EQUAL(nanbox::ops::decode<int64_t>(res).value(), 3);
}

BOOST_AUTO_TEST_CASE(test_length_empty) {
    LispVal res = run("(module m (define result (length (list))))");
    BOOST_CHECK_EQUAL(nanbox::ops::decode<int64_t>(res).value(), 0);
}

BOOST_AUTO_TEST_CASE(test_append) {
    LispVal res = run("(module m (define result (car (cdr (append (list 1 2) (list 3 4))))))");
    BOOST_CHECK_EQUAL(nanbox::ops::decode<int64_t>(res).value(), 2);
}

BOOST_AUTO_TEST_CASE(test_append_empty) {
    LispVal res = run("(module m (define result (null? (append))))");
    BOOST_CHECK_EQUAL(res, nanbox::True);
}

BOOST_AUTO_TEST_CASE(test_reverse) {
    LispVal res = run("(module m (define result (car (reverse (list 1 2 3)))))");
    BOOST_CHECK_EQUAL(nanbox::ops::decode<int64_t>(res).value(), 3);
}

BOOST_AUTO_TEST_CASE(test_list_ref) {
    LispVal res = run("(module m (define result (list-ref (list 10 20 30) 1)))");
    BOOST_CHECK_EQUAL(nanbox::ops::decode<int64_t>(res).value(), 20);
}

// ============================================================================
// New primitives: equal?
// ============================================================================

BOOST_AUTO_TEST_CASE(test_equal_lists) {
    LispVal res = run("(module m (define result (equal? (list 1 2 3) (list 1 2 3))))");
    BOOST_CHECK_EQUAL(res, nanbox::True);
}

BOOST_AUTO_TEST_CASE(test_equal_lists_different) {
    LispVal res = run("(module m (define result (equal? (list 1 2) (list 1 3))))");
    BOOST_CHECK_EQUAL(res, nanbox::False);
}

// ============================================================================
// New primitives: vector operations
// ============================================================================

BOOST_AUTO_TEST_CASE(test_vector_construction) {
    LispVal res = run("(module m (define result (vector-ref (vector 10 20 30) 1)))");
    BOOST_CHECK_EQUAL(nanbox::ops::decode<int64_t>(res).value(), 20);
}

BOOST_AUTO_TEST_CASE(test_vector_length) {
    LispVal res = run("(module m (define result (vector-length (vector 1 2 3))))");
    BOOST_CHECK_EQUAL(nanbox::ops::decode<int64_t>(res).value(), 3);
}

BOOST_AUTO_TEST_CASE(test_vector_pred) {
    LispVal res = run("(module m (define result (vector? (vector 1 2))))");
    BOOST_CHECK_EQUAL(res, nanbox::True);
}

BOOST_AUTO_TEST_CASE(test_vector_pred_false) {
    LispVal res = run("(module m (define result (vector? 42)))");
    BOOST_CHECK_EQUAL(res, nanbox::False);
}

// ============================================================================
// Multi-module execution tests
//
// These tests document the cross-module runtime execution requirement.
// Currently, each ModuleSemantics has independent global slot numbering,
// and BuiltinEnvironment::install() wipes the globals vector on each call.
// A cross-module slot mapping or unified global allocation is needed.
//
// STATUS: Expected to fail until cross-module runtime is implemented.
// ============================================================================

BOOST_AUTO_TEST_CASE(test_multi_module_analyze_produces_multiple) {
    // Verify that the pipeline at least produces multiple ModuleSemantics
    std::string_view source =
        "(module lib (export answer) (define answer 42))\n"
        "(module main (import lib) (define result answer))";

    reader::lexer::Lexer lex(0, source);
    reader::parser::Parser p(lex);
    auto parsed = std::move(*p.parse_toplevel());

    reader::expander::Expander ex;
    auto expanded = std::move(*ex.expand_many(parsed));

    reader::ModuleLinker linker;
    auto idx = linker.index_modules(expanded);
    BOOST_REQUIRE(idx.has_value());
    auto lk = linker.link();
    BOOST_REQUIRE(lk.has_value());

    SemanticAnalyzer sa;
    auto sem_res = sa.analyze_all(expanded, linker, builtins);
    BOOST_REQUIRE(sem_res.has_value());
    BOOST_CHECK_EQUAL(sem_res->size(), 2);
    BOOST_CHECK_EQUAL((*sem_res)[0].name, "lib");
    BOOST_CHECK_EQUAL((*sem_res)[1].name, "main");

    // Verify the import binding exists in module 'main'
    const auto& main_mod = (*sem_res)[1];
    bool found_import = false;
    for (const auto& b : main_mod.bindings) {
        if (b.name == "answer" && b.kind == BindingInfo::Kind::Import) {
            found_import = true;
            break;
        }
    }
    BOOST_CHECK(found_import);
}

BOOST_AUTO_TEST_CASE(test_multi_module_each_emits_bytecode) {
    // Verify that each module produces valid bytecode
    std::string_view source =
        "(module lib (export answer) (define answer 42))\n"
        "(module main (import lib) (define result answer))";

    reader::lexer::Lexer lex(0, source);
    reader::parser::Parser p(lex);
    auto parsed = std::move(*p.parse_toplevel());

    reader::expander::Expander ex;
    auto expanded = std::move(*ex.expand_many(parsed));

    reader::ModuleLinker linker;
    linker.index_modules(expanded);
    linker.link();

    SemanticAnalyzer sa;
    auto sem_res = sa.analyze_all(expanded, linker, builtins);
    BOOST_REQUIRE(sem_res.has_value());

    for (auto& mod : *sem_res) {
        Emitter emitter(mod, heap, intern_table, registry);
        auto* main_func = emitter.emit();
        BOOST_CHECK(main_func != nullptr);
        BOOST_CHECK(!main_func->code.empty());
    }
}

BOOST_AUTO_TEST_SUITE_END()
