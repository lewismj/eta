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

        /// Emit bytecode for all modules
        std::vector<BytecodeFunction*> main_funcs;
        for (auto& mod : sem_mods) {
            Emitter emitter(mod, heap, intern_table, registry);
            main_funcs.push_back(emitter.emit());
        }

        /// Execute each module in order on the same VM with unified globals
        VM vm(heap, intern_table);
        vm.set_function_resolver([this](uint32_t idx) { return registry.get(idx); });

        /// Install builtins ONCE with the unified total_globals count
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

        /// Find 'result' in the last module
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

        /// Install builtins and size globals to accommodate all module bindings
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

        /// Find 'result' global
        for (size_t i = 0; i < sem_mod.bindings.size(); ++i) {
            if (sem_mod.bindings[i].name == "result") {
                return vm.globals()[sem_mod.bindings[i].slot];
            }
        }

        return exec_res.value();
    }

    /**
     * @brief Compile and execute, expecting a runtime error.
     * Returns the RuntimeError variant if execution fails.
     * Throws if compilation fails or execution succeeds unexpectedly.
     */
    RuntimeError run_expect_error(std::string_view source) {
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

        auto install_res = builtins.install(heap, vm.globals(), sem_mod.total_globals);
        if (!install_res) throw std::runtime_error("Failed to install builtins");

        auto exec_res = vm.execute(*main_func);
        if (exec_res) throw std::runtime_error("Expected execution to fail, but it succeeded");
        return exec_res.error();
    }
};

BOOST_FIXTURE_TEST_SUITE(vm_tests, VMTestFixture)

/**
 * Basic arithmetic
 */

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

/**
 * Comparison
 */

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

/**
 * Equivalence
 */

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

/**
 *
 * eqv? differs from eq? in that it unwraps heap-allocated numbers and
 * compares by value.  For inline fixnums, symbols, booleans, and chars
 * the two are identical; for separately-constructed heap-boxed fixnums
 * (values exceeding the 47-bit NaN-box range) eqv? returns #t where
 * eq? returns #f.
 */

BOOST_AUTO_TEST_CASE(test_eqv_same_fixnum) {
    LispVal res = run("(module m (define result (eqv? 42 42)))");
    BOOST_CHECK_EQUAL(res, nanbox::True);
}

BOOST_AUTO_TEST_CASE(test_eqv_different_fixnum) {
    LispVal res = run("(module m (define result (eqv? 1 2)))");
    BOOST_CHECK_EQUAL(res, nanbox::False);
}

BOOST_AUTO_TEST_CASE(test_eqv_booleans_same) {
    LispVal res = run("(module m (define result (eqv? #t #t)))");
    BOOST_CHECK_EQUAL(res, nanbox::True);
}

BOOST_AUTO_TEST_CASE(test_eqv_booleans_different) {
    LispVal res = run("(module m (define result (eqv? #t #f)))");
    BOOST_CHECK_EQUAL(res, nanbox::False);
}

BOOST_AUTO_TEST_CASE(test_eqv_chars_same) {
    LispVal res = run("(module m (define result (eqv? #\\a #\\a)))");
    BOOST_CHECK_EQUAL(res, nanbox::True);
}

BOOST_AUTO_TEST_CASE(test_eqv_chars_different) {
    LispVal res = run("(module m (define result (eqv? #\\a #\\b)))");
    BOOST_CHECK_EQUAL(res, nanbox::False);
}

BOOST_AUTO_TEST_CASE(test_eqv_symbols_same) {
    /// Symbols are interned so eqv? on the same symbol is #t
    LispVal res = run("(module m (define result (eqv? 'foo 'foo)))");
    BOOST_CHECK_EQUAL(res, nanbox::True);
}

BOOST_AUTO_TEST_CASE(test_eqv_symbols_different) {
    LispVal res = run("(module m (define result (eqv? 'foo 'bar)))");
    BOOST_CHECK_EQUAL(res, nanbox::False);
}

BOOST_AUTO_TEST_CASE(test_eqv_nil) {
    /// Both are the Nil singleton
    LispVal res = run("(module m (define result (eqv? '() '())))");
    BOOST_CHECK_EQUAL(res, nanbox::True);
}

BOOST_AUTO_TEST_CASE(test_eqv_lists_not_structural) {
    LispVal res = run("(module m (define result (eqv? (list 1 2) (list 1 2))))");
    BOOST_CHECK_EQUAL(res, nanbox::False);
}

BOOST_AUTO_TEST_CASE(test_eqv_mixed_types) {
    LispVal res = run("(module m (define result (eqv? 42 #t)))");
    BOOST_CHECK_EQUAL(res, nanbox::False);
}

BOOST_AUTO_TEST_CASE(test_eqv_heap_boxed_fixnums) {
    /**
     * Key divergence from eq?: two separately-computed large fixnums
     * get heap-allocated at different addresses.
     */
    std::string src =
        "(module m"
        "  (define a (* 100000000 100000000))"   ///< 10^16, heap-boxed
        "  (define b (* 100000000 100000000))"   ///< same value, different heap object
        "  (define result (eqv? a b)))";
    LispVal res = run(src);
    BOOST_CHECK_EQUAL(res, nanbox::True);
}

BOOST_AUTO_TEST_CASE(test_eq_heap_boxed_fixnums_false) {
    /// heap-allocated fixnums occupy different addresses.
    std::string src =
        "(module m"
        "  (define a (* 100000000 100000000))"
        "  (define b (* 100000000 100000000))"
        "  (define result (eq? a b)))";
    LispVal res = run(src);
    BOOST_CHECK_EQUAL(res, nanbox::False);
}

BOOST_AUTO_TEST_CASE(test_eqv_heap_boxed_fixnums_different_values) {
    /// Two large heap-boxed fixnums with different values
    std::string src =
        "(module m"
        "  (define a (* 100000000 100000000))"   ///< 10^16
        "  (define b (* 100000000 200000000))"
        "  (define result (eqv? a b)))";
    LispVal res = run(src);
    BOOST_CHECK_EQUAL(res, nanbox::False);
}

/**
 * Pairs / Lists
 */

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

/**
 * Type predicates
 */

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

/**
 * Call/cc and control flow
 */

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

/**
 * Closures and higher-order functions
 */

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

/**
 * Arity checking
 */

BOOST_AUTO_TEST_CASE(test_arity_error_too_few) {
    BOOST_CHECK_THROW(run("(module m (define result (cons 1)))"), std::runtime_error);
}

BOOST_AUTO_TEST_CASE(test_arity_error_too_many) {
    BOOST_CHECK_THROW(run("(module m (define result (car 1 2)))"), std::runtime_error);
}

/**
 * Immutable builtins
 */

BOOST_AUTO_TEST_CASE(test_builtin_immutable) {
    BOOST_CHECK_THROW(run("(module m (set! + 42))"), std::runtime_error);
}

/**
 * and / or / when / unless
 */

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

/**
 * do loop
 */

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

/**
 * Quote
 */

BOOST_AUTO_TEST_CASE(test_quote_number) {
    LispVal res = run("(module m (define result (quote 42)))");
    BOOST_CHECK_EQUAL(nanbox::ops::decode<int64_t>(res).value(), 42);
}

BOOST_AUTO_TEST_CASE(test_quote_list) {
    /// (quote (1 2 3)) -> list, check car
    LispVal res = run("(module m (define result (car (quote (1 2 3)))))");
    BOOST_CHECK_EQUAL(nanbox::ops::decode<int64_t>(res).value(), 1);
}

BOOST_AUTO_TEST_CASE(test_quote_nil) {
    LispVal res = run("(module m (define result (null? (quote ()))))");
    BOOST_CHECK_EQUAL(res, nanbox::True);
}

/**
 * New primitives: numeric predicates
 */

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

/**
 * New primitives: list operations
 */

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

/**
 * New primitives: equal?
 */

BOOST_AUTO_TEST_CASE(test_equal_lists) {
    LispVal res = run("(module m (define result (equal? (list 1 2 3) (list 1 2 3))))");
    BOOST_CHECK_EQUAL(res, nanbox::True);
}

BOOST_AUTO_TEST_CASE(test_equal_lists_different) {
    LispVal res = run("(module m (define result (equal? (list 1 2) (list 1 3))))");
    BOOST_CHECK_EQUAL(res, nanbox::False);
}

/**
 * New primitives: vector operations
 */

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

/**
 * Multi-module execution tests (unified global allocation)
 */

BOOST_AUTO_TEST_CASE(test_multi_module_analyze_proDUCES_multiple) {
    /// Verify that the pipeline at least produces multiple ModuleSemantics
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

    /// Verify the import binding exists in module 'main'
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
    /// Verify that each module produces valid bytecode
    std::string_view source =
        "(module lib (export answer) (define answer 42))\n"
        "(module main (import lib) (define result answer))";

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

    for (auto& mod : *sem_res) {
        Emitter emitter(mod, heap, intern_table, registry);
        auto* main_func = emitter.emit();
        BOOST_CHECK(main_func != nullptr);
        BOOST_CHECK(!main_func->code.empty());
    }
}

BOOST_AUTO_TEST_CASE(test_multi_module_unified_global_slots) {
    /// Verify that imported bindings share the same global slot as the export
    std::string_view source =
        "(module lib (export answer) (define answer 42))\n"
        "(module main (import lib) (define result answer))";

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
    BOOST_REQUIRE_EQUAL(sem_res->size(), 2);

    const auto& lib_mod = (*sem_res)[0];
    const auto& main_mod = (*sem_res)[1];

    /// Find the slot where lib defines 'answer'
    uint16_t lib_answer_slot = 0;
    for (const auto& b : lib_mod.bindings) {
        if (b.name == "answer" && b.kind == BindingInfo::Kind::Global) {
            lib_answer_slot = b.slot;
            break;
        }
    }

    /// Find the slot where main imports 'answer'
    uint16_t main_answer_slot = 0;
    for (const auto& b : main_mod.bindings) {
        if (b.name == "answer" && b.kind == BindingInfo::Kind::Import) {
            main_answer_slot = b.slot;
            break;
        }
    }

    /// Both must reference the same unified global slot
    BOOST_CHECK_EQUAL(lib_answer_slot, main_answer_slot);

    /// total_globals should be the same on both modules
    BOOST_CHECK_EQUAL(lib_mod.total_globals, main_mod.total_globals);
}

BOOST_AUTO_TEST_CASE(test_multi_module_import_constant) {
    /// Module lib exports a constant; module main imports and uses it
    LispVal res = run_multi(
        "(module lib (export answer) (define answer 42))\n"
        "(module main (import lib) (define result answer))");
    BOOST_CHECK_EQUAL(nanbox::ops::decode<int64_t>(res).value(), 42);
}

BOOST_AUTO_TEST_CASE(test_multi_module_import_function) {
    /// Module lib exports a function; module main calls it
    LispVal res = run_multi(
        "(module lib (export double) (define (double x) (* x 2)))\n"
        "(module main (import lib) (define result (double 21)))");
    BOOST_CHECK_EQUAL(nanbox::ops::decode<int64_t>(res).value(), 42);
}

BOOST_AUTO_TEST_CASE(test_multi_module_chain) {
    /// Linear chain: A -> B -> C, each adds 1
    LispVal res = run_multi(
        "(module A (export x) (define x 1))\n"
        "(module B (import A) (export y) (define y (+ x 1)))\n"
        "(module C (import B) (define result (+ y 1)))");
    BOOST_CHECK_EQUAL(nanbox::ops::decode<int64_t>(res).value(), 3);
}

BOOST_AUTO_TEST_CASE(test_multi_module_diamond_dependency) {
    /// A exports a; B and C both import from A; D imports from B and C
    LispVal res = run_multi(
        "(module A (export a) (define a 10))\n"
        "(module B (import A) (export b) (define b (+ a 5)))\n"
        "(module C (import A) (export c) (define c (+ a 20)))\n"
        "(module D (import B) (import C) (define result (+ b c)))");
    /// b = 15, c = 30, result = 45
    BOOST_CHECK_EQUAL(nanbox::ops::decode<int64_t>(res).value(), 45);
}

BOOST_AUTO_TEST_CASE(test_multi_module_export_function_closure) {
    /**
     * Module lib exports a closure (function capturing a local);
     * module main calls it
     */
    LispVal res = run_multi(
        "(module lib (export make-adder)\n"
        "  (define (make-adder n) (lambda (x) (+ n x))))\n"
        "(module main (import lib)\n"
        "  (define add5 (make-adder 5))\n"
        "  (define result (add5 37)))");
    BOOST_CHECK_EQUAL(nanbox::ops::decode<int64_t>(res).value(), 42);
}

