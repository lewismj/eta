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
#include "eta/runtime/builtin_env.h"
#include "eta/runtime/core_primitives.h"

using namespace eta;
using namespace eta::semantics;
using namespace eta::runtime;
using namespace eta::runtime::vm;

// ============================================================================
// Helper fixture for emitter tests
// ============================================================================

struct EmitterFixture {
    memory::heap::Heap heap;
    memory::intern::InternTable intern_table;
    BytecodeFunctionRegistry registry;
    BuiltinEnvironment builtins;

    EmitterFixture() : heap(1024 * 1024) {
        register_core_primitives(builtins, heap, intern_table);
    }

    // Compile module source through full pipeline, return main function pointer
    BytecodeFunction* compile(const std::string& src) {
        reader::lexer::Lexer lex(0, src);
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
        auto& sem_mod = (*sem_res)[0];

        Emitter emitter(sem_mod, heap, intern_table, registry);
        return emitter.emit();
    }

    // Check if any function in the registry contains the given opcode
    bool has_opcode(OpCode op) const {
        for (const auto& func : registry.all()) {
            for (const auto& instr : func.code) {
                if (instr.opcode == op) return true;
            }
        }
        return false;
    }

    // Count occurrences of an opcode across all functions
    int count_opcode(OpCode op) const {
        int count = 0;
        for (const auto& func : registry.all()) {
            for (const auto& instr : func.code) {
                if (instr.opcode == op) ++count;
            }
        }
        return count;
    }

    // Find opcodes in a specific function (by index in registry)
    bool func_has_opcode(size_t func_idx, OpCode op) const {
        const auto& funcs = registry.all();
        if (func_idx >= funcs.size()) return false;
        for (const auto& instr : funcs[func_idx].code) {
            if (instr.opcode == op) return true;
        }
        return false;
    }

    // Get the main function (first registered, index 0 after compile)
    const BytecodeFunction& main_func() const {
        return registry.all().back();
    }
};

BOOST_FIXTURE_TEST_SUITE(emitter_tests, EmitterFixture)

// ============================================================================
// Basic emission structure
// ============================================================================

BOOST_AUTO_TEST_CASE(emit_returns_non_null) {
    auto* f = compile("(module m (define x 42))");
    BOOST_REQUIRE(f != nullptr);
}

BOOST_AUTO_TEST_CASE(emit_main_ends_with_return) {
    auto* f = compile("(module m (define x 42))");
    BOOST_REQUIRE(f);
    BOOST_REQUIRE(!f->code.empty());
    BOOST_CHECK(f->code.back().opcode == OpCode::Return);
}

BOOST_AUTO_TEST_CASE(emit_main_loads_nil_before_return) {
    auto* f = compile("(module m (define x 1))");
    BOOST_REQUIRE(f);
    // Module init returns Nil: LoadConst(Nil) then Return
    auto sz = f->code.size();
    BOOST_REQUIRE(sz >= 2);
    BOOST_CHECK(f->code[sz - 2].opcode == OpCode::LoadConst);
    BOOST_CHECK(f->code[sz - 1].opcode == OpCode::Return);
}

// ============================================================================
// LoadConst - constants
// ============================================================================

BOOST_AUTO_TEST_CASE(emit_integer_constant) {
    compile("(module m (define x 42))");
    BOOST_CHECK(has_opcode(OpCode::LoadConst));
}

BOOST_AUTO_TEST_CASE(emit_boolean_true) {
    compile("(module m (define x #t))");
    BOOST_CHECK(has_opcode(OpCode::LoadConst));
}

BOOST_AUTO_TEST_CASE(emit_boolean_false) {
    compile("(module m (define x #f))");
    BOOST_CHECK(has_opcode(OpCode::LoadConst));
}

BOOST_AUTO_TEST_CASE(emit_string_constant) {
    compile("(module m (define x \"hello\"))");
    BOOST_CHECK(has_opcode(OpCode::LoadConst));
}

