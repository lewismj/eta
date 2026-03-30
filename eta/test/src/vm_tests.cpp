#include <boost/test/unit_test.hpp>
#include <iostream>
#include <algorithm>
#include "eta/reader/lexer.h"
#include "eta/reader/parser.h"
#include "eta/reader/expander.h"
#include "eta/reader/module_linker.h"
#include "eta/semantics/semantic_analyzer.h"
#include "eta/semantics/emitter.h"
#include "eta/runtime/vm/vm.h"
#include "eta/runtime/factory.h"

using namespace eta;
using namespace eta::semantics;
using namespace eta::runtime;
using namespace eta::runtime::vm;
using namespace eta::runtime::memory::factory;

struct VMTestFixture {
    memory::heap::Heap heap;
    memory::intern::InternTable intern_table;
    BytecodeFunctionRegistry registry;

    VMTestFixture() : heap(1024 * 1024), intern_table(), registry() {}

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
        auto sem_res = sa.analyze_all(expanded, linker);
        if (!sem_res) throw std::runtime_error("Semantic error: " + sem_res.error().message);
        auto sem_mods_vec = std::move(*sem_res);

        BOOST_REQUIRE(!sem_mods_vec.empty());
        const auto& sem_mod = sem_mods_vec[0];

        Emitter emitter(sem_mod, heap, intern_table, registry);
        auto* main_func = emitter.emit();

        VM vm(heap, intern_table);
        vm.set_function_resolver([this](uint32_t idx) { return registry.get(idx); });

        // Populate basic primitives if they are used
        auto prim_add = make_primitive(heap, [](const std::vector<LispVal>& args) -> std::expected<LispVal, RuntimeError> {
            int64_t res = 0;
            for (auto v : args) {
                auto val = nanbox::ops::decode<int64_t>(v);
                if (!val) return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError, "Arg not fixnum"}});
                res += val.value();
            }
            return nanbox::ops::encode(res).value();
        }, 0, true).value();

        auto prim_sub = make_primitive(heap, [](const std::vector<LispVal>& args) -> std::expected<LispVal, RuntimeError> {
            if (args.empty()) return nanbox::ops::encode(0LL).value();
            auto v0 = nanbox::ops::decode<int64_t>(args[0]);
            if (!v0) return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError, "Arg not fixnum"}});
            int64_t res = v0.value();
            for (size_t i = 1; i < args.size(); ++i) {
                auto val = nanbox::ops::decode<int64_t>(args[i]);
                if (!val) return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError, "Arg not fixnum"}});
                res -= val.value();
            }
            return nanbox::ops::encode(res).value();
        }, 1, true).value();

        auto prim_eq = make_primitive(heap, [](const std::vector<LispVal>& args) -> std::expected<LispVal, RuntimeError> {
            if (args.size() != 2) return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::InvalidArity, "eq? expects 2 args"}});
            return (args[0] == args[1]) ? nanbox::True : nanbox::False;
        }, 2).value();

        // Register primitives in VM globals
        vm.globals().assign(sem_mod.bindings.size(), nanbox::Nil);
        for (size_t i = 0; i < sem_mod.bindings.size(); ++i) {
            const auto& b = sem_mod.bindings[i];
            if (b.kind == BindingInfo::Kind::Global || b.kind == BindingInfo::Kind::Import) {
                if (b.name == "+") vm.globals()[b.slot] = prim_add;
                else if (b.name == "-") vm.globals()[b.slot] = prim_sub;
                else if (b.name == "eq?") vm.globals()[b.slot] = prim_eq;
            }
        }

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

BOOST_AUTO_TEST_CASE(test_call_cc_basic) {
    std::string src = "(module m (define result (call/cc (lambda (k) (k 42) 99))))";
    LispVal res = run(src);
    BOOST_CHECK_EQUAL(nanbox::ops::decode<int64_t>(res).value(), 42);
}

BOOST_AUTO_TEST_CASE(test_call_cc_multiple_invocations) {
    std::string src = 
        "(module m (define result 0) "
        "  (define cont #f) "
        "  (define (test) "
        "    (set! result (+ result 1)) "
        "    (call/cc (lambda (k) (set! cont k)))) "
        "  (test) " // result = 1
        "  (if (eq? result 1) (cont #f) #f))"; // call cont, result becomes 2
    LispVal res = run(src);
    BOOST_CHECK_EQUAL(nanbox::ops::decode<int64_t>(res).value(), 2);
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

BOOST_AUTO_TEST_CASE(test_dynamic_wind_with_call_cc) {
    std::string src = 
        "(module m (define result 0) "
        "  (define (before) (set! result (+ result 1))) "
        "  (define (after) (set! result (+ result 10))) "
        "  (define cont #f) "
        "  (call/cc (lambda (k) "
        "    (dynamic-wind before "
        "      (lambda () (set! cont k)) "
        "      after))) "
        "  (if (eq? result 11) (cont #f) #f))";
    LispVal res = run(src);
    BOOST_CHECK_EQUAL(nanbox::ops::decode<int64_t>(res).value(), 22);
}

BOOST_AUTO_TEST_SUITE_END()