/**
 * VM Error-Path Tests
 */

BOOST_AUTO_TEST_CASE(test_error_car_on_non_pair) {
    auto err = run_expect_error("(module m (define result (car 42)))");
    auto* vm_err = std::get_if<VMError>(&err);
    BOOST_REQUIRE(vm_err);
    BOOST_CHECK(vm_err->code == RuntimeErrorCode::TypeError);
}

BOOST_AUTO_TEST_CASE(test_error_cdr_on_non_pair) {
    auto err = run_expect_error("(module m (define result (cdr 42)))");
    auto* vm_err = std::get_if<VMError>(&err);
    BOOST_REQUIRE(vm_err);
    BOOST_CHECK(vm_err->code == RuntimeErrorCode::TypeError);
}

BOOST_AUTO_TEST_CASE(test_error_car_on_string) {
    auto err = run_expect_error("(module m (define result (car \"hello\")))");
    auto* vm_err = std::get_if<VMError>(&err);
    BOOST_REQUIRE(vm_err);
    BOOST_CHECK(vm_err->code == RuntimeErrorCode::TypeError);
}

BOOST_AUTO_TEST_CASE(test_error_add_non_numbers) {
    auto err = run_expect_error("(module m (define result (+ 1 #t)))");
    auto* vm_err = std::get_if<VMError>(&err);
    BOOST_REQUIRE(vm_err);
    BOOST_CHECK(vm_err->code == RuntimeErrorCode::TypeError);
}

BOOST_AUTO_TEST_CASE(test_error_sub_non_numbers) {
    auto err = run_expect_error("(module m (define result (- \"a\" 1)))");
    auto* vm_err = std::get_if<VMError>(&err);
    BOOST_REQUIRE(vm_err);
    BOOST_CHECK(vm_err->code == RuntimeErrorCode::TypeError);
}

BOOST_AUTO_TEST_CASE(test_error_mul_non_numbers) {
    auto err = run_expect_error("(module m (define result (* #f 2)))");
    auto* vm_err = std::get_if<VMError>(&err);
    BOOST_REQUIRE(vm_err);
    BOOST_CHECK(vm_err->code == RuntimeErrorCode::TypeError);
}

BOOST_AUTO_TEST_CASE(test_error_div_non_numbers) {
    auto err = run_expect_error("(module m (define result (/ \"a\" 2)))");
    auto* vm_err = std::get_if<VMError>(&err);
    BOOST_REQUIRE(vm_err);
    BOOST_CHECK(vm_err->code == RuntimeErrorCode::TypeError);
}

BOOST_AUTO_TEST_CASE(test_error_div_by_zero_integers) {
    auto err = run_expect_error("(module m (define result (/ 10 0)))");
    auto* vm_err = std::get_if<VMError>(&err);
    BOOST_REQUIRE(vm_err);
    BOOST_CHECK(vm_err->code == RuntimeErrorCode::TypeError);
}

BOOST_AUTO_TEST_CASE(test_error_div_by_zero_floats) {
    auto err = run_expect_error("(module m (define result (/ 10.0 0.0)))");
    auto* vm_err = std::get_if<VMError>(&err);
    BOOST_REQUIRE(vm_err);
    BOOST_CHECK(vm_err->code == RuntimeErrorCode::TypeError);
}

BOOST_AUTO_TEST_CASE(test_error_call_non_procedure) {
    auto err = run_expect_error("(module m (define x 42) (define result (x 1)))");
    auto* vm_err = std::get_if<VMError>(&err);
    BOOST_REQUIRE(vm_err);
    BOOST_CHECK(vm_err->code == RuntimeErrorCode::TypeError);
}

BOOST_AUTO_TEST_CASE(test_error_call_boolean_as_procedure) {
    auto err = run_expect_error("(module m (define result (#t 1)))");
    auto* vm_err = std::get_if<VMError>(&err);
    BOOST_REQUIRE(vm_err);
    BOOST_CHECK(vm_err->code == RuntimeErrorCode::TypeError);
}

BOOST_AUTO_TEST_CASE(test_error_arity_too_few_args) {
    auto err = run_expect_error(
        "(module m (define (f a b) (+ a b)) (define result (f 1)))");
    auto* vm_err = std::get_if<VMError>(&err);
    BOOST_REQUIRE(vm_err);
    BOOST_CHECK(vm_err->code == RuntimeErrorCode::InvalidArity);
}

BOOST_AUTO_TEST_CASE(test_error_arity_too_many_args) {
    auto err = run_expect_error(
        "(module m (define (f a) a) (define result (f 1 2 3)))");
    auto* vm_err = std::get_if<VMError>(&err);
    BOOST_REQUIRE(vm_err);
    BOOST_CHECK(vm_err->code == RuntimeErrorCode::InvalidArity);
}

BOOST_AUTO_TEST_CASE(test_error_arity_zero_args_expected_one) {
    auto err = run_expect_error(
        "(module m (define (f x) x) (define result (f)))");
    auto* vm_err = std::get_if<VMError>(&err);
    BOOST_REQUIRE(vm_err);
    BOOST_CHECK(vm_err->code == RuntimeErrorCode::InvalidArity);
}

BOOST_AUTO_TEST_CASE(test_error_cons_wrong_arg_count) {
    auto err = run_expect_error("(module m (define result (cons 1)))");
    auto* vm_err = std::get_if<VMError>(&err);
    BOOST_REQUIRE(vm_err);
    BOOST_CHECK(vm_err->code == RuntimeErrorCode::InvalidArity);
}

/**
 * Runtime-error catchability
 */

BOOST_AUTO_TEST_CASE(test_runtime_catch_super_tag_returns_payload_with_span_and_trace) {
    LispVal res = run(
        "(module m"
        "  (define p (catch 'runtime.error (car 42)))"
        "  (define tag   (car (cdr p)))"
        "  (define msg   (car (cdr (cdr p))))"
        "  (define spn   (car (cdr (cdr (cdr p)))))"
        "  (define trace (car (cdr (cdr (cdr (cdr p))))))"
        "  (define span-ok"
        "    (and (pair? spn)"
        "         (eq? (car spn) 'span)"
        "         (number? (car (cdr spn)))"
        "         (number? (car (cdr (cdr spn))))"
        "         (number? (car (cdr (cdr (cdr spn)))))"
        "         (number? (car (cdr (cdr (cdr (cdr spn))))))"
        "         (number? (car (cdr (cdr (cdr (cdr (cdr spn)))))))))"
        "  (define top-frame (car trace))"
        "  (define frame-span (car (cdr (cdr top-frame))))"
        "  (define trace-ok"
        "    (and (pair? trace)"
        "         (pair? top-frame)"
        "         (eq? (car top-frame) 'frame)"
        "         (string? (car (cdr top-frame)))"
        "         (pair? frame-span)"
        "         (eq? (car frame-span) 'span)))"
        "  (define result"
        "    (and (pair? p)"
        "         (eq? (car p) 'runtime-error)"
        "         (eq? tag 'runtime.type-error)"
        "         (string? msg)"
        "         span-ok"
        "         trace-ok)))");
    BOOST_CHECK_EQUAL(res, nanbox::True);
}

BOOST_AUTO_TEST_CASE(test_runtime_catch_specific_subtype_invalid_arity) {
    LispVal res = run(
        "(module m"
        "  (define p (catch 'runtime.invalid-arity (car 1 2)))"
        "  (define result"
        "    (and (pair? p)"
        "         (eq? (car p) 'runtime-error)"
        "         (eq? (car (cdr p)) 'runtime.invalid-arity)"
        "         (string? (car (cdr (cdr p)))))))");
    BOOST_CHECK_EQUAL(res, nanbox::True);
}

BOOST_AUTO_TEST_CASE(test_runtime_catch_mismatched_subtype_propagates) {
    auto err = run_expect_error("(module m (define result (catch 'runtime.type-error (car 1 2))))");
    auto* vm_err = std::get_if<VMError>(&err);
    BOOST_REQUIRE(vm_err);
    BOOST_CHECK(vm_err->code == RuntimeErrorCode::InvalidArity);
}

BOOST_AUTO_TEST_CASE(test_runtime_catch_tagless_catch_body_catches_runtime_errors) {
    LispVal res = run(
        "(module m"
        "  (define p (catch (car 42)))"
        "  (define result"
        "    (and (pair? p)"
        "         (eq? (car p) 'runtime-error)"
        "         (eq? (car (cdr p)) 'runtime.type-error))))");
    BOOST_CHECK_EQUAL(res, nanbox::True);
}

BOOST_AUTO_TEST_CASE(test_runtime_catch_explicit_raise_semantics_unchanged) {
    LispVal res = run(
        "(module m"
        "  (define a (catch 'boom (raise 'boom 7)))"
        "  (define b (catch (raise 'x 9)))"
        "  (define result (and (= a 7) (= b 9))))");
    BOOST_CHECK_EQUAL(res, nanbox::True);
}

BOOST_AUTO_TEST_CASE(test_runtime_catch_nested_inner_matching_handler_wins) {
    LispVal res = run(
        "(module m"
        "  (define p"
        "    (catch 'runtime.error"
        "      (catch 'runtime.type-error"
        "        (car 42))))"
        "  (define result (eq? (car (cdr p)) 'runtime.type-error)))");
    BOOST_CHECK_EQUAL(res, nanbox::True);
}

/**
 * make-vector primitive
 */

BOOST_AUTO_TEST_CASE(test_make_vector_basic) {
    LispVal res = run(
        "(module m"
        "  (define v (make-vector 3 0))"
        "  (define result (vector-length v)))");
    BOOST_CHECK_EQUAL(nanbox::ops::decode<int64_t>(res).value(), 3);
}

BOOST_AUTO_TEST_CASE(test_make_vector_fill_value) {
    LispVal res = run(
        "(module m"
        "  (define v (make-vector 2 42))"
        "  (define result (vector-ref v 1)))");
    BOOST_CHECK_EQUAL(nanbox::ops::decode<int64_t>(res).value(), 42);
}

BOOST_AUTO_TEST_CASE(test_make_vector_set_and_ref) {
    LispVal res = run(
        "(module m"
        "  (define v (make-vector 3 0))"
        "  (vector-set! v 1 99)"
        "  (define result (vector-ref v 1)))");
    BOOST_CHECK_EQUAL(nanbox::ops::decode<int64_t>(res).value(), 99);
}

/**
 * define-record-type end-to-end
 */

BOOST_AUTO_TEST_CASE(test_record_type_construct_and_access) {
    LispVal res = run(
        "(module m"
        "  (define-record-type point"
        "    (make-point x y)"
        "    point?"
        "    (x point-x)"
        "    (y point-y))"
        "  (define p (make-point 3 4))"
        "  (define result (+ (point-x p) (point-y p))))");
    BOOST_CHECK_EQUAL(nanbox::ops::decode<int64_t>(res).value(), 7);
}

BOOST_AUTO_TEST_CASE(test_record_type_predicate_true) {
    LispVal res = run(
        "(module m"
        "  (define-record-type point"
        "    (make-point x y)"
        "    point?"
        "    (x point-x)"
        "    (y point-y))"
        "  (define p (make-point 1 2))"
        "  (define result (point? p)))");
    BOOST_CHECK_EQUAL(res, nanbox::True);
}

BOOST_AUTO_TEST_CASE(test_record_type_predicate_false_on_number) {
    LispVal res = run(
        "(module m"
        "  (define-record-type point"
        "    (make-point x y)"
        "    point?"
        "    (x point-x)"
        "    (y point-y))"
        "  (define result (point? 42)))");
    BOOST_CHECK_EQUAL(res, nanbox::False);
}

BOOST_AUTO_TEST_CASE(test_record_type_predicate_false_on_plain_vector) {
    LispVal res = run(
        "(module m"
        "  (define-record-type point"
        "    (make-point x y)"
        "    point?"
        "    (x point-x)"
        "    (y point-y))"
        "  (define result (point? (vector 1 2 3))))");
    BOOST_CHECK_EQUAL(res, nanbox::False);
}

BOOST_AUTO_TEST_CASE(test_record_type_mutator) {
    LispVal res = run(
        "(module m"
        "  (define-record-type mpoint"
        "    (make-mpoint x y)"
        "    mpoint?"
        "    (x mpoint-x set-mpoint-x!)"
        "    (y mpoint-y set-mpoint-y!))"
        "  (define p (make-mpoint 1 2))"
        "  (set-mpoint-x! p 10)"
        "  (define result (mpoint-x p)))");
    BOOST_CHECK_EQUAL(nanbox::ops::decode<int64_t>(res).value(), 10);
}

