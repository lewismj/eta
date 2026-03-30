#include <boost/test/unit_test.hpp>
#include "eta/reader/lexer.h"
#include "eta/reader/parser.h"
#include "eta/reader/expander.h"
#include "eta/reader/module_linker.h"
#include "eta/semantics/semantic_analyzer.h"
#include "eta/semantics/emitter.h"
#include "eta/runtime/vm/bytecode.h"
#include "eta/runtime/memory/heap.h"
#include "eta/runtime/memory/intern_table.h"

using namespace eta;
using namespace eta::semantics;
using namespace eta::runtime;
using namespace eta::runtime::vm;

BOOST_AUTO_TEST_SUITE(emitter_tests)

BOOST_AUTO_TEST_CASE(test_tail_call_recursion_bytecode) {
    std::string src =
        "(module m "
        "  (define (eq? a b) #f) (define (- a b) #f) "
        "  (define (loop n) "
        "    (if (eq? n 0) "
        "        42 "
        "        (loop (- n 1)))) "
        "  (define result (loop 2000)))";

    // Parse, expand, link, analyze
    reader::lexer::Lexer lex(0, src);
    reader::parser::Parser p(lex);
    auto parsed_res = p.parse_toplevel();
    BOOST_REQUIRE(parsed_res);
    auto parsed = std::move(*parsed_res);

    reader::expander::Expander ex;
    auto expanded_res = ex.expand_many(parsed);
    BOOST_REQUIRE(expanded_res);
    auto expanded = std::move(*expanded_res);

    reader::ModuleLinker linker;
    linker.index_modules(expanded);
    linker.link();

    SemanticAnalyzer sa;
    auto sem_res = sa.analyze_all(expanded, linker);
    BOOST_REQUIRE(sem_res);
    const auto& sem_mod = (*sem_res)[0];

    // Emit bytecode
    memory::heap::Heap heap(1024 * 1024);
    memory::intern::InternTable intern_table;
    BytecodeFunctionRegistry registry;
    Emitter emitter(sem_mod, heap, intern_table, registry);
    auto* main_func = emitter.emit();
    BOOST_REQUIRE(main_func);

    // Check that the bytecode contains a TailCall opcode for the recursive call
    bool found_tailcall = false;
    for (const auto& func : registry.all()) {
        for (const auto& instr : func.code) {
            if (instr.opcode == OpCode::TailCall) {
                found_tailcall = true;
                break;
            }
        }
        if (found_tailcall) break;
    }
    BOOST_CHECK(found_tailcall);
}

BOOST_AUTO_TEST_SUITE_END()

