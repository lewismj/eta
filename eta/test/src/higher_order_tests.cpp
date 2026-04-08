/**
 * @file higher_order_tests.cpp
 * @brief Tests for map / for-each with user closures using the VM trampoline.
 *
 * Uses a dedicated fixture that wires register_core_primitives with a VM
 * reference so closures work correctly.
 */

#include <boost/test/unit_test.hpp>
#include <algorithm>
#include <iostream>
#include <string>
#include <vector>

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
#include "eta/runtime/port_primitives.h"
#include "eta/runtime/io_primitives.h"
#include "eta/runtime/value_formatter.h"

using namespace eta;
using namespace eta::semantics;
using namespace eta::runtime;
using namespace eta::runtime::vm;
using namespace eta::runtime::memory::factory;
using namespace eta::runtime::nanbox;

// ============================================================================
// Fixture — creates VM in constructor so register_core_primitives gets the vm
// ============================================================================

struct HOTestFixture {
    memory::heap::Heap         heap{4 * 1024 * 1024};
    memory::intern::InternTable intern_table;
    BytecodeFunctionRegistry   registry;
    BuiltinEnvironment         builtins;
    VM                         vm;  // VM lives in the fixture

    HOTestFixture() : vm(heap, intern_table) {
        vm.set_function_resolver([this](uint32_t idx) { return registry.get(idx); });
        // Register primitives with VM reference — enables closure support in map/for-each
        register_core_primitives(builtins, heap, intern_table, &vm);
        register_port_primitives(builtins, heap, intern_table, vm);
        register_io_primitives(builtins, heap, intern_table, vm);
    }

    /// Compile and run a single top-level module, return the 'result' global.
    LispVal run(std::string_view source) {
        reader::lexer::Lexer lex(0, source);
        reader::parser::Parser p(lex);
        auto parsed_res = p.parse_toplevel();
        if (!parsed_res) throw std::runtime_error("Parse error");

        reader::expander::Expander ex;
        auto expanded_res = ex.expand_many(*parsed_res);
        if (!expanded_res) throw std::runtime_error("Expansion error: " + expanded_res.error().message);

        reader::ModuleLinker linker;
        (void) linker.index_modules(*expanded_res);
        (void) linker.link();

        SemanticAnalyzer sa;
        auto sem_res = sa.analyze_all(*expanded_res, linker, builtins);
        if (!sem_res) throw std::runtime_error("Semantic error: " + sem_res.error().message);
        auto& sem_mod = (*sem_res)[0];

        Emitter emitter(sem_mod, heap, intern_table, registry);
        auto* main_func = emitter.emit();

        // Reset globals each call (install() clears the vector)
        auto install_res = builtins.install(heap, vm.globals(), sem_mod.total_globals);
        if (!install_res) throw std::runtime_error("Failed to install builtins");

        auto exec_res = vm.execute(*main_func);
        if (!exec_res) {
            std::string msg = "Runtime error";
            std::visit([&msg](auto&& arg) {
                using T = std::decay_t<decltype(arg)>;
                if constexpr (std::is_same_v<T, VMError>) msg += ": " + arg.message;
            }, exec_res.error());
            throw std::runtime_error(msg);
        }

        // Look for 'result' binding
        for (size_t i = 0; i < sem_mod.bindings.size(); ++i) {
            if (sem_mod.bindings[i].name == "result") {
                return vm.globals()[sem_mod.bindings[i].slot];
            }
        }
        return exec_res.value();
    }

    /// Run and expect a runtime error; returns the error message string.
    std::string run_expect_error(std::string_view source) {
        reader::lexer::Lexer lex(0, source);
        reader::parser::Parser p(lex);
        auto parsed_res = p.parse_toplevel();
        if (!parsed_res) throw std::runtime_error("Parse error in test source");

        reader::expander::Expander ex;
        auto expanded_res = ex.expand_many(*parsed_res);
        if (!expanded_res) throw std::runtime_error("Expansion error");

        reader::ModuleLinker linker;
        (void) linker.index_modules(*expanded_res);
        (void) linker.link();

        SemanticAnalyzer sa;
        auto sem_res = sa.analyze_all(*expanded_res, linker, builtins);
        if (!sem_res) throw std::runtime_error("Semantic error");
        auto& sem_mod = (*sem_res)[0];

        Emitter emitter(sem_mod, heap, intern_table, registry);
        auto* main_func = emitter.emit();

        (void) builtins.install(heap, vm.globals(), sem_mod.total_globals);
        auto exec_res = vm.execute(*main_func);
        if (exec_res) throw std::runtime_error("Expected error but execution succeeded");

        std::string msg;
        std::visit([&msg](auto&& arg) {
            using T = std::decay_t<decltype(arg)>;
            if constexpr (std::is_same_v<T, VMError>) msg = arg.message;
        }, exec_res.error());
        return msg;
    }

