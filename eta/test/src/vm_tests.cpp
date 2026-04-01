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
        auto& sem_mod = sem_mods_vec[0];

        // For tests, skip StoreGlobal for our injected primitives in the module initializer
        auto it = std::remove_if(sem_mod.toplevel_inits.begin(), sem_mod.toplevel_inits.end(), [&](const core::Node* node) {
            if (const auto* s = std::get_if<core::Set>(&node->data)) {
                if (const auto* g = std::get_if<core::Address::Global>(&s->target.where)) {
                    const auto& b = sem_mod.bindings[g->id];
                    return b.name == "+" || b.name == "-" || b.name == "eq?";
                }
            }
            return false;
        });
        sem_mod.toplevel_inits.erase(it, sem_mod.toplevel_inits.end());

        // Remove bindings for injected primitives (+, -, eq?) so VM uses the injected versions
        auto is_injected_prim = [](const auto& b) {
            return b.name == "+" || b.name == "-" || b.name == "eq?";
        };
        // Record old-to-new slot mapping
        std::vector<int> slot_map(sem_mod.bindings.size(), -1);
        std::vector<decltype(sem_mod.bindings)::value_type> new_bindings;
        int new_slot = 0;
        for (size_t i = 0; i < sem_mod.bindings.size(); ++i) {
            slot_map[i] = new_slot;
            auto b = sem_mod.bindings[i];
            b.slot = new_slot;
            new_bindings.push_back(b);
            ++new_slot;
        }
        sem_mod.bindings = std::move(new_bindings);
        // Update all global slot references in toplevel_inits (recursive)
        std::function<void(core::Node*)> patch = [&](core::Node* n) {
            if (!n) return;
            if (auto* var_node = std::get_if<core::Var>(&n->data)) {
                if (auto* g = std::get_if<core::Address::Global>(&var_node->addr.where)) {
                    if (g->id < slot_map.size() && slot_map[g->id] != -1)
                        g->id = slot_map[g->id];
                }
            } else if (auto* set_node = std::get_if<core::Set>(&n->data)) {
                if (auto* g = std::get_if<core::Address::Global>(&set_node->target.where)) {
                    if (g->id < slot_map.size() && slot_map[g->id] != -1)
                        g->id = slot_map[g->id];
                }
                patch(set_node->value);
            } else if (auto* call_node = std::get_if<core::Call>(&n->data)) {
                patch(call_node->callee);
                for (auto* a : call_node->args) patch(a);
            } else if (auto* if_node = std::get_if<core::If>(&n->data)) {
                patch(if_node->test); patch(if_node->conseq); patch(if_node->alt);
            } else if (auto* begin_node = std::get_if<core::Begin>(&n->data)) {
                for (auto* e : begin_node->exprs) patch(e);
            } else if (auto* lambda_node = std::get_if<core::Lambda>(&n->data)) {
                patch(lambda_node->body);
                for (auto& as : lambda_node->upval_sources) {
                    if (auto* g = std::get_if<core::Address::Global>(&as.where)) {
                        if (g->id < slot_map.size() && slot_map[g->id] != -1)
                            g->id = slot_map[g->id];
                    }
                }
            } else if (auto* values_node = std::get_if<core::Values>(&n->data)) {
                for (auto* e : values_node->exprs) patch(e);
            } else if (auto* cwv = std::get_if<core::CallWithValues>(&n->data)) {
                patch(cwv->producer); patch(cwv->consumer);
            } else if (auto* ccc = std::get_if<core::CallCC>(&n->data)) {
                patch(ccc->consumer);
            } else if (auto* dw = std::get_if<core::DynamicWind>(&n->data)) {
                patch(dw->before); patch(dw->body); patch(dw->after);
            }
        };
        for (auto* node : sem_mod.toplevel_inits) {
            patch(node);
        }

        Emitter emitter(sem_mod, heap, intern_table, registry);
        auto* main_func = emitter.emit();

        VM vm(heap, intern_table);
        vm.set_function_resolver([this](uint32_t idx) { return registry.get(idx); });

        // Populate basic primitives if they are used
        auto prim_add = make_primitive(heap, [](const std::vector<LispVal>& args) -> std::expected<LispVal, RuntimeError> {
            int64_t res = 0;
            for (auto v : args) {
                auto val = nanbox::ops::decode<int64_t>(v);
                if (!val) {
                    std::cout << "DEBUG: + Arg not fixnum: " << std::hex << v << std::dec << " tag=" << static_cast<int>(nanbox::ops::tag(v)) << std::endl;
                    return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError, "Arg not fixnum"}});
                }
                res += val.value();
            }
            return nanbox::ops::encode(res).value();
        }, 0, true).value();

        auto prim_sub = make_primitive(heap, [](const std::vector<LispVal>& args) -> std::expected<LispVal, RuntimeError> {
            if (args.empty()) return nanbox::ops::encode(0LL).value();
            auto v0 = nanbox::ops::decode<int64_t>(args[0]);
            if (!v0) {
                 std::cout << "DEBUG: - Arg0 not fixnum: " << std::hex << args[0] << std::dec << std::endl;
                 return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError, "Arg not fixnum"}});
            }
            int64_t res = v0.value();
            for (size_t i = 1; i < args.size(); ++i) {
                auto val = nanbox::ops::decode<int64_t>(args[i]);
                if (!val) {
                    std::cout << "DEBUG: - Arg" << i << " not fixnum: " << std::hex << args[i] << std::dec << std::endl;
                    return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError, "Arg not fixnum"}});
                }
                res -= val.value();
            }
            return nanbox::ops::encode(res).value();
        }, 1, true).value();

        auto prim_eq = make_primitive(heap, [](const std::vector<LispVal>& args) -> std::expected<LispVal, RuntimeError> {
            if (args.size() != 2) return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::InvalidArity, "eq? expects 2 args"}});
            //std::cout << "DEBUG: eq? " << std::hex << args[0] << " " << args[1] << std::dec << " result=" << (args[0] == args[1]) << std::endl;
            return (args[0] == args[1]) ? nanbox::True : nanbox::False;
        }, 2).value();

        // Register primitives in VM globals
        vm.globals().assign(sem_mod.bindings.size(), nanbox::Nil);
        for (size_t i = 0; i < sem_mod.bindings.size(); ++i) {
            const auto& b = sem_mod.bindings[i];
            if (b.name == "+") vm.globals()[b.slot] = prim_add;
            else if (b.name == "-") vm.globals()[b.slot] = prim_sub;
            else if (b.name == "eq?") vm.globals()[b.slot] = prim_eq;
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

        //std::cout << "DEBUG: exec_res.value() = " << std::hex << exec_res.value() << std::dec << std::endl;
        //std::cout << "DEBUG: tag = " << static_cast<int>(nanbox::ops::tag(exec_res.value())) << std::endl;
        
        // Find 'result' global
        for (size_t i = 0; i < sem_mod.bindings.size(); ++i) {
            //std::cout << "DEBUG: binding[" << i << "].name = " << sem_mod.bindings[i].name << ", slot = " << sem_mod.bindings[i].slot << ", value = " << std::hex << vm.globals()[sem_mod.bindings[i].slot] << std::dec << std::endl;
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


BOOST_AUTO_TEST_CASE(test_tail_call_recursion) {
    std::string src = 
        "(module m "
        "  (define (eq? a b) #f) (define (- a b) #f) "
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
        "  (define (+ a b) #f) (define (eq? a b) #f) "
        "  (define result 0) "
        "  (define (before) (set! result (+ result 1))) "
        "  (define (after) (set! result (+ result 10))) "
        "  (define cont #f) "
        "  (call/cc (lambda (k) "
        "    (dynamic-wind before "
        "      (lambda () (set! cont k)) "
        "      after))) "
        "  (if (eq? result 11) (cont #f) #f) "
        "  result)";

    LispVal res = run(src);
    BOOST_CHECK_EQUAL(nanbox::ops::decode<int64_t>(res).value(), 22);
}


BOOST_AUTO_TEST_SUITE_END()