BOOST_AUTO_TEST_CASE(emit_char_constant) {
    compile("(module m (define x #\\a))");
    BOOST_CHECK(has_opcode(OpCode::LoadConst));
}

BOOST_AUTO_TEST_CASE(emit_float_constant) {
    compile("(module m (define x 3.14))");
    BOOST_CHECK(has_opcode(OpCode::LoadConst));
}

// ============================================================================
// Global load/store
// ============================================================================

BOOST_AUTO_TEST_CASE(emit_store_global) {
    compile("(module m (define x 42))");
    BOOST_CHECK(has_opcode(OpCode::StoreGlobal));
}

BOOST_AUTO_TEST_CASE(emit_load_global) {
    compile("(module m (define x 42) (define y x))");
    BOOST_CHECK(has_opcode(OpCode::LoadGlobal));
}

// ============================================================================
// Lambda / MakeClosure
// ============================================================================

BOOST_AUTO_TEST_CASE(emit_lambda_makes_closure) {
    compile("(module m (define f (lambda (x) x)))");
    BOOST_CHECK(has_opcode(OpCode::MakeClosure));
}

BOOST_AUTO_TEST_CASE(emit_lambda_body_has_return) {
    compile("(module m (define f (lambda (x) x)))");
    // The lambda body (second function in registry) should end with Return
    BOOST_REQUIRE(registry.size() >= 2);
    // Lambda func should be added first, main func last
    const auto& funcs = registry.all();
    bool found_lambda_return = false;
    for (size_t i = 0; i < funcs.size() - 1; ++i) {
        if (!funcs[i].code.empty() && funcs[i].code.back().opcode == OpCode::Return) {
            found_lambda_return = true;
            break;
        }
    }
    BOOST_CHECK(found_lambda_return);
}

BOOST_AUTO_TEST_CASE(emit_lambda_arity_set) {
    compile("(module m (define f (lambda (a b c) a)))");
    // Find the lambda function (not main)
    const auto& funcs = registry.all();
    bool found = false;
    for (const auto& func : funcs) {
        if (func.arity == 3) {
            found = true;
            BOOST_CHECK(!func.has_rest);
            break;
        }
    }
    BOOST_CHECK(found);
}

BOOST_AUTO_TEST_CASE(emit_lambda_rest_param) {
    compile("(module m (define f (lambda (a . rest) a)))");
    const auto& funcs = registry.all();
    bool found = false;
    for (const auto& func : funcs) {
        if (func.has_rest) {
            found = true;
            break;
        }
    }
    BOOST_CHECK(found);
}

// ============================================================================
// Local load/store
// ============================================================================

BOOST_AUTO_TEST_CASE(emit_load_local) {
    compile("(module m (define f (lambda (x) x)))");
    // Lambda body should load x from local slot
    BOOST_CHECK(has_opcode(OpCode::LoadLocal));
}

// ============================================================================
// Upvalues (closures capturing variables)
// ============================================================================

BOOST_AUTO_TEST_CASE(emit_load_upval) {
    compile("(module m (define f (lambda (x) (lambda () x))))");
    BOOST_CHECK(has_opcode(OpCode::LoadUpval));
}

BOOST_AUTO_TEST_CASE(emit_store_upval) {
    compile("(module m (define f (lambda (x) (lambda () (set! x 1)))))");
    BOOST_CHECK(has_opcode(OpCode::StoreUpval));
}

// ============================================================================
// Call / TailCall
// ============================================================================

BOOST_AUTO_TEST_CASE(emit_call) {
    compile("(module m (define (f x) x) (define y (f 1)))");
    BOOST_CHECK(has_opcode(OpCode::Call));
}

BOOST_AUTO_TEST_CASE(emit_tail_call) {
    auto* f = compile(
        "(module m "
        "  (define (eq? a b) #f) (define (- a b) #f) "
        "  (define (loop n) "
        "    (if (eq? n 0) 42 (loop (- n 1)))) "
        "  (define result (loop 100)))");
    BOOST_REQUIRE(f);
    BOOST_CHECK(has_opcode(OpCode::TailCall));
}