BOOST_AUTO_TEST_CASE(test_record_type_generative_identity) {
    /// Two record types with the same name should NOT be interchangeable (gensym tags)
    LispVal res = run(
        "(module m"
        "  (define-record-type point"
        "    (make-point1 x)"
        "    point1?"
        "    (x point1-x))"
        "  (define-record-type point"
        "    (make-point2 x)"
        "    point2?"
        "    (x point2-x))"
        "  (define p (make-point1 42))"
        "  (define result (point2? p)))");
    BOOST_CHECK_EQUAL(res, nanbox::False);
}

BOOST_AUTO_TEST_CASE(test_record_type_no_fields) {
    LispVal res = run(
        "(module m"
        "  (define-record-type unit"
        "    (make-unit)"
        "    unit?)"
        "  (define u (make-unit))"
        "  (define result (unit? u)))");
    BOOST_CHECK_EQUAL(res, nanbox::True);
}

BOOST_AUTO_TEST_CASE(test_record_type_mixed_readonly_mutable) {
    /// x is read-only, y is mutable
    LispVal res = run(
        "(module m"
        "  (define-record-type rec"
        "    (make-rec x y)"
        "    rec?"
        "    (x rec-x)"
        "    (y rec-y set-rec-y!))"
        "  (define r (make-rec 100 200))"
        "  (set-rec-y! r 999)"
        "  (define result (+ (rec-x r) (rec-y r))))");
    BOOST_CHECK_EQUAL(nanbox::ops::decode<int64_t>(res).value(), 1099);
}

/**
 * Apply tests (VM-level)
 */

BOOST_AUTO_TEST_CASE(test_apply_primitive) {
    /// apply with a primitive procedure
    LispVal res = run(
        "(module m"
        "  (define result (apply + '(1 2 3))))");
    BOOST_CHECK_EQUAL(nanbox::ops::decode<int64_t>(res).value(), 6);
}

BOOST_AUTO_TEST_CASE(test_apply_closure) {
    LispVal res = run(
        "(module m"
        "  (define (add a b) (+ a b))"
        "  (define result (apply add '(10 20))))");
    BOOST_CHECK_EQUAL(nanbox::ops::decode<int64_t>(res).value(), 30);
}

BOOST_AUTO_TEST_CASE(test_apply_with_leading_args) {
    LispVal res = run(
        "(module m"
        "  (define result (apply + 1 2 '(3 4))))");
    BOOST_CHECK_EQUAL(nanbox::ops::decode<int64_t>(res).value(), 10);
}

BOOST_AUTO_TEST_CASE(test_apply_empty_list) {
    /// apply with an empty tail list
    LispVal res = run(
        "(module m"
        "  (define (f x) (* x x))"
        "  (define result (apply f 5 '())))");
    BOOST_CHECK_EQUAL(nanbox::ops::decode<int64_t>(res).value(), 25);
}

BOOST_AUTO_TEST_CASE(test_apply_tail_position) {
    LispVal res = run(
        "(module m"
        "  (define (sum-list lst acc)"
        "    (if (null? lst)"
        "        acc"
        "        (sum-list (cdr lst) (+ acc (car lst)))))"
        "  (define result (apply sum-list '((1 2 3 4 5) 0))))");
    BOOST_CHECK_EQUAL(nanbox::ops::decode<int64_t>(res).value(), 15);
}

BOOST_AUTO_TEST_CASE(test_apply_closure_rest_args) {
    /// apply a closure that has rest args
    LispVal res = run(
        "(module m"
        "  (define (f a . rest) (apply + a rest))"
        "  (define result (apply f '(10 20 30))))");
    BOOST_CHECK_EQUAL(nanbox::ops::decode<int64_t>(res).value(), 60);
}

/**
 * Lambda rest parameter tests
 */

BOOST_AUTO_TEST_CASE(test_lambda_rest_basic) {
    LispVal res = run(
        "(module m"
        "  (define (f . args) (apply + args))"
        "  (define result (f 1 2 3 4)))");
    BOOST_CHECK_EQUAL(nanbox::ops::decode<int64_t>(res).value(), 10);
}

BOOST_AUTO_TEST_CASE(test_lambda_rest_with_required) {
    /// Rest param with required params
    LispVal res = run(
        "(module m"
        "  (define (f a b . rest) (+ a b (apply + rest)))"
        "  (define result (f 100 200 10 20)))");
    BOOST_CHECK_EQUAL(nanbox::ops::decode<int64_t>(res).value(), 330);
}

BOOST_AUTO_TEST_CASE(test_lambda_rest_empty) {
    LispVal res = run(
        "(module m"
        "  (define (f a . rest) (null? rest))"
        "  (define result (f 42)))");
    BOOST_CHECK_EQUAL(res, nanbox::True);
}

/**
 * Error primitive tests
 */

BOOST_AUTO_TEST_CASE(test_error_basic) {
    /// error should throw a runtime error
    BOOST_CHECK_THROW(
        run("(module m"
            "  (define result (error \"something went wrong\")))"),
        std::runtime_error);
}

BOOST_AUTO_TEST_CASE(test_error_with_irritants) {
    /// error with irritants should include them in the message
    try {
        run("(module m"
            "  (define result (error \"bad value\" 42)))");
        BOOST_FAIL("Expected runtime_error");
    } catch (const std::runtime_error& e) {
        std::string msg = e.what();
        BOOST_CHECK(msg.find("bad value") != std::string::npos);
        BOOST_CHECK(msg.find("42") != std::string::npos);
    }
}

BOOST_AUTO_TEST_CASE(test_error_conditional) {
    /// error only triggers on the failing path
    LispVal res = run(
        "(module m"
        "  (define (safe-div a b)"
        "    (if (= b 0)"
        "        (error \"division by zero\")"
        "        (/ a b)))"
        "  (define result (safe-div 10 2)))");
    BOOST_CHECK_EQUAL(nanbox::ops::decode<int64_t>(res).value(), 5);
}

/**
 * Boolean simplifier integration test
 *
 * A non-trivial symbolic computation: simplify boolean expression trees
 * represented as nested lists.  Exercises: defun, cond, let, and/or,
 * recursion, quoted symbols, eq?, equal?, list, car/cdr, pair?.
 *
 * Expression language:
 *
 * Simplification rules implemented:
 *   identity / annihilator   (and x #t)=x, (and x #f)=#f, etc.
 *   double negation          (not (not y))=y
 *   idempotence              (and x x)=x, (or x x)=x
 *   De Morgan                (not (and a b))=(or (not a) (not b))
 *   fixed-point iteration    keep simplifying until stable
 */