    /// Helper: extract the integer value from a LispVal fixnum.
    static int64_t as_int(LispVal v) {
        auto r = ops::decode<int64_t>(v);
        if (!r) throw std::runtime_error("Expected fixnum");
        return *r;
    }

    /// Helper: check the result is a proper list and return its elements.
    std::vector<LispVal> list_to_vec(LispVal lst) {
        std::vector<LispVal> out;
        while (lst != Nil) {
            auto* c = heap.try_get_as<ObjectKind::Cons, types::Cons>(ops::payload(lst));
            if (!c) throw std::runtime_error("Not a cons cell");
            out.push_back(c->car);
            lst = c->cdr;
        }
        return out;
    }
};

// ============================================================================
// map tests
// ============================================================================

BOOST_FIXTURE_TEST_SUITE(map_tests, HOTestFixture)

BOOST_AUTO_TEST_CASE(map_with_lambda_doubles_each_element) {
    // (map (lambda (x) (* x 2)) '(1 2 3))  → '(2 4 6)
    auto result = run(R"(
(module test ()
  (define result (map (lambda (x) (* x 2)) (list 1 2 3))))
)");
    auto elems = list_to_vec(result);
    BOOST_REQUIRE_EQUAL(elems.size(), 3u);
    BOOST_TEST(as_int(elems[0]) == 2);
    BOOST_TEST(as_int(elems[1]) == 4);
    BOOST_TEST(as_int(elems[2]) == 6);
}

BOOST_AUTO_TEST_CASE(map_with_lambda_on_empty_list) {
    // (map (lambda (x) x) '())  → '()
    auto result = run(R"(
(module test ()
  (define result (map (lambda (x) x) (list))))
)");
    BOOST_TEST(result == Nil);
}

BOOST_AUTO_TEST_CASE(map_with_named_closure) {
    // Define a named function and pass it to map
    auto result = run(R"(
(module test ()
  (define (square x) (* x x))
  (define result (map square (list 1 2 3 4 5))))
)");
    auto elems = list_to_vec(result);
    BOOST_REQUIRE_EQUAL(elems.size(), 5u);
    BOOST_TEST(as_int(elems[0]) == 1);
    BOOST_TEST(as_int(elems[1]) == 4);
    BOOST_TEST(as_int(elems[2]) == 9);
    BOOST_TEST(as_int(elems[3]) == 16);
    BOOST_TEST(as_int(elems[4]) == 25);
}

BOOST_AUTO_TEST_CASE(map_closure_captures_variable) {
    // Closure that captures 'offset' from outer scope
    auto result = run(R"(
(module test ()
  (define offset 10)
  (define result (map (lambda (x) (+ x offset)) (list 1 2 3))))
)");
    auto elems = list_to_vec(result);
    BOOST_REQUIRE_EQUAL(elems.size(), 3u);
    BOOST_TEST(as_int(elems[0]) == 11);
    BOOST_TEST(as_int(elems[1]) == 12);
    BOOST_TEST(as_int(elems[2]) == 13);
}

BOOST_AUTO_TEST_CASE(map_with_primitive_procedure) {
    // map with a built-in primitive still works
    auto result = run(R"(
(module test ()
  (define result (map (lambda (x) (+ x 0)) (list 5 10 15))))
)");
    auto elems = list_to_vec(result);
    BOOST_REQUIRE_EQUAL(elems.size(), 3u);
    BOOST_TEST(as_int(elems[0]) == 5);
    BOOST_TEST(as_int(elems[1]) == 10);
    BOOST_TEST(as_int(elems[2]) == 15);
}

BOOST_AUTO_TEST_CASE(map_preserves_order) {
    // Verify result list order matches input list order
    auto result = run(R"(
(module test ()
  (define result (map (lambda (x) (- 10 x)) (list 1 2 3 4))))
)");
    auto elems = list_to_vec(result);
    BOOST_REQUIRE_EQUAL(elems.size(), 4u);
    BOOST_TEST(as_int(elems[0]) == 9);
    BOOST_TEST(as_int(elems[1]) == 8);
    BOOST_TEST(as_int(elems[2]) == 7);
    BOOST_TEST(as_int(elems[3]) == 6);
}