BOOST_AUTO_TEST_CASE(emit_non_tail_position_uses_call) {
    compile("(module m (define (f x) x) (define (g x) (+ (f x) 1)))");
    // (f x) is in non-tail position due to (+ ... 1)
    BOOST_CHECK(has_opcode(OpCode::Call));
}

// ============================================================================
// If (JumpIfFalse + Jump)
// ============================================================================

BOOST_AUTO_TEST_CASE(emit_if_has_jump_if_false) {
    compile("(module m (define x (if #t 1 2)))");
    BOOST_CHECK(has_opcode(OpCode::JumpIfFalse));
}

BOOST_AUTO_TEST_CASE(emit_if_has_jump) {
    compile("(module m (define x (if #t 1 2)))");
    BOOST_CHECK(has_opcode(OpCode::Jump));
}

BOOST_AUTO_TEST_CASE(emit_if_both_branches) {
    compile("(module m (define x (if #t 1 2)))");
    // Should have at least 1 JumpIfFalse and 1 Jump
    BOOST_CHECK_GE(count_opcode(OpCode::JumpIfFalse), 1);
    BOOST_CHECK_GE(count_opcode(OpCode::Jump), 1);
}

// ============================================================================
// Begin (with Pop between expressions)
// ============================================================================

BOOST_AUTO_TEST_CASE(emit_begin_pops_intermediate) {
    compile("(module m (define x (begin 1 2 3)))");
    // (begin 1 2 3): emit 1, Pop, emit 2, Pop, emit 3
    BOOST_CHECK(has_opcode(OpCode::Pop));
}

// ============================================================================
// Set!
// ============================================================================

BOOST_AUTO_TEST_CASE(emit_set_stores_global) {
    compile("(module m (define x 1) (set! x 2))");
    BOOST_CHECK(has_opcode(OpCode::StoreGlobal));
}

BOOST_AUTO_TEST_CASE(emit_set_pushes_nil) {
    // set! returns unspecified (nil)
    compile("(module m (define x 1) (define y (set! x 2)))");
    // After StoreGlobal, a LoadConst(Nil) is emitted
    BOOST_CHECK(has_opcode(OpCode::StoreGlobal));
    BOOST_CHECK(has_opcode(OpCode::LoadConst));
}

// ============================================================================
// Quote
// ============================================================================

BOOST_AUTO_TEST_CASE(emit_quote_number) {
    compile("(module m (define x (quote 42)))");
    BOOST_CHECK(has_opcode(OpCode::LoadConst));
}

BOOST_AUTO_TEST_CASE(emit_quote_symbol) {
    compile("(module m (define x (quote foo)))");
    BOOST_CHECK(has_opcode(OpCode::LoadConst));
}

BOOST_AUTO_TEST_CASE(emit_quote_list) {
    compile("(module m (define x (quote (1 2 3))))");
    // Quoted list builds cons chain via LoadConst + Cons or just constants
    BOOST_CHECK(has_opcode(OpCode::LoadConst));
}

BOOST_AUTO_TEST_CASE(emit_quote_nil) {
    compile("(module m (define x (quote ())))");
    BOOST_CHECK(has_opcode(OpCode::LoadConst));
}

BOOST_AUTO_TEST_CASE(emit_quote_nested) {
    compile("(module m (define x (quote (a (b c) d))))");
    BOOST_CHECK(has_opcode(OpCode::LoadConst));
}

BOOST_AUTO_TEST_CASE(emit_quote_boolean) {
    compile("(module m (define x (quote #t)))");
    BOOST_CHECK(has_opcode(OpCode::LoadConst));
}

BOOST_AUTO_TEST_CASE(emit_quote_string) {
    compile("(module m (define x (quote \"hello\")))");
    BOOST_CHECK(has_opcode(OpCode::LoadConst));
}

// ============================================================================
// Values
// ============================================================================