BOOST_AUTO_TEST_CASE(test_boolean_simplifier) {
    std::string src = R"(
        (module m
          ;; atom? â€” true for anything that is not a pair (symbols, bools, nil, numbersâ€¦)
          ;; This is a natural prelude candidate.
          (defun atom? (x) (not (pair? x)))

          ;; Helpers: extract operator and first/second argument
          (defun op (e) (car e))
          (defun a1 (e) (car (cdr e)))
          (defun a2 (e) (car (cdr (cdr e))))

          ;; One-step boolean simplifier on binary expression trees
          (defun simplify-bool (e)
            (cond
              ;; Atoms (variables or constants) return as-is
              ((atom? e) e)

              ;; not
              ((eq? (op e) 'not)
                (let ((u (simplify-bool (a1 e))))
                  (cond
                    ((and (atom? u) (eq? u #t)) #f)
                    ((and (atom? u) (eq? u #f)) #t)
                    ;; double negation: (not (not x)) => x
                    ((and (pair? u) (eq? (op u) 'not)) (a1 u))
                    ;; De Morgan: (not (and a b)) => (or (not a) (not b))
                    ((and (pair? u) (eq? (op u) 'and))
                      (list 'or  (list 'not (a1 u)) (list 'not (a2 u))))
                    ;; De Morgan: (not (or a b)) => (and (not a) (not b))
                    ((and (pair? u) (eq? (op u) 'or))
                      (list 'and (list 'not (a1 u)) (list 'not (a2 u))))
                    (#t (list 'not u)))))

              ;; and
              ((eq? (op e) 'and)
                (let ((sa (simplify-bool (a1 e)))
                      (sb (simplify-bool (a2 e))))
                  (cond
                    ((eq? sa #f) #f)
                    ((eq? sb #f) #f)
                    ((eq? sa #t) sb)
                    ((eq? sb #t) sa)
                    ((equal? sa sb) sa)
                    (#t (list 'and sa sb)))))

              ;; or
              ((eq? (op e) 'or)
                (let ((sa (simplify-bool (a1 e)))
                      (sb (simplify-bool (a2 e))))
                  (cond
                    ((eq? sa #t) #t)
                    ((eq? sb #t) #t)
                    ((eq? sa #f) sb)
                    ((eq? sb #f) sa)
                    ((equal? sa sb) sa)
                    (#t (list 'or sa sb)))))

              ;; Anything else passes through unchanged
              (#t e)))

          ;; Fixed-point wrapper: keep simplifying until stable
          (defun simplify-bool* (e)
            (let ((s (simplify-bool e)))
              (if (equal? s e) s (simplify-bool* s))))

          ;; Test cases
          ;; x âˆ§ âŠ¤ = x
          (define t1 (simplify-bool* '(and x #t)))
          ;; x âˆ§ âŠ¥ = âŠ¥
          (define t2 (simplify-bool* '(and x #f)))
          ;; (âŠ¤ âˆ§ x) âˆ¨ âŠ¥ = x
          (define t3 (simplify-bool* '(or (and #t x) #f)))
          ;; Â¬(Â¬y) = y
          (define t4 (simplify-bool* '(not (not y))))
          ;; Â¬(a âˆ§ b) = (Â¬a) âˆ¨ (Â¬b)   (De Morgan)
          (define t5 (simplify-bool* '(not (and a b))))

          ;; Verify each result
          (define r1 (eq? t1 'x))
          (define r2 (eq? t2 #f))
          (define r3 (eq? t3 'x))
          (define r4 (eq? t4 'y))
          (define r5 (equal? t5 '(or (not a) (not b))))

          ;; All checks must pass
          (define result (and r1 r2 r3 r4 r5)))
    )";

    LispVal res = run(src);
    /**
     * (and r1 r2 r3 r4 r5) returns the last truthy value when all pass,
     * or #f if any check fails.
     */
    BOOST_CHECK(res != nanbox::False);
}

BOOST_AUTO_TEST_CASE(test_boolean_simplifier_identity_and) {
    LispVal res = run(
        "(module m"
        "  (defun atom? (x) (not (pair? x)))"
        "  (defun op (e) (car e))"
        "  (defun a1 (e) (car (cdr e)))"
        "  (defun a2 (e) (car (cdr (cdr e))))"
        "  (defun simplify-bool (e)"
        "    (cond"
        "      ((atom? e) e)"
        "      ((eq? (op e) 'and)"
        "        (let ((sa (simplify-bool (a1 e)))"
        "              (sb (simplify-bool (a2 e))))"
        "          (cond"
        "            ((eq? sa #f) #f)"
        "            ((eq? sb #f) #f)"
        "            ((eq? sa #t) sb)"
        "            ((eq? sb #t) sa)"
        "            (#t (list 'and sa sb)))))"
        "      (#t e)))"
        "  (define result (eq? (simplify-bool '(and x #t)) 'x)))");
    BOOST_CHECK_EQUAL(res, nanbox::True);
}

BOOST_AUTO_TEST_CASE(test_boolean_simplifier_double_negation) {
    LispVal res = run(
        "(module m"
        "  (defun atom? (x) (not (pair? x)))"
        "  (defun op (e) (car e))"
        "  (defun a1 (e) (car (cdr e)))"
        "  (defun simplify-bool (e)"
        "    (cond"
        "      ((atom? e) e)"
        "      ((eq? (op e) 'not)"
        "        (let ((u (simplify-bool (a1 e))))"
        "          (cond"
        "            ((and (atom? u) (eq? u #t)) #f)"
        "            ((and (atom? u) (eq? u #f)) #t)"
        "            ((and (pair? u) (eq? (op u) 'not)) (a1 u))"
        "            (#t (list 'not u)))))"
        "      (#t e)))"
        "  (define result (eq? (simplify-bool '(not (not y))) 'y)))");
    BOOST_CHECK_EQUAL(res, nanbox::True);
}

BOOST_AUTO_TEST_CASE(test_boolean_simplifier_de_morgan) {
    LispVal res = run(
        "(module m"
        "  (defun atom? (x) (not (pair? x)))"
        "  (defun op (e) (car e))"
        "  (defun a1 (e) (car (cdr e)))"
        "  (defun a2 (e) (car (cdr (cdr e))))"
        "  (defun simplify-bool (e)"
        "    (cond"
        "      ((atom? e) e)"
        "      ((eq? (op e) 'not)"
        "        (let ((u (simplify-bool (a1 e))))"
        "          (cond"
        "            ((and (atom? u) (eq? u #t)) #f)"
        "            ((and (atom? u) (eq? u #f)) #t)"
        "            ((and (pair? u) (eq? (op u) 'not)) (a1 u))"
        "            ((and (pair? u) (eq? (op u) 'and))"
        "              (list 'or (list 'not (a1 u)) (list 'not (a2 u))))"
        "            ((and (pair? u) (eq? (op u) 'or))"
        "              (list 'and (list 'not (a1 u)) (list 'not (a2 u))))"
        "            (#t (list 'not u)))))"
        "      (#t e)))"
        "  (define result (equal? (simplify-bool '(not (and a b)))"
        "                         '(or (not a) (not b)))))");
    BOOST_CHECK_EQUAL(res, nanbox::True);
}

/**
 * Symbolic differentiation integration test
 *
 * A symbolic algebra system that computes derivatives of expression trees
 * and simplifies the result.  Exercises: defun, cond, let, recursion,
 * quoted symbols, eq?, equal?, =, list, car/cdr, pair?, number?, symbol?,
 * and arithmetic on both runtime values and symbolic trees.
 *
 * Expression language (binary operators):
 *
 * Differentiation rules:
 *   d(c)/dv = 0                       (constant)
 *   d(v)/dv = 1                       (identity)
 *   d(a+b)/dv = da/dv + db/dv         (sum rule)
 *   d(a*b)/dv = da*b + a*db           (product rule)
 *   d(u^n)/dv = n*u^(n-1)*du/dv       (power rule, numeric n)
 *   d(sin u)/dv = cos(u)*du/dv        (chain rule)
 *   d(cos u)/dv = -sin(u)*du/dv       (chain rule)
 *   d(exp u)/dv = exp(u)*du/dv        (chain rule)
 *   d(log u)/dv = (1/u)*du/dv         (chain rule)
 *
 * Simplification rules:
 *   0+b = b,  a+0 = a                 (additive identity)
 *   0*b = 0,  a*0 = 0                 (multiplicative annihilator)
 *   1*b = b,  a*1 = a                 (multiplicative identity)
 *   constant folding for + and *
 *   x+x = 2*x                         (combine like terms)
 *   fixed-point iteration
 */

BOOST_AUTO_TEST_CASE(test_symbolic_diff_comprehensive) {
    /// Full diff + simplify system with all rules, tested against 4 examples
    std::string src = R"(
        (module m
          ;; Prelude helpers
          (defun atom? (x) (not (pair? x)))
          (defun op (e) (car e))
          (defun a1 (e) (car (cdr e)))
          (defun a2 (e) (car (cdr (cdr e))))

          ;; Algebraic simplifier
          (defun simplify (e)
            (cond
              ;; atoms simplify to themselves
              ((atom? e) e)

              ;; (+ a b)
              ((eq? (op e) '+)
                (let ((sa (simplify (a1 e)))
                      (sb (simplify (a2 e))))
                  (cond
                    ((and (number? sa) (= sa 0)) sb)
                    ((and (number? sb) (= sb 0)) sa)
                    ((and (number? sa) (number? sb)) (+ sa sb))
                    ;; x + x => 2*x
                    ((equal? sa sb) (list '* 2 sa))
                    (#t (list '+ sa sb)))))

              ;; (* a b)
              ((eq? (op e) '*)
                (let ((sa (simplify (a1 e)))
                      (sb (simplify (a2 e))))
                  (cond
                    ((and (number? sa) (= sa 0)) 0)
                    ((and (number? sb) (= sb 0)) 0)
                    ((and (number? sa) (= sa 1)) sb)
                    ((and (number? sb) (= sb 1)) sa)
                    ((and (number? sa) (number? sb)) (* sa sb))
                    (#t (list '* sa sb)))))

              ;; default: pass through
              (#t e)))

          ;; Fixed-point simplifier
          (defun simplify* (e)
            (let ((s (simplify e)))
              (if (equal? s e) s (simplify* s))))

          ;; Symbolic differentiator
          (defun diff (e v)
            (cond
              ((number? e) 0)
              ((symbol? e) (if (eq? e v) 1 0))
              ;; sum rule
              ((eq? (op e) '+)
                (list '+ (diff (a1 e) v) (diff (a2 e) v)))
              ;; product rule
              ((eq? (op e) '*)
                (list '+ (list '* (diff (a1 e) v) (a2 e))
                         (list '* (a1 e) (diff (a2 e) v))))
              ;; power rule: (^ u n), numeric n
              ((eq? (op e) '^)
                (let ((u (a1 e))
                      (n (a2 e)))
                  (if (number? n)
                      (list '* n (list '* (list '^ u (- n 1)) (diff u v)))
                      0)))
              ;; chain rule: sin(u)
              ((eq? (op e) 'sin)
                (let ((u (a1 e)))
                  (list '* (list 'cos u) (diff u v))))
              ;; chain rule: cos(u) => -sin(u)*u'
              ((eq? (op e) 'cos)
                (let ((u (a1 e)))
                  (list '* -1 (list '* (list 'sin u) (diff u v)))))
              ;; chain rule: exp(u) => exp(u)*u'
              ((eq? (op e) 'exp)
                (let ((u (a1 e)))
                  (list '* (list 'exp u) (diff u v))))
              ;; chain rule: log(u) => (1/u)*u'
              ((eq? (op e) 'log)
                (let ((u (a1 e)))
                  (list '* (list '/ 1 u) (diff u v))))
              (#t 0)))

          ;; Test cases

          ;; 1) d/dx (xÂ² + 3) = 2x
          (define t1 (simplify* (diff '(+ (* x x) 3) 'x)))

          ;; 2) d/dx (x(x+3)) = (+ (+ x 3) x)
          (define t2 (simplify* (diff '(* x (+ x 3)) 'x)))

          ;; 3) d/dx sin(xÂ²) = (* (cos (* x x)) (* 2 x))
          (define t3 (simplify* (diff '(sin (* x x)) 'x)))

          ;; 4) d/dx (x+1)Â³ = (* 3 (^ (+ x 1) 2))
          (define t4 (simplify* (diff '(^ (+ x 1) 3) 'x)))

          ;; Verify each
          (define r1 (equal? t1 '(* 2 x)))
          (define r2 (equal? t2 '(+ (+ x 3) x)))
          (define r3 (equal? t3 '(* (cos (* x x)) (* 2 x))))
          (define r4 (equal? t4 '(* 3 (^ (+ x 1) 2))))

          (define result (and r1 r2 r3 r4)))
    )";

    LispVal res = run(src);
    BOOST_CHECK(res != nanbox::False);
}

BOOST_AUTO_TEST_CASE(test_symbolic_diff_constant) {
    /// d/dx(5) = 0
    LispVal res = run(
        "(module m"
        "  (defun atom? (x) (not (pair? x)))"
        "  (defun diff (e v)"
        "    (cond"
        "      ((number? e) 0)"
        "      ((symbol? e) (if (eq? e v) 1 0))"
        "      (#t 0)))"
        "  (define result (= (diff 5 'x) 0)))");
    BOOST_CHECK_EQUAL(res, nanbox::True);
}

BOOST_AUTO_TEST_CASE(test_symbolic_diff_identity_variable) {
    /// d/dx(x) = 1
    LispVal res = run(
        "(module m"
        "  (defun diff (e v)"
        "    (cond"
        "      ((number? e) 0)"
        "      ((symbol? e) (if (eq? e v) 1 0))"
        "      (#t 0)))"
        "  (define result (= (diff 'x 'x) 1)))");
    BOOST_CHECK_EQUAL(res, nanbox::True);
}

BOOST_AUTO_TEST_CASE(test_symbolic_diff_other_variable) {
    /// d/dx(y) = 0
    LispVal res = run(
        "(module m"
        "  (defun diff (e v)"
        "    (cond"
        "      ((number? e) 0)"
        "      ((symbol? e) (if (eq? e v) 1 0))"
        "      (#t 0)))"
        "  (define result (= (diff 'y 'x) 0)))");
    BOOST_CHECK_EQUAL(res, nanbox::True);
}

BOOST_AUTO_TEST_CASE(test_symbolic_diff_sum_rule) {
    /// d/dx(x + 3) = (+ 1 0)  (raw, unsimplified)
    LispVal res = run(
        "(module m"
        "  (defun atom? (x) (not (pair? x)))"
        "  (defun op (e) (car e))"
        "  (defun a1 (e) (car (cdr e)))"
        "  (defun a2 (e) (car (cdr (cdr e))))"
        "  (defun diff (e v)"
        "    (cond"
        "      ((number? e) 0)"
        "      ((symbol? e) (if (eq? e v) 1 0))"
        "      ((eq? (op e) '+)"
        "        (list '+ (diff (a1 e) v) (diff (a2 e) v)))"
        "      (#t 0)))"
        "  (define result (equal? (diff '(+ x 3) 'x)"
        "                         '(+ 1 0))))");
    BOOST_CHECK_EQUAL(res, nanbox::True);
}

BOOST_AUTO_TEST_CASE(test_symbolic_diff_product_rule) {
    /// d/dx(x*x) = (+ (* 1 x) (* x 1))  (raw)
    LispVal res = run(
        "(module m"
        "  (defun atom? (x) (not (pair? x)))"
        "  (defun op (e) (car e))"
        "  (defun a1 (e) (car (cdr e)))"
        "  (defun a2 (e) (car (cdr (cdr e))))"
        "  (defun diff (e v)"
        "    (cond"
        "      ((number? e) 0)"
        "      ((symbol? e) (if (eq? e v) 1 0))"
        "      ((eq? (op e) '+)"
        "        (list '+ (diff (a1 e) v) (diff (a2 e) v)))"
        "      ((eq? (op e) '*)"
        "        (list '+ (list '* (diff (a1 e) v) (a2 e))"
        "                 (list '* (a1 e) (diff (a2 e) v))))"
        "      (#t 0)))"
        "  (define result (equal? (diff '(* x x) 'x)"
        "                         '(+ (* 1 x) (* x 1)))))");
    BOOST_CHECK_EQUAL(res, nanbox::True);
}

BOOST_AUTO_TEST_CASE(test_symbolic_diff_power_rule) {
    /// d/dx(x^3) = (* 3 (* (^ x 2) 1))  (raw, before simplification)
    LispVal res = run(
        "(module m"
        "  (defun atom? (x) (not (pair? x)))"
        "  (defun op (e) (car e))"
        "  (defun a1 (e) (car (cdr e)))"
        "  (defun a2 (e) (car (cdr (cdr e))))"
        "  (defun diff (e v)"
        "    (cond"
        "      ((number? e) 0)"
        "      ((symbol? e) (if (eq? e v) 1 0))"
        "      ((eq? (op e) '^)"
        "        (let ((u (a1 e)) (n (a2 e)))"
        "          (if (number? n)"
        "              (list '* n (list '* (list '^ u (- n 1)) (diff u v)))"
        "              0)))"
        "      (#t 0)))"
        "  (define result (equal? (diff '(^ x 3) 'x)"
        "                         '(* 3 (* (^ x 2) 1)))))");
    BOOST_CHECK_EQUAL(res, nanbox::True);
}

BOOST_AUTO_TEST_CASE(test_symbolic_diff_chain_rule_sin) {
    /// d/dx(sin(x)) = (* (cos x) 1)
    LispVal res = run(
        "(module m"
        "  (defun atom? (x) (not (pair? x)))"
        "  (defun op (e) (car e))"
        "  (defun a1 (e) (car (cdr e)))"
        "  (defun diff (e v)"
        "    (cond"
        "      ((number? e) 0)"
        "      ((symbol? e) (if (eq? e v) 1 0))"
        "      ((eq? (op e) 'sin)"
        "        (let ((u (a1 e)))"
        "          (list '* (list 'cos u) (diff u v))))"
        "      (#t 0)))"
        "  (define result (equal? (diff '(sin x) 'x)"
        "                         '(* (cos x) 1))))");
    BOOST_CHECK_EQUAL(res, nanbox::True);
}

BOOST_AUTO_TEST_CASE(test_symbolic_simplify_additive_identity) {
    /// 0 + x => x,  x + 0 => x
    std::string src =
        "(module m"
        "  (defun atom? (x) (not (pair? x)))"
        "  (defun op (e) (car e))"
        "  (defun a1 (e) (car (cdr e)))"
        "  (defun a2 (e) (car (cdr (cdr e))))"
        "  (defun simplify (e)"
        "    (cond"
        "      ((atom? e) e)"
        "      ((eq? (op e) '+)"
        "        (let ((sa (simplify (a1 e)))"
        "              (sb (simplify (a2 e))))"
        "          (cond"
        "            ((and (number? sa) (= sa 0)) sb)"
        "            ((and (number? sb) (= sb 0)) sa)"
        "            (#t (list '+ sa sb)))))"
        "      (#t e)))"
        "  (define r1 (eq? (simplify '(+ 0 x)) 'x))"
        "  (define r2 (eq? (simplify '(+ x 0)) 'x))"
        "  (define result (and r1 r2)))";
    LispVal res = run(src);
    BOOST_CHECK(res != nanbox::False);
}

BOOST_AUTO_TEST_CASE(test_symbolic_simplify_multiplicative_rules) {
    /// 0*x => 0,  1*x => x,  x*1 => x
    std::string src =
        "(module m"
        "  (defun atom? (x) (not (pair? x)))"
        "  (defun op (e) (car e))"
        "  (defun a1 (e) (car (cdr e)))"
        "  (defun a2 (e) (car (cdr (cdr e))))"
        "  (defun simplify (e)"
        "    (cond"
        "      ((atom? e) e)"
        "      ((eq? (op e) '*)"
        "        (let ((sa (simplify (a1 e)))"
        "              (sb (simplify (a2 e))))"
        "          (cond"
        "            ((and (number? sa) (= sa 0)) 0)"
        "            ((and (number? sb) (= sb 0)) 0)"
        "            ((and (number? sa) (= sa 1)) sb)"
        "            ((and (number? sb) (= sb 1)) sa)"
        "            ((and (number? sa) (number? sb)) (* sa sb))"
        "            (#t (list '* sa sb)))))"
        "      (#t e)))"
        "  (define r1 (= (simplify '(* 0 x)) 0))"
        "  (define r2 (eq? (simplify '(* 1 x)) 'x))"
        "  (define r3 (eq? (simplify '(* x 1)) 'x))"
        "  (define result (and r1 r2 r3)))";
    LispVal res = run(src);
    BOOST_CHECK(res != nanbox::False);
}

BOOST_AUTO_TEST_CASE(test_symbolic_simplify_constant_folding) {
    /// (+ 2 3) => 5,  (* 4 5) => 20
    std::string src =
        "(module m"
        "  (defun atom? (x) (not (pair? x)))"
        "  (defun op (e) (car e))"
        "  (defun a1 (e) (car (cdr e)))"
        "  (defun a2 (e) (car (cdr (cdr e))))"
        "  (defun simplify (e)"
        "    (cond"
        "      ((atom? e) e)"
        "      ((eq? (op e) '+)"
        "        (let ((sa (simplify (a1 e)))"
        "              (sb (simplify (a2 e))))"
        "          (cond"
        "            ((and (number? sa) (= sa 0)) sb)"
        "            ((and (number? sb) (= sb 0)) sa)"
        "            ((and (number? sa) (number? sb)) (+ sa sb))"
        "            (#t (list '+ sa sb)))))"
        "      ((eq? (op e) '*)"
        "        (let ((sa (simplify (a1 e)))"
        "              (sb (simplify (a2 e))))"
        "          (cond"
        "            ((and (number? sa) (= sa 0)) 0)"
        "            ((and (number? sb) (= sb 0)) 0)"
        "            ((and (number? sa) (= sa 1)) sb)"
        "            ((and (number? sb) (= sb 1)) sa)"
        "            ((and (number? sa) (number? sb)) (* sa sb))"
        "            (#t (list '* sa sb)))))"
        "      (#t e)))"
        "  (define r1 (= (simplify '(+ 2 3)) 5))"
        "  (define r2 (= (simplify '(* 4 5)) 20))"
        "  (define result (and r1 r2)))";
    LispVal res = run(src);
    BOOST_CHECK(res != nanbox::False);
}

/**
 * syntax-rules macro tests (end-to-end)
 */

BOOST_AUTO_TEST_CASE(test_syntax_rules_basic) {
    LispVal res = run(
        "(module m"
        "  (define-syntax my-if"
        "    (syntax-rules ()"
        "      ((_ t c a) (if t c a))))"
        "  (define result (my-if #t 42 99)))");
    BOOST_CHECK_EQUAL(nanbox::ops::decode<int64_t>(res).value(), 42);
}

BOOST_AUTO_TEST_CASE(test_syntax_rules_ellipsis) {
    LispVal res = run(
        "(module m"
        "  (define-syntax my-list"
        "    (syntax-rules ()"
        "      ((_ x ...) (list x ...))))"
        "  (define result (length (my-list 1 2 3 4 5))))");
    BOOST_CHECK_EQUAL(nanbox::ops::decode<int64_t>(res).value(), 5);
}

BOOST_AUTO_TEST_CASE(test_syntax_rules_my_and) {
    LispVal res = run(
        "(module m"
        "  (define-syntax my-and"
        "    (syntax-rules ()"
        "      ((_) #t)"
        "      ((_ e) e)"
        "      ((_ e1 e2 ...) (if e1 (my-and e2 ...) #f))))"
        "  (define result (my-and #t #t 42)))");
    BOOST_CHECK_EQUAL(nanbox::ops::decode<int64_t>(res).value(), 42);
}

BOOST_AUTO_TEST_CASE(test_syntax_rules_my_and_short_circuit) {
    LispVal res = run(
        "(module m"
        "  (define-syntax my-and"
        "    (syntax-rules ()"
        "      ((_) #t)"
        "      ((_ e) e)"
        "      ((_ e1 e2 ...) (if e1 (my-and e2 ...) #f))))"
        "  (define result (my-and #f (error \"should not reach\"))))");
    BOOST_CHECK_EQUAL(res, nanbox::False);
}

BOOST_AUTO_TEST_CASE(test_syntax_rules_my_or) {
    LispVal res = run(
        "(module m"
        "  (define-syntax my-or"
        "    (syntax-rules ()"
        "      ((_) #f)"
        "      ((_ e) e)"
        "      ((_ e1 e2 ...) (let ((t e1)) (if t t (my-or e2 ...))))))"
        "  (define result (my-or #f #f 99)))");
    BOOST_CHECK_EQUAL(nanbox::ops::decode<int64_t>(res).value(), 99);
}

BOOST_AUTO_TEST_CASE(test_syntax_rules_when_macro) {
    LispVal res = run(
        "(module m"
        "  (define-syntax my-when"
        "    (syntax-rules ()"
        "      ((_ test body ...) (if test (begin body ...) (begin)))))"
        "  (define x 0)"
        "  (my-when #t (set! x 42))"
        "  (define result x))");
    BOOST_CHECK_EQUAL(nanbox::ops::decode<int64_t>(res).value(), 42);
}

BOOST_AUTO_TEST_CASE(test_syntax_rules_chained) {
    LispVal res = run(
        "(module m"
        "  (define-syntax double"
        "    (syntax-rules ()"
        "      ((_ x) (+ x x))))"
        "  (define-syntax quadruple"
        "    (syntax-rules ()"
        "      ((_ x) (double (double x)))))"
        "  (define result (quadruple 3)))");
    BOOST_CHECK_EQUAL(nanbox::ops::decode<int64_t>(res).value(), 12);
}

BOOST_AUTO_TEST_CASE(test_syntax_rules_with_literal) {
    LispVal res = run(
        "(module m"
        "  (define-syntax my-case"
        "    (syntax-rules (else)"
        "      ((_ (else e)) e)"
        "      ((_ (t e)) (if t e (begin)))))"
        "  (define result (my-case (else 77))))");
    BOOST_CHECK_EQUAL(nanbox::ops::decode<int64_t>(res).value(), 77);
}

BOOST_AUTO_TEST_CASE(test_syntax_rules_hygiene) {
    /**
     * The macro introduces a 'tmp' binding. The user also has a 'tmp' variable.
     * Hygiene should prevent the macro's 'tmp' from capturing the user's 'tmp'.
     */
    LispVal res = run(
        "(module m"
        "  (define-syntax my-or2"
        "    (syntax-rules ()"
        "      ((_ a b) (let ((tmp a)) (if tmp tmp b)))))"
        "  (define tmp 42)"
        "  (define result (my-or2 #f tmp)))");
    BOOST_CHECK_EQUAL(nanbox::ops::decode<int64_t>(res).value(), 42);
}

/**
 * Unification tests
 */

BOOST_FIXTURE_TEST_SUITE(unification_tests, VMTestFixture)

BOOST_AUTO_TEST_CASE(logic_var_is_unbound_after_creation) {
    /**
     * A freshly created logic variable should display as _G<id>
     * and logic-var? should return #t
     */
    LispVal res = run(
        "(module m"
        "  (define x (logic-var))"
        "  (define result (logic-var? x)))");
    BOOST_CHECK_EQUAL(res, nanbox::True);
}

BOOST_AUTO_TEST_CASE(logic_var_predicate_false_for_non_lvar) {
    LispVal res = run(
        "(module m"
        "  (define result (logic-var? 42)))");
    BOOST_CHECK_EQUAL(res, nanbox::False);
}

BOOST_AUTO_TEST_CASE(unify_identical_atoms) {
    LispVal res = run(
        "(module m"
        "  (define result (unify 1 1)))");
    BOOST_CHECK_EQUAL(res, nanbox::True);
}

BOOST_AUTO_TEST_CASE(unify_mismatched_atoms) {
    LispVal res = run(
        "(module m"
        "  (define result (unify 1 2)))");
    BOOST_CHECK_EQUAL(res, nanbox::False);
}

BOOST_AUTO_TEST_CASE(unify_variable_binds_and_deref) {
    LispVal res = run(
        "(module m"
        "  (define x (logic-var))"
        "  (unify x 42)"
        "  (define result (deref-lvar x)))");
    auto v = nanbox::ops::decode<int64_t>(res);
    BOOST_REQUIRE(v.has_value());
    BOOST_CHECK_EQUAL(*v, 42);
}

BOOST_AUTO_TEST_CASE(unify_two_vars_then_deref) {
    LispVal res = run(
        "(module m"
        "  (define x (logic-var))"
        "  (define y (logic-var))"
        "  (unify x y)"
        "  (unify y 99)"
        "  (define result (deref-lvar x)))");
    auto v = nanbox::ops::decode<int64_t>(res);
    BOOST_REQUIRE(v.has_value());
    BOOST_CHECK_EQUAL(*v, 99);
}

BOOST_AUTO_TEST_CASE(unify_list_patterns) {
    /**
     * (unify '(x 2 3) '(1 y 3)) should bind x=1, y=2
     * Build lists with cons to avoid requiring std.core
     */
    LispVal res = run(
        "(module m"
        "  (define x (logic-var))"
        "  (define y (logic-var))"
        "  (define l1 (cons x (cons 2 (cons 3 '()))))"
        "  (define l2 (cons 1 (cons y (cons 3 '()))))"
        "  (unify l1 l2)"
        "  (define result (+ (deref-lvar x) (deref-lvar y))))");
    auto v = nanbox::ops::decode<int64_t>(res);
    BOOST_REQUIRE(v.has_value());
    BOOST_CHECK_EQUAL(*v, 3);  ///< x=1, y=2, sum=3
}

BOOST_AUTO_TEST_CASE(unify_occurs_check_rejects_cycle) {
    /// (unify x (cons x '())) must fail due to the occurs check
    LispVal res = run(
        "(module m"
        "  (define x (logic-var))"
        "  (define result (unify x (cons x '()))))");
    BOOST_CHECK_EQUAL(res, nanbox::False);
}

BOOST_AUTO_TEST_CASE(trail_mark_and_unwind) {
    /// Bind a variable, record the mark, unwind, and verify the variable is unbound again
    LispVal res = run(
        "(module m"
        "  (define x (logic-var))"
        "  (define mark (trail-mark))"
        "  (unify x 77)"                         ///< x = 77
        "  (unwind-trail mark)"
        "  (define result (logic-var? (deref-lvar x))))");
    /**
     * After unwinding, deref-lvar x returns x itself (still a LogicVar)
     * so logic-var? should be #t
     */
    BOOST_CHECK_EQUAL(res, nanbox::True);
}

BOOST_AUTO_TEST_CASE(backtrack_restores_multiple_bindings) {
    LispVal res = run(
        "(module m"
        "  (define x (logic-var))"
        "  (define y (logic-var))"
        "  (define mark (trail-mark))"
        "  (unify x 10)"
        "  (unify y 20)"
        "  (unwind-trail mark)"
        "  (define result"
        "    (if (and (logic-var? (deref-lvar x))"
        "             (logic-var? (deref-lvar y)))"
        "        1 0)))");
    auto v = nanbox::ops::decode<int64_t>(res);
    BOOST_REQUIRE(v.has_value());
    BOOST_CHECK_EQUAL(*v, 1);
}

BOOST_AUTO_TEST_SUITE_END()

/**
 * copy-term opcode tests
 */

BOOST_FIXTURE_TEST_SUITE(copy_term_tests, VMTestFixture)

BOOST_AUTO_TEST_CASE(copy_term_ground_value_unchanged) {
    /// Copying a ground integer returns the same integer
    LispVal res = run(
        "(module m"
        "  (define result (copy-term 42)))");
    auto v = nanbox::ops::decode<int64_t>(res);
    BOOST_REQUIRE(v.has_value());
    BOOST_CHECK_EQUAL(*v, 42);
}

BOOST_AUTO_TEST_CASE(copy_term_ground_list_unchanged) {
    /// Copying a ground list returns an equal list
    LispVal res = run(
        "(module m"
        "  (define lst (cons 1 (cons 2 (cons 3 '()))))"
        "  (define c (copy-term lst))"
        "  (define result (+ (car c) (+ (car (cdr c)) (car (cdr (cdr c)))))))");
    auto v = nanbox::ops::decode<int64_t>(res);
    BOOST_REQUIRE(v.has_value());
    BOOST_CHECK_EQUAL(*v, 6);  ///< 1 + 2 + 3
}

BOOST_AUTO_TEST_CASE(copy_term_unbound_var_creates_fresh) {
    /// Copying an unbound variable yields a different unbound variable
    LispVal res = run(
        "(module m"
        "  (define x (logic-var))"
        "  (define y (copy-term x))"
        "  (define result (and (logic-var? (deref-lvar y))"
        "                      (not (eq? x y)))))");
    BOOST_CHECK_EQUAL(res, nanbox::True);
}

BOOST_AUTO_TEST_CASE(copy_term_preserves_sharing) {
    /// The same unbound variable appearing twice maps to the same fresh copy
    LispVal res = run(
        "(module m"
        "  (define x (logic-var))"
        "  (define pair (cons x x))"
        "  (define c (copy-term pair))"
        "  (unify (car c) 99)"
        "  (define result (deref-lvar (cdr c))))");
    auto v = nanbox::ops::decode<int64_t>(res);
    BOOST_REQUIRE(v.has_value());
    BOOST_CHECK_EQUAL(*v, 99);  ///< cdr(c) is the same fresh var as car(c)
}

BOOST_AUTO_TEST_CASE(copy_term_does_not_affect_original) {
    /// Binding the copy does not affect the original template
    LispVal res = run(
        "(module m"
        "  (define x (logic-var))"
        "  (define tmpl (cons x (logic-var)))"
        "  (define c (copy-term tmpl))"
        "  (unify (car c) 'hello)"
        "  (define result (logic-var? (deref-lvar x))))");
    BOOST_CHECK_EQUAL(res, nanbox::True);
}

BOOST_AUTO_TEST_CASE(copy_term_mixed_ground_and_vars) {
    /// A list with both ground values and unbound vars copies correctly
    LispVal res = run(
        "(module m"
        "  (define x (logic-var))"
        "  (define lst (cons 1 (cons x (cons 3 '()))))"
        "  (define c (copy-term lst))"
        "  (unify (car (cdr c)) 2)"
        "  (define result (+ (car c) (+ (deref-lvar (car (cdr c)))"
        "                                (car (cdr (cdr c)))))))");
    auto v = nanbox::ops::decode<int64_t>(res);
    BOOST_REQUIRE(v.has_value());
    BOOST_CHECK_EQUAL(*v, 6);  ///< 1 + 2 + 3
}

BOOST_AUTO_TEST_CASE(copy_term_bound_var_copies_value) {
    /// A bound variable is dereferenced; the copy contains the ground value
    LispVal res = run(
        "(module m"
        "  (define x (logic-var))"
        "  (unify x 42)"
        "  (define c (copy-term x))"
        "  (define result c))");
    auto v = nanbox::ops::decode<int64_t>(res);
    BOOST_REQUIRE(v.has_value());
    BOOST_CHECK_EQUAL(*v, 42);
}

BOOST_AUTO_TEST_CASE(copy_term_nested_pairs) {
    /// Deep nested structure with variables at various depths
    LispVal res = run(
        "(module m"
        "  (define a (logic-var))"
        "  (define b (logic-var))"
        "  (define tmpl (cons (cons a b) (cons 'x a)))"
        "  (define c (copy-term tmpl))"
        "  (unify (car (car c)) 10)"
        "  (define result (deref-lvar (cdr (cdr c)))))");
    auto v = nanbox::ops::decode<int64_t>(res);
    BOOST_REQUIRE(v.has_value());
    BOOST_CHECK_EQUAL(*v, 10);  ///< sharing preserved across nesting
}

BOOST_AUTO_TEST_SUITE_END()

/**
 * fact-table builtin tests
 */

BOOST_FIXTURE_TEST_SUITE(fact_table_tests, VMTestFixture)

BOOST_AUTO_TEST_CASE(fact_table_predicate) {
    LispVal res = run(
        "(module m"
        "  (define ft (%make-fact-table '(name age)))"
        "  (define result (fact-table? ft)))");
    BOOST_CHECK_EQUAL(res, nanbox::True);
}

BOOST_AUTO_TEST_CASE(fact_table_predicate_false_for_non_table) {
    LispVal res = run(
        "(module m"
        "  (define result (fact-table? 42)))");
    BOOST_CHECK_EQUAL(res, nanbox::False);
}

BOOST_AUTO_TEST_CASE(fact_table_insert_and_row_count) {
    LispVal res = run(
        "(module m"
        "  (define ft (%make-fact-table '(a b)))"
        "  (%fact-table-insert! ft '(1 2))"
        "  (%fact-table-insert! ft '(3 4))"
        "  (%fact-table-insert! ft '(5 6))"
        "  (define result (%fact-table-row-count ft)))");
    auto v = nanbox::ops::decode<int64_t>(res);
    BOOST_REQUIRE(v.has_value());
    BOOST_CHECK_EQUAL(*v, 3);
}

BOOST_AUTO_TEST_CASE(fact_table_ref_cell) {
    LispVal res = run(
        "(module m"
        "  (define ft (%make-fact-table '(x y)))"
        "  (%fact-table-insert! ft '(10 20))"
        "  (%fact-table-insert! ft '(30 40))"
        "  (define result (+ (%fact-table-ref ft 0 0)"
        "                    (%fact-table-ref ft 1 1))))");
    auto v = nanbox::ops::decode<int64_t>(res);
    BOOST_REQUIRE(v.has_value());
    BOOST_CHECK_EQUAL(*v, 50);  ///< 10 + 40
}

BOOST_AUTO_TEST_CASE(fact_table_query_linear_scan) {
    LispVal res = run(
        "(module m"
        "  (define ft (%make-fact-table '(name val)))"
        "  (%fact-table-insert! ft '(a 1))"
        "  (%fact-table-insert! ft '(b 2))"
        "  (%fact-table-insert! ft '(a 3))"
        "  (define rows (%fact-table-query ft 0 'a))"
        "  (define result (length rows)))");
    auto v = nanbox::ops::decode<int64_t>(res);
    BOOST_REQUIRE(v.has_value());
    BOOST_CHECK_EQUAL(*v, 2);
}

BOOST_AUTO_TEST_CASE(fact_table_query_with_index) {
    LispVal res = run(
        "(module m"
        "  (define ft (%make-fact-table '(name val)))"
        "  (%fact-table-insert! ft '(a 1))"
        "  (%fact-table-insert! ft '(b 2))"
        "  (%fact-table-insert! ft '(a 3))"
        "  (%fact-table-build-index! ft 0)"
        "  (define rows (%fact-table-query ft 0 'a))"
        "  (define result (length rows)))");
    auto v = nanbox::ops::decode<int64_t>(res);
    BOOST_REQUIRE(v.has_value());
    BOOST_CHECK_EQUAL(*v, 2);
}

BOOST_AUTO_TEST_CASE(fact_table_query_returns_correct_row_ids) {
    LispVal res = run(
        "(module m"
        "  (define ft (%make-fact-table '(k v)))"
        "  (%fact-table-insert! ft '(x 10))"
        "  (%fact-table-insert! ft '(y 20))"
        "  (%fact-table-insert! ft '(x 30))"
        "  (%fact-table-build-index! ft 0)"
        "  (define rows (%fact-table-query ft 0 'x))"
        "  (define v1 (%fact-table-ref ft (car rows) 1))"
        "  (define v2 (%fact-table-ref ft (car (cdr rows)) 1))"
        "  (define result (+ v1 v2)))");
    auto v = nanbox::ops::decode<int64_t>(res);
    BOOST_REQUIRE(v.has_value());
    BOOST_CHECK_EQUAL(*v, 40);  ///< 10 + 30
}

BOOST_AUTO_TEST_CASE(fact_table_query_no_match) {
    LispVal res = run(
        "(module m"
        "  (define ft (%make-fact-table '(a)))"
        "  (%fact-table-insert! ft '(1))"
        "  (define rows (%fact-table-query ft 0 999))"
        "  (define result (null? rows)))");
    BOOST_CHECK_EQUAL(res, nanbox::True);
}

BOOST_AUTO_TEST_CASE(fact_table_insert_arity_mismatch_errors) {
    BOOST_CHECK_THROW(
        run("(module m"
            "  (define ft (%make-fact-table '(a b)))"
            "  (define result (%fact-table-insert! ft '(1))))"),
        std::runtime_error);
}

BOOST_AUTO_TEST_CASE(fact_table_incremental_index_update) {
    LispVal res = run(
        "(module m"
        "  (define ft (%make-fact-table '(k v)))"
        "  (%fact-table-insert! ft '(a 1))"
        "  (%fact-table-build-index! ft 0)"
        "  (%fact-table-insert! ft '(a 2))"
        "  (define rows (%fact-table-query ft 0 'a))"
        "  (define result (length rows)))");
    auto v = nanbox::ops::decode<int64_t>(res);
    BOOST_REQUIRE(v.has_value());
    BOOST_CHECK_EQUAL(*v, 2);
}

BOOST_AUTO_TEST_CASE(fact_table_delete_row_updates_visibility_and_count) {
    LispVal res = run(
        "(module m"
        "  (define ft (%make-fact-table '(k v)))"
        "  (%fact-table-insert! ft '(a 1))"
        "  (%fact-table-insert! ft '(a 2))"
        "  (%fact-table-insert! ft '(b 3))"
        "  (%fact-table-build-index! ft 0)"
        "  (%fact-table-delete-row! ft 1)"
        "  (define rows (%fact-table-query ft 0 'a))"
        "  (define result (+ (* 100 (%fact-table-row-count ft))"
        "                    (* 10 (length rows))"
        "                    (if (%fact-table-row-live? ft 1) 1 0))))");
    auto v = nanbox::ops::decode<int64_t>(res);
    BOOST_REQUIRE(v.has_value());
    BOOST_CHECK_EQUAL(*v, 210);  ///< row-count=2, query-len=1, deleted row not live.
}

BOOST_AUTO_TEST_CASE(fact_table_predicate_header_round_trip) {
    LispVal res = run(
        "(module m"
        "  (define ft (%make-fact-table '(x y)))"
        "  (%fact-table-set-predicate! ft 'edge 2)"
        "  (define hdr (%fact-table-predicate ft))"
        "  (define result (and (eq? (car hdr) 'edge)"
        "                      (= (car (cdr hdr)) 2))))");
    BOOST_CHECK_EQUAL(res, nanbox::True);
}

BOOST_AUTO_TEST_CASE(fact_table_clause_metadata_and_rule_column) {
    LispVal res = run(
        "(module m"
        "  (define ft (%make-fact-table '(x y)))"
        "  (%fact-table-insert-clause! ft '(a b) #f #t)"
        "  (%fact-table-insert-clause! ft '(?x ?y) (lambda (x y) #t) #f)"
        "  (define result (and (%fact-table-row-ground? ft 0)"
        "                      (not (%fact-table-row-ground? ft 1))"
        "                      (procedure? (%fact-table-row-rule ft 1)))))");
    BOOST_CHECK_EQUAL(res, nanbox::True);
}

BOOST_AUTO_TEST_CASE(fact_table_live_row_ids_excludes_tombstones) {
    LispVal res = run(
        "(module m"
        "  (define ft (%make-fact-table '(k)))"
        "  (%fact-table-insert! ft '(a))"
        "  (%fact-table-insert! ft '(b))"
        "  (%fact-table-insert! ft '(c))"
        "  (%fact-table-delete-row! ft 1)"
        "  (define ids (%fact-table-live-row-ids ft))"
        "  (define result (+ (car ids) (car (cdr ids)))))");
    auto v = nanbox::ops::decode<int64_t>(res);
    BOOST_REQUIRE(v.has_value());
    BOOST_CHECK_EQUAL(*v, 2);  ///< ids are (0 2)
}

BOOST_AUTO_TEST_CASE(fact_table_column_names_round_trip) {
    LispVal res = run(
        "(module m"
        "  (define ft (%make-fact-table '(k v)))"
        "  (define names (%fact-table-column-names ft))"
        "  (define result (and (eq? (car names) 'k)"
        "                      (eq? (car (cdr names)) 'v))))");
    BOOST_CHECK_EQUAL(res, nanbox::True);
}

BOOST_AUTO_TEST_CASE(fact_table_group_count_counts_live_rows_only) {
    LispVal res = run(
        "(module m"
        "  (define ft (%make-fact-table '(k v)))"
        "  (%fact-table-insert! ft '(a 10))"
        "  (%fact-table-insert! ft '(a 20))"
        "  (%fact-table-insert! ft '(b 30))"
        "  (%fact-table-delete-row! ft 1)"
        "  (define grouped (%fact-table-group-count ft 0))"
        "  (define a-count (cdr (assq 'a grouped)))"
        "  (define b-count (cdr (assq 'b grouped)))"
        "  (define result (+ (* 10 a-count) b-count)))");
    auto v = nanbox::ops::decode<int64_t>(res);
    BOOST_REQUIRE(v.has_value());
    BOOST_CHECK_EQUAL(*v, 11);  ///< a=1, b=1
}

BOOST_AUTO_TEST_CASE(fact_table_group_sum_accumulates_by_key) {
    LispVal res = run(
        "(module m"
        "  (define ft (%make-fact-table '(k v)))"
        "  (%fact-table-insert! ft '(a 1))"
        "  (%fact-table-insert! ft '(a 3))"
        "  (%fact-table-insert! ft '(b 2))"
        "  (define grouped (%fact-table-group-sum ft 0 1))"
        "  (define a-sum (cdr (assq 'a grouped)))"
        "  (define b-sum (cdr (assq 'b grouped)))"
        "  (define result (+ (* 10 a-sum) b-sum)))");
    auto v = nanbox::ops::decode<int64_t>(res);
    BOOST_REQUIRE(v.has_value());
    BOOST_CHECK_EQUAL(*v, 42);  ///< a=4, b=2
}

BOOST_AUTO_TEST_CASE(fact_table_group_sum_promotes_to_flonum) {
    LispVal res = run(
        "(module m"
        "  (define ft (%make-fact-table '(k v)))"
        "  (%fact-table-insert! ft '(a 1))"
        "  (%fact-table-insert! ft '(a 2.5))"
        "  (define grouped (%fact-table-group-sum ft 0 1))"
        "  (define result (cdr (assq 'a grouped))))");
    auto d = nanbox::ops::decode<double>(res);
    BOOST_REQUIRE(d.has_value());
    BOOST_CHECK_CLOSE(*d, 3.5, 1e-10);
}

BOOST_AUTO_TEST_CASE(fact_table_group_sum_errors_on_non_numeric_value_column) {
    BOOST_CHECK_THROW(
        run("(module m"
            "  (define ft (%make-fact-table '(k v)))"
            "  (%fact-table-insert! ft '(a 'x))"
            "  (define result (%fact-table-group-sum ft 0 1)))"),
        std::runtime_error);
}

BOOST_AUTO_TEST_CASE(term_hash_structural_consistency) {
    LispVal res = run(
        "(module m"
        "  (define t1 (term 'pair 1 (list 2 3)))"
        "  (define t2 (term 'pair 1 (list 2 3)))"
        "  (define t3 (term 'pair 1 (list 2 4)))"
        "  (define h1 (term-hash t1 8))"
        "  (define h2 (term-hash t2 8))"
        "  (define h3 (term-hash t3 8))"
        "  (define result (and (= h1 h2) (not (= h1 h3)))))");
    BOOST_CHECK_EQUAL(res, nanbox::True);
}

BOOST_AUTO_TEST_CASE(term_variant_hash_ignores_logic_var_identity) {
    LispVal res = run(
        "(module m"
        "  (define x1 (logic-var))"
        "  (define y1 (logic-var))"
        "  (define x2 (logic-var))"
        "  (define y2 (logic-var))"
        "  (define h1 (term-variant-hash (list x1 x1 y1) 8))"
        "  (define h2 (term-variant-hash (list x2 x2 y2) 8))"
        "  (define h3 (term-variant-hash (list x2 y2 y2) 8))"
        "  (define result (and (= h1 h2) (not (= h1 h3)))))");
    BOOST_CHECK_EQUAL(res, nanbox::True);
}

BOOST_AUTO_TEST_SUITE_END() ///< fact_table_tests

/**
 * VM finalizer safe-point tests
 */

BOOST_AUTO_TEST_SUITE(finalizer_vm_tests)

struct FinalizerVMFixture {
    memory::heap::Heap heap{1024 * 1024};
    memory::intern::InternTable intern_table;

    static BytecodeFunction constant_return_function(LispVal value) {
        BytecodeFunction func;
        func.name = "finalizer_safe_point";
        func.stack_size = 1;
        func.constants.push_back(value);
        func.code.push_back({OpCode::LoadConst, 0u});
        func.code.push_back({OpCode::Return, 0u});
        return func;
    }

    std::expected<LispVal, RuntimeError> execute_constant(VM& vm, LispVal value) {
        auto func = constant_return_function(value);
        return vm.execute(func);
    }
};

BOOST_FIXTURE_TEST_CASE(pending_finalizer_executes_once_and_mutates_state, FinalizerVMFixture) {
    VM vm(heap, intern_table);
    int call_count = 0;

    auto obj = make_vector(heap, {});
    BOOST_REQUIRE(obj.has_value());

    auto proc = make_primitive(
        heap,
        [&call_count, expected_obj = *obj](const std::vector<LispVal>& args) -> std::expected<LispVal, RuntimeError> {
            if (args.size() != 1u || args[0] != expected_obj) {
                return std::unexpected(RuntimeError{VMError{
                    RuntimeErrorCode::TypeError,
                    "finalizer: unexpected argument"}});
            }
            ++call_count;
            return Nil;
        },
        1,
        false
    );
    BOOST_REQUIRE(proc.has_value());

    heap.enqueue_pending_finalizer(*obj, *proc);
    auto forty_two = nanbox::ops::encode<int64_t>(42);
    BOOST_REQUIRE(forty_two.has_value());

    auto exec_res = execute_constant(vm, *forty_two);
    BOOST_REQUIRE(exec_res.has_value());
    BOOST_TEST(exec_res.value() == *forty_two);
    BOOST_TEST(call_count == 1);
    BOOST_TEST(heap.pending_finalizer_count() == 0u);

    auto second_exec = execute_constant(vm, *forty_two);
    BOOST_REQUIRE(second_exec.has_value());
    BOOST_TEST(call_count == 1);
}

BOOST_FIXTURE_TEST_CASE(failing_finalizer_does_not_block_later_entries, FinalizerVMFixture) {
    VM vm(heap, intern_table);
    int failing_calls = 0;
    int succeeding_calls = 0;

    auto bad_obj = make_vector(heap, {});
    auto good_obj = make_vector(heap, {});
    BOOST_REQUIRE(bad_obj.has_value());
    BOOST_REQUIRE(good_obj.has_value());

    auto bad_proc = make_primitive(
        heap,
        [&failing_calls](const std::vector<LispVal>&) -> std::expected<LispVal, RuntimeError> {
            ++failing_calls;
            return std::unexpected(RuntimeError{VMError{
                RuntimeErrorCode::UserError,
                "finalizer failure"}});
        },
        1,
        false
    );
    BOOST_REQUIRE(bad_proc.has_value());

    auto good_proc = make_primitive(
        heap,
        [&succeeding_calls](const std::vector<LispVal>&) -> std::expected<LispVal, RuntimeError> {
            ++succeeding_calls;
            return Nil;
        },
        1,
        false
    );
    BOOST_REQUIRE(good_proc.has_value());

    heap.enqueue_pending_finalizer(*bad_obj, *bad_proc);
    heap.enqueue_pending_finalizer(*good_obj, *good_proc);

    auto result_val = nanbox::ops::encode<int64_t>(7);
    BOOST_REQUIRE(result_val.has_value());
    auto exec_res = execute_constant(vm, *result_val);
    BOOST_REQUIRE(exec_res.has_value());
    BOOST_TEST(exec_res.value() == *result_val);

    BOOST_TEST(failing_calls == 1);
    BOOST_TEST(succeeding_calls == 1);
    BOOST_TEST(heap.pending_finalizer_count() == 0u);
}

BOOST_FIXTURE_TEST_CASE(non_resurrected_finalized_object_is_reclaimed_later, FinalizerVMFixture) {
    VM vm(heap, intern_table);

    auto obj = make_vector(heap, {});
    BOOST_REQUIRE(obj.has_value());
    const auto obj_id = static_cast<memory::heap::ObjectId>(nanbox::ops::payload(*obj));

    auto proc = make_primitive(
        heap,
        [](const std::vector<LispVal>&) -> std::expected<LispVal, RuntimeError> {
            return Nil;
        },
        1,
        false
    );
    BOOST_REQUIRE(proc.has_value());
    BOOST_REQUIRE(heap.register_finalizer(obj_id, *proc).has_value());

    vm.collect_garbage();
    BOOST_TEST(heap.pending_finalizer_count() == 1u);

    memory::heap::HeapEntry entry{};
    BOOST_TEST(heap.try_get(obj_id, entry));

    auto zero = nanbox::ops::encode<int64_t>(0);
    BOOST_REQUIRE(zero.has_value());
    auto exec_res = execute_constant(vm, *zero);
    BOOST_REQUIRE(exec_res.has_value());
    BOOST_TEST(heap.pending_finalizer_count() == 0u);

    vm.collect_garbage();
    BOOST_TEST(!heap.try_get(obj_id, entry));
}

BOOST_FIXTURE_TEST_CASE(resurrected_finalized_object_survives_next_collection, FinalizerVMFixture) {
    VM vm(heap, intern_table);
    vm.globals().resize(1, Nil);

    int call_count = 0;
    auto obj = make_vector(heap, {});
    BOOST_REQUIRE(obj.has_value());
    const auto obj_id = static_cast<memory::heap::ObjectId>(nanbox::ops::payload(*obj));

    auto proc = make_primitive(
        heap,
        [&vm, &call_count](const std::vector<LispVal>& args) -> std::expected<LispVal, RuntimeError> {
            if (args.size() != 1u) {
                return std::unexpected(RuntimeError{VMError{
                    RuntimeErrorCode::TypeError,
                    "finalizer: unexpected argument count"}});
            }
            ++call_count;
            vm.globals()[0] = args[0];
            return Nil;
        },
        1,
        false
    );
    BOOST_REQUIRE(proc.has_value());
    BOOST_REQUIRE(heap.register_finalizer(obj_id, *proc).has_value());

    vm.collect_garbage();
    BOOST_TEST(heap.pending_finalizer_count() == 1u);

    vm.drain_finalizers_for_test();
    BOOST_TEST(call_count == 1);
    BOOST_TEST(vm.globals()[0] == *obj);
    BOOST_TEST(heap.pending_finalizer_count() == 0u);

    memory::heap::HeapEntry entry{};
    vm.collect_garbage();
    BOOST_TEST(heap.try_get(obj_id, entry));

    vm.globals()[0] = Nil;
    vm.collect_garbage();
    BOOST_TEST(!heap.try_get(obj_id, entry));

    vm.collect_garbage();
    BOOST_TEST(call_count == 1);
}

BOOST_FIXTURE_TEST_CASE(finalizer_and_guardian_on_same_object_deliver_once_each, FinalizerVMFixture) {
    VM vm(heap, intern_table);
    vm.globals().resize(1, Nil);

    auto guardian = make_guardian(heap);
    BOOST_REQUIRE(guardian.has_value());
    vm.globals()[0] = *guardian; ///< Keep guardian live across GC.

    auto obj = make_vector(heap, {});
    BOOST_REQUIRE(obj.has_value());
    const auto obj_id = static_cast<memory::heap::ObjectId>(nanbox::ops::payload(*obj));
    const auto guardian_id = static_cast<memory::heap::ObjectId>(nanbox::ops::payload(*guardian));

    int finalizer_calls = 0;
    auto proc = make_primitive(
        heap,
        [&finalizer_calls, expected_obj = *obj](const std::vector<LispVal>& args) -> std::expected<LispVal, RuntimeError> {
            if (args.size() != 1u || args[0] != expected_obj) {
                return std::unexpected(RuntimeError{VMError{
                    RuntimeErrorCode::TypeError,
                    "finalizer: unexpected argument"}});
            }
            ++finalizer_calls;
            return Nil;
        },
        1,
        false
    );
    BOOST_REQUIRE(proc.has_value());
    BOOST_REQUIRE(heap.register_finalizer(obj_id, *proc).has_value());
    BOOST_REQUIRE(heap.guardian_track(guardian_id, obj_id).has_value());

    vm.collect_garbage();
    BOOST_TEST(heap.pending_finalizer_count() == 1u);

    auto queued_obj = heap.dequeue_guardian_ready(guardian_id);
    BOOST_REQUIRE(queued_obj.has_value());
    BOOST_TEST(*queued_obj == *obj);
    BOOST_TEST(!heap.dequeue_guardian_ready(guardian_id).has_value());

    vm.drain_finalizers_for_test();
    BOOST_TEST(finalizer_calls == 1);
    BOOST_TEST(heap.pending_finalizer_count() == 0u);

    memory::heap::HeapEntry entry{};
    BOOST_TEST(heap.try_get(obj_id, entry));

    vm.collect_garbage();
    BOOST_TEST(!heap.try_get(obj_id, entry));
}

BOOST_FIXTURE_TEST_CASE(cyclic_finalizable_objects_are_reclaimed_after_finalizer_runs, FinalizerVMFixture) {
    VM vm(heap, intern_table);

    auto a = make_vector(heap, {});
    auto b = make_vector(heap, {});
    BOOST_REQUIRE(a.has_value());
    BOOST_REQUIRE(b.has_value());

    const auto a_id = static_cast<memory::heap::ObjectId>(nanbox::ops::payload(*a));
    const auto b_id = static_cast<memory::heap::ObjectId>(nanbox::ops::payload(*b));

    auto* a_vec = heap.try_get_as<memory::heap::ObjectKind::Vector, eta::runtime::types::Vector>(a_id);
    auto* b_vec = heap.try_get_as<memory::heap::ObjectKind::Vector, eta::runtime::types::Vector>(b_id);
    BOOST_REQUIRE(a_vec != nullptr);
    BOOST_REQUIRE(b_vec != nullptr);
    a_vec->elements.push_back(*b);
    b_vec->elements.push_back(*a);

    int finalizer_calls = 0;
    auto proc = make_primitive(
        heap,
        [&finalizer_calls](const std::vector<LispVal>&) -> std::expected<LispVal, RuntimeError> {
            ++finalizer_calls;
            return Nil;
        },
        1,
        false
    );
    BOOST_REQUIRE(proc.has_value());
    BOOST_REQUIRE(heap.register_finalizer(a_id, *proc).has_value());

    vm.collect_garbage();
    BOOST_TEST(heap.pending_finalizer_count() == 1u);

    memory::heap::HeapEntry entry{};
    BOOST_TEST(heap.try_get(a_id, entry));
    BOOST_TEST(heap.try_get(b_id, entry));

    vm.drain_finalizers_for_test();
    BOOST_TEST(finalizer_calls == 1);
    BOOST_TEST(heap.pending_finalizer_count() == 0u);

    vm.collect_garbage();
    BOOST_TEST(!heap.try_get(a_id, entry));
    BOOST_TEST(!heap.try_get(b_id, entry));
}

BOOST_AUTO_TEST_SUITE_END() ///< finalizer_vm_tests

BOOST_AUTO_TEST_SUITE(guardian_primitive_tests)

BOOST_FIXTURE_TEST_CASE(guardian_collect_returns_false_when_queue_empty, VMTestFixture) {
    LispVal res = run(
        "(module m"
        "  (define g (make-guardian))"
        "  (define result (guardian-collect g)))");
    BOOST_CHECK_EQUAL(res, nanbox::False);
}

BOOST_FIXTURE_TEST_CASE(finalizer_primitive_register_and_unregister_smoke, VMTestFixture) {
    LispVal res = run(
        "(module m"
        "  (define obj (vector))"
        "  (define fin (lambda (x) #t))"
        "  (define r1 (register-finalizer! obj fin))"
        "  (define r2 (unregister-finalizer! obj))"
        "  (define r3 (unregister-finalizer! obj))"
        "  (define result (and r1 r2 (not r3))))");
    BOOST_CHECK_EQUAL(res, nanbox::True);
}

BOOST_FIXTURE_TEST_CASE(register_finalizer_validates_types, VMTestFixture) {
    auto bad_obj_err = run_expect_error(
        "(module m"
        "  (define result (register-finalizer! 42 (lambda (x) #t))))");
    auto* bad_obj_vm = std::get_if<VMError>(&bad_obj_err);
    BOOST_REQUIRE(bad_obj_vm);
    BOOST_CHECK(bad_obj_vm->code == RuntimeErrorCode::TypeError);

    auto bad_proc_err = run_expect_error(
        "(module m"
        "  (define obj (vector))"
        "  (define result (register-finalizer! obj 42)))");
    auto* bad_proc_vm = std::get_if<VMError>(&bad_proc_err);
    BOOST_REQUIRE(bad_proc_vm);
    BOOST_CHECK(bad_proc_vm->code == RuntimeErrorCode::TypeError);

    auto cons_err = run_expect_error(
        "(module m"
        "  (define obj (cons 1 2))"
        "  (define result (register-finalizer! obj (lambda (x) #t))))");
    auto* cons_vm = std::get_if<VMError>(&cons_err);
    BOOST_REQUIRE(cons_vm);
    BOOST_CHECK(cons_vm->code == RuntimeErrorCode::TypeError);
}

BOOST_FIXTURE_TEST_CASE(guardian_primitives_validate_types, VMTestFixture) {
    auto track_bad_guardian = run_expect_error(
        "(module m"
        "  (define obj (vector))"
        "  (define result (guardian-track! 42 obj)))");
    auto* track_bad_guardian_vm = std::get_if<VMError>(&track_bad_guardian);
    BOOST_REQUIRE(track_bad_guardian_vm);
    BOOST_CHECK(track_bad_guardian_vm->code == RuntimeErrorCode::TypeError);

    auto track_bad_object = run_expect_error(
        "(module m"
        "  (define g (make-guardian))"
        "  (define result (guardian-track! g 42)))");
    auto* track_bad_object_vm = std::get_if<VMError>(&track_bad_object);
    BOOST_REQUIRE(track_bad_object_vm);
    BOOST_CHECK(track_bad_object_vm->code == RuntimeErrorCode::TypeError);

    auto track_cons_object = run_expect_error(
        "(module m"
        "  (define g (make-guardian))"
        "  (define obj (cons 1 2))"
        "  (define result (guardian-track! g obj)))");
    auto* track_cons_vm = std::get_if<VMError>(&track_cons_object);
    BOOST_REQUIRE(track_cons_vm);
    BOOST_CHECK(track_cons_vm->code == RuntimeErrorCode::TypeError);

    auto collect_bad_guardian = run_expect_error(
        "(module m"
        "  (define result (guardian-collect 42)))");
    auto* collect_bad_guardian_vm = std::get_if<VMError>(&collect_bad_guardian);
    BOOST_REQUIRE(collect_bad_guardian_vm);
    BOOST_CHECK(collect_bad_guardian_vm->code == RuntimeErrorCode::TypeError);
}

BOOST_FIXTURE_TEST_CASE(finalizer_and_guardian_primitives_validate_arity, VMTestFixture) {
    auto register_arity_err = run_expect_error(
        "(module m"
        "  (define obj (vector))"
        "  (define result (register-finalizer! obj)))");
    auto* register_arity_vm = std::get_if<VMError>(&register_arity_err);
    BOOST_REQUIRE(register_arity_vm);
    BOOST_CHECK(register_arity_vm->code == RuntimeErrorCode::InvalidArity);

    auto unregister_arity_err = run_expect_error(
        "(module m"
        "  (define obj (vector))"
        "  (define result (unregister-finalizer! obj obj)))");
    auto* unregister_arity_vm = std::get_if<VMError>(&unregister_arity_err);
    BOOST_REQUIRE(unregister_arity_vm);
    BOOST_CHECK(unregister_arity_vm->code == RuntimeErrorCode::InvalidArity);

    auto make_guardian_arity_err = run_expect_error(
        "(module m"
        "  (define result (make-guardian 1)))");
    auto* make_guardian_arity_vm = std::get_if<VMError>(&make_guardian_arity_err);
    BOOST_REQUIRE(make_guardian_arity_vm);
    BOOST_CHECK(make_guardian_arity_vm->code == RuntimeErrorCode::InvalidArity);

    auto track_arity_err = run_expect_error(
        "(module m"
        "  (define g (make-guardian))"
        "  (define result (guardian-track! g)))");
    auto* track_arity_vm = std::get_if<VMError>(&track_arity_err);
    BOOST_REQUIRE(track_arity_vm);
    BOOST_CHECK(track_arity_vm->code == RuntimeErrorCode::InvalidArity);

    auto collect_arity_err = run_expect_error(
        "(module m"
        "  (define g (make-guardian))"
        "  (define result (guardian-collect g 1)))");
    auto* collect_arity_vm = std::get_if<VMError>(&collect_arity_err);
    BOOST_REQUIRE(collect_arity_vm);
    BOOST_CHECK(collect_arity_vm->code == RuntimeErrorCode::InvalidArity);
}

BOOST_AUTO_TEST_SUITE_END() ///< guardian_primitive_tests

/**
 * VM runtime bounds-check tests (bug fix: unchecked stack/upval ops, bug #1)
 * These tests directly construct BytecodeFunctions that bypass the deserializer
 * verifier to exercise the VM's own defense-in-depth guards.
 */

BOOST_AUTO_TEST_SUITE(vm_bounds_check_tests)

/// Helper: build a VM with a resolved function registry.
struct BoundsCheckFixture {
    memory::heap::Heap heap{1024 * 1024};
    memory::intern::InternTable intern_table;
    BytecodeFunctionRegistry registry;
    BuiltinEnvironment builtins;

    BoundsCheckFixture() : heap(1024 * 1024) {
        register_core_primitives(builtins, heap, intern_table);
    }

    std::expected<LispVal, RuntimeError> execute_func(BytecodeFunction& func) {
        auto* fptr = &func;
        VM vm(heap, intern_table);
        vm.set_function_resolver([&](uint32_t) -> const BytecodeFunction* { return nullptr; });
        vm.globals().resize(16, nanbox::Nil);
        return vm.execute(*fptr);
    }
};

BOOST_FIXTURE_TEST_CASE(loadlocal_oob_returns_error, BoundsCheckFixture) {
    /// not crash with undefined behaviour.
    BytecodeFunction func;
    func.name       = "oob_loadlocal";
    func.stack_size = 2;
    func.constants.push_back(nanbox::Nil);
    func.code.push_back({OpCode::LoadLocal, 99u});
    func.code.push_back({OpCode::Return,    0u});

    auto result = execute_func(func);
    BOOST_CHECK(!result.has_value());
}

BOOST_FIXTURE_TEST_CASE(storelocal_oob_returns_error, BoundsCheckFixture) {
    /// Push a value then try to store it at an out-of-range slot.
    BytecodeFunction func;
    func.name       = "oob_storelocal";
    func.stack_size = 2;
    func.constants.push_back(nanbox::Nil);
    func.code.push_back({OpCode::LoadConst,  0u});  ///< push Nil
    func.code.push_back({OpCode::StoreLocal, 99u});
    func.code.push_back({OpCode::LoadConst,  0u});
    func.code.push_back({OpCode::Return,     0u});

    auto result = execute_func(func);
    BOOST_CHECK(!result.has_value());
}

BOOST_AUTO_TEST_SUITE_END() ///< vm_bounds_check_tests

BOOST_AUTO_TEST_SUITE_END() ///< vm_tests