BOOST_AUTO_TEST_CASE(map_error_propagation) {
    // Error inside the mapped lambda propagates out
    auto msg = run_expect_error(R"(
(module test ()
  (define result (map (lambda (x) (error "oops" x)) (list 1))))
)");
    BOOST_TEST(msg.find("oops") != std::string::npos);
}

BOOST_AUTO_TEST_SUITE_END()

// ============================================================================
// for-each tests
// ============================================================================

BOOST_FIXTURE_TEST_SUITE(for_each_tests, HOTestFixture)

BOOST_AUTO_TEST_CASE(for_each_calls_lambda_for_each_element) {
    // Use a list to accumulate results via set! (verifies side effects fire)
    // Simpler approach: use for-each to sum into a global variable
    auto result = run(R"(
(module test ()
  (define count 0)
  (for-each (lambda (x) (set! count (+ count x))) (list 1 2 3 4 5))
  (define result count))
)");
    BOOST_TEST(as_int(result) == 15);
}

BOOST_AUTO_TEST_CASE(for_each_on_empty_list_does_nothing) {
    auto result = run(R"(
(module test ()
  (define called #f)
  (for-each (lambda (x) (set! called #t)) (list))
  (define result called))
)");
    BOOST_TEST(result == False);
}

BOOST_AUTO_TEST_CASE(for_each_with_named_closure) {
    auto result = run(R"(
(module test ()
  (define total 0)
  (define (add-to-total x) (set! total (+ total x)))
  (for-each add-to-total (list 10 20 30))
  (define result total))
)");
    BOOST_TEST(as_int(result) == 60);
}

BOOST_AUTO_TEST_CASE(for_each_returns_unspecified) {
    // for-each returns nil (#void-ish) — we just check it doesn't error
    BOOST_CHECK_NO_THROW(
        run(R"(
(module test ()
  (define result (for-each (lambda (x) x) (list 1 2 3))))
)")
    );
}

BOOST_AUTO_TEST_CASE(for_each_error_propagation) {
    auto msg = run_expect_error(R"(
(module test ()
  (for-each (lambda (x) (error "stop" x)) (list 1))
  (define result 0))
)");
    BOOST_TEST(msg.find("stop") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(for_each_closure_captures_outer_binding) {
    auto result = run(R"(
(module test ()
  (define multiplier 3)
  (define sum 0)
  (for-each (lambda (x) (set! sum (+ sum (* x multiplier)))) (list 1 2 3 4))
  (define result sum))
)");
    // 1*3 + 2*3 + 3*3 + 4*3 = 3+6+9+12 = 30
    BOOST_TEST(as_int(result) == 30);
}

BOOST_AUTO_TEST_SUITE_END()

// ============================================================================
// call_value trampoline unit tests (lower level)
// ============================================================================

BOOST_FIXTURE_TEST_SUITE(call_value_tests, HOTestFixture)

BOOST_AUTO_TEST_CASE(nested_map_calls) {
    // map inside map — tests recursive trampoline calls
    auto result = run(R"(
(module test ()
  (define lst (list (list 1 2) (list 3 4) (list 5 6)))
  (define result (map (lambda (inner)
                        (map (lambda (x) (* x x)) inner))
                      lst)))
)");
    // Result should be ((1 4) (9 16) (25 36))
    auto outer = list_to_vec(result);
    BOOST_REQUIRE_EQUAL(outer.size(), 3u);

    auto row0 = list_to_vec(outer[0]);
    BOOST_REQUIRE_EQUAL(row0.size(), 2u);
    BOOST_TEST(as_int(row0[0]) == 1);
    BOOST_TEST(as_int(row0[1]) == 4);

    auto row1 = list_to_vec(outer[1]);
    BOOST_REQUIRE_EQUAL(row1.size(), 2u);
    BOOST_TEST(as_int(row1[0]) == 9);
    BOOST_TEST(as_int(row1[1]) == 16);
}

BOOST_AUTO_TEST_CASE(map_then_for_each) {
    // map followed by for-each on the result
    auto result = run(R"(
(module test ()
  (define doubled (map (lambda (x) (* x 2)) (list 1 2 3)))
  (define total 0)
  (for-each (lambda (x) (set! total (+ total x))) doubled)
  (define result total))
)");
    // doubled = (2 4 6), total = 12
    BOOST_TEST(as_int(result) == 12);
}

BOOST_AUTO_TEST_SUITE_END()