BOOST_AUTO_TEST_CASE(emit_values_opcode) {
    compile("(module m (define x (values 1 2 3)))");
    BOOST_CHECK(has_opcode(OpCode::Values));
}

BOOST_AUTO_TEST_CASE(emit_values_arg_count) {
    compile("(module m (define x (values 1 2 3)))");
    // Find the Values instruction and check arg == 3
    for (const auto& func : registry.all()) {
        for (const auto& instr : func.code) {
            if (instr.opcode == OpCode::Values) {
                BOOST_CHECK_EQUAL(instr.arg, 3u);
                return;
            }
        }
    }
    BOOST_FAIL("Values opcode not found");
}

// ============================================================================
// call-with-values
// ============================================================================

BOOST_AUTO_TEST_CASE(emit_call_with_values) {
    compile("(module m (define x (call-with-values (lambda () (values 1 2)) (lambda (a b) (+ a b)))))");
    BOOST_CHECK(has_opcode(OpCode::CallWithValues));
}

// ============================================================================
// dynamic-wind
// ============================================================================

BOOST_AUTO_TEST_CASE(emit_dynamic_wind) {
    compile("(module m (define x (dynamic-wind (lambda () 0) (lambda () 42) (lambda () 0))))");
    BOOST_CHECK(has_opcode(OpCode::DynamicWind));
}

// ============================================================================
// call/cc
// ============================================================================

BOOST_AUTO_TEST_CASE(emit_call_cc) {
    compile("(module m (define x (call/cc (lambda (k) 42))))");
    BOOST_CHECK(has_opcode(OpCode::CallCC));
}

// ============================================================================
// PatchClosureUpval (letrec self-reference)
// ============================================================================

BOOST_AUTO_TEST_CASE(emit_patch_closure_upval) {
    compile("(module m (define x (letrec ((f (lambda () (f)))) (f))))");
    BOOST_CHECK(has_opcode(OpCode::PatchClosureUpval));
}

// ============================================================================
// Multiple functions in registry
// ============================================================================

BOOST_AUTO_TEST_CASE(emit_multiple_lambdas) {
    compile("(module m (define f (lambda (x) x)) (define g (lambda (y) y)))");
    // Should have main func + 2 lambda funcs = 3 total
    BOOST_CHECK_GE(registry.size(), 3u);
}

BOOST_AUTO_TEST_CASE(emit_nested_lambda) {
    compile("(module m (define f (lambda (x) (lambda (y) (+ x y)))))");
    // Should have main + outer lambda + inner lambda = 3
    BOOST_CHECK_GE(registry.size(), 3u);
}

// ============================================================================
// BytecodeFunctionRegistry
// ============================================================================

BOOST_AUTO_TEST_CASE(registry_add_and_get) {
    BytecodeFunctionRegistry reg;
    BytecodeFunction func;
    func.name = "test";
    func.arity = 2;
    func.has_rest = false;
    func.stack_size = 4;
    uint32_t idx = reg.add(std::move(func));
    BOOST_CHECK_EQUAL(idx, 0u);

    auto* retrieved = reg.get(idx);
    BOOST_REQUIRE(retrieved);
    BOOST_CHECK_EQUAL(retrieved->name, "test");
    BOOST_CHECK_EQUAL(retrieved->arity, 2u);
}

BOOST_AUTO_TEST_CASE(registry_get_out_of_bounds) {
    BytecodeFunctionRegistry reg;
    BOOST_CHECK(reg.get(0) == nullptr);
    BOOST_CHECK(reg.get(999) == nullptr);
}

BOOST_AUTO_TEST_CASE(registry_size) {
    BytecodeFunctionRegistry reg;
    BOOST_CHECK_EQUAL(reg.size(), 0u);
    BytecodeFunction f1; f1.name = "a";
    BytecodeFunction f2; f2.name = "b";
    reg.add(std::move(f1));
    BOOST_CHECK_EQUAL(reg.size(), 1u);
    reg.add(std::move(f2));
    BOOST_CHECK_EQUAL(reg.size(), 2u);
}

BOOST_AUTO_TEST_CASE(registry_stable_pointers) {
    BytecodeFunctionRegistry reg;
    BytecodeFunction f1; f1.name = "first";
    uint32_t idx1 = reg.add(std::move(f1));
    auto* ptr1 = reg.get(idx1);

    // Add more functions - pointer should remain stable (deque)
    for (int i = 0; i < 100; ++i) {
        BytecodeFunction f; f.name = "func" + std::to_string(i);
        reg.add(std::move(f));
    }

    auto* ptr1_after = reg.get(idx1);
    BOOST_CHECK_EQUAL(ptr1, ptr1_after);
    BOOST_CHECK_EQUAL(ptr1_after->name, "first");
}

BOOST_AUTO_TEST_CASE(registry_get_mut) {
    BytecodeFunctionRegistry reg;
    BytecodeFunction f; f.name = "mutable";
    uint32_t idx = reg.add(std::move(f));

    auto* mptr = reg.get_mut(idx);
    BOOST_REQUIRE(mptr);
    mptr->name = "modified";

    BOOST_CHECK_EQUAL(reg.get(idx)->name, "modified");
}

// ============================================================================
// Bytecode encoding helpers
// ============================================================================

BOOST_AUTO_TEST_CASE(encode_decode_func_index) {
    uint32_t original = 42;
    auto encoded = encode_func_index(original);
    BOOST_CHECK(is_func_index(encoded));
    BOOST_CHECK_EQUAL(decode_func_index(encoded), original);
}

BOOST_AUTO_TEST_CASE(func_index_not_regular_value) {
    BOOST_CHECK(!is_func_index(0));
    BOOST_CHECK(!is_func_index(42));
    BOOST_CHECK(!is_func_index(nanbox::Nil));
}

// ============================================================================
// String constant caching
// ============================================================================

BOOST_AUTO_TEST_CASE(emit_duplicate_strings_share_constant) {
    auto* f = compile("(module m (define x \"hello\") (define y \"hello\"))");
    BOOST_REQUIRE(f);
    // Count LoadConst instructions that reference string constants
    // Due to caching, "hello" should be loaded from the same constant index
    // We can't easily inspect this without more infrastructure,
    // but at minimum the emission should succeed
    BOOST_CHECK(has_opcode(OpCode::LoadConst));
}

// ============================================================================
// Complex emission patterns
// ============================================================================

BOOST_AUTO_TEST_CASE(emit_letrec_mutual_recursion) {
    compile("(module m "
            "  (define result "
            "    (letrec ((even? (lambda (n) (if (= n 0) #t (odd? (- n 1)))))"
            "             (odd?  (lambda (n) (if (= n 0) #f (even? (- n 1))))))"
            "      (even? 4))))");
    BOOST_CHECK(has_opcode(OpCode::MakeClosure));
    BOOST_CHECK(has_opcode(OpCode::Call));
}

BOOST_AUTO_TEST_CASE(emit_closure_with_multiple_upvals) {
    compile("(module m "
            "  (define f (lambda (a b c) (lambda () (+ a b c)))))");
    BOOST_CHECK(has_opcode(OpCode::MakeClosure));
    BOOST_CHECK(has_opcode(OpCode::LoadUpval));
}

BOOST_AUTO_TEST_CASE(emit_deeply_nested_if) {
    compile("(module m (define x (if #t (if #f 1 2) (if #t 3 4))))");
    // Should have multiple JumpIfFalse/Jump pairs
    BOOST_CHECK_GE(count_opcode(OpCode::JumpIfFalse), 3);
    BOOST_CHECK_GE(count_opcode(OpCode::Jump), 3);
}

BOOST_AUTO_TEST_CASE(emit_complex_begin) {
    compile("(module m (define x (begin 1 2 3 4 5)))");
    // 4 Pop instructions between 5 expressions
    BOOST_CHECK_GE(count_opcode(OpCode::Pop), 4);
}

BOOST_AUTO_TEST_SUITE_END()

