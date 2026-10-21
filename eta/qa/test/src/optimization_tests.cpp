#include <boost/test/unit_test.hpp>
#include <bit>
#include <cmath>

#include "eta/semantics/optimization_pass.h"
#include "eta/semantics/optimization_pipeline.h"
#include "eta/semantics/ir_visitor.h"
#include "eta/semantics/semantic_analyzer.h"
#include "eta/semantics/core_ir.h"
#include "eta/semantics/passes/constant_folding.h"
#include "eta/semantics/passes/dead_code_elimination.h"
#include "eta/semantics/passes/primitive_specialisation.h"

/// Full pipeline includes for end-to-end tests
#include "eta/reader/lexer.h"
#include "eta/reader/parser.h"
#include "eta/reader/expander.h"
#include "eta/reader/module_linker.h"
#include "eta/semantics/emitter.h"
#include "eta/runtime/vm/vm.h"
#include "eta/runtime/vm/bytecode.h"
#include "eta/runtime/memory/heap.h"
#include "eta/runtime/memory/intern_table.h"
#include "eta/runtime/builtin_env.h"
#include "eta/runtime/core_primitives.h"
#include "eta/runtime/factory.h"

using namespace eta::semantics;
using namespace eta::runtime;
using namespace eta::runtime::vm;
using namespace eta::runtime::nanbox;

/**
 * A no-op pass to verify the pipeline mechanics
 */

struct NoOpPass : OptimizationPass {
    int run_count = 0;

    std::string_view name() const noexcept override { return "noop"; }

    void run(ModuleSemantics& /*mod*/) override {
        ++run_count;
    }
};

/// A counting pass to verify ordering
struct CountingPass : OptimizationPass {
    std::string label;
    std::vector<std::string>* log;

    CountingPass(std::string label, std::vector<std::string>* log)
        : label(std::move(label)), log(log) {}

    std::string_view name() const noexcept override { return label; }

    void run(ModuleSemantics& /*mod*/) override {
        log->push_back(std::string(label));
    }
};

/**
 * Fixture for compile-and-run optimization tests
 */

struct OptFixture {
    memory::heap::Heap heap;
    memory::intern::InternTable intern_table;
    BytecodeFunctionRegistry registry;
    BuiltinEnvironment builtins;

    OptFixture() : heap(4 * 1024 * 1024) {
        register_core_primitives(builtins, heap, intern_table);
    }

    /**
     * Compile a module through the full pipeline with the given optimization
     * passes applied, then execute and return the 'result' binding.
     */
    LispVal run_with_passes(std::string_view source,
                            std::vector<std::unique_ptr<OptimizationPass>> passes) {
        eta::reader::lexer::Lexer lex(0, source);
        eta::reader::parser::Parser p(lex);
        auto parsed = std::move(*p.parse_toplevel());

        eta::reader::expander::Expander ex;
        auto expanded = std::move(*ex.expand_many(parsed));

        eta::reader::ModuleLinker linker;
        (void) linker.index_modules(expanded);
        (void) linker.link();

        SemanticAnalyzer sa;
        auto sem_mods = std::move(*sa.analyze_all(expanded, linker, builtins));

        /// Apply optimization passes
        OptimizationPipeline pipeline;
        for (auto& pass : passes) pipeline.add_pass(std::move(pass));
        pipeline.run_all(sem_mods);

        auto& sem_mod = sem_mods[0];

        Emitter emitter(sem_mod, heap, intern_table, registry);
        auto* main_func = emitter.emit();

        VM vm(heap, intern_table);
        vm.set_function_resolver([this](uint32_t idx) { return registry.get(idx); });
        (void)builtins.install(heap, vm.globals(), sem_mod.total_globals);

        auto exec_res = vm.execute(*main_func);
        if (!exec_res) throw std::runtime_error("Execution error");

        for (const auto& bi : sem_mod.bindings) {
            if (bi.name == "result") return vm.globals()[bi.slot];
        }
        return exec_res.value();
    }

    /// Compile with given passes, return the emitted BytecodeFunction (module init).
    const BytecodeFunction& compile_with_passes(
            std::string_view source,
            std::vector<std::unique_ptr<OptimizationPass>> passes) {
        eta::reader::lexer::Lexer lex(0, source);
        eta::reader::parser::Parser p(lex);
        auto parsed = std::move(*p.parse_toplevel());

        eta::reader::expander::Expander ex;
        auto expanded = std::move(*ex.expand_many(parsed));

        eta::reader::ModuleLinker linker;
        (void) linker.index_modules(expanded);
        (void) linker.link();

        SemanticAnalyzer sa;
        auto sem_mods = std::move(*sa.analyze_all(expanded, linker, builtins));

        OptimizationPipeline pipeline;
        for (auto& pass : passes) pipeline.add_pass(std::move(pass));
        pipeline.run_all(sem_mods);

        Emitter emitter(sem_mods[0], heap, intern_table, registry);
        emitter.emit();
        return registry.all().back(); ///< module init is last
    }

    int count_opcode(const BytecodeFunction& func, OpCode op) const {
        int count = 0;
        for (const auto& instr : func.code) {
            if (instr.opcode == op) ++count;
        }
        return count;
    }

    int count_opcode_all(OpCode op) const {
        int count = 0;
        for (const auto& func : registry.all()) {
            for (const auto& instr : func.code) {
                if (instr.opcode == op) ++count;
            }
        }
        return count;
    }
};

BOOST_AUTO_TEST_SUITE(optimization_tests)

/**
 * Pipeline mechanics
 */

BOOST_AUTO_TEST_CASE(empty_pipeline_does_nothing) {
    OptimizationPipeline pipeline;
    BOOST_CHECK(pipeline.empty());
    BOOST_CHECK_EQUAL(pipeline.size(), 0u);

    ModuleSemantics mod;
    mod.name = "test";
    pipeline.run(mod); ///< should not crash
}

BOOST_AUTO_TEST_CASE(single_pass_runs) {
    auto pass = std::make_unique<NoOpPass>();
    auto* raw = pass.get();

    OptimizationPipeline pipeline;
    pipeline.add_pass(std::move(pass));
    BOOST_CHECK_EQUAL(pipeline.size(), 1u);
    BOOST_CHECK(!pipeline.empty());

    ModuleSemantics mod;
    mod.name = "test";
    pipeline.run(mod);
    BOOST_CHECK_EQUAL(raw->run_count, 1);
}

BOOST_AUTO_TEST_CASE(pipeline_runs_passes_in_order) {
    std::vector<std::string> log;

    OptimizationPipeline pipeline;
    pipeline.add_pass(std::make_unique<CountingPass>("A", &log));
    pipeline.add_pass(std::make_unique<CountingPass>("B", &log));
    pipeline.add_pass(std::make_unique<CountingPass>("C", &log));

    ModuleSemantics mod;
    mod.name = "test";
    pipeline.run(mod);

    BOOST_REQUIRE_EQUAL(log.size(), 3u);
    BOOST_CHECK_EQUAL(log[0], "A");
    BOOST_CHECK_EQUAL(log[1], "B");
    BOOST_CHECK_EQUAL(log[2], "C");
}

BOOST_AUTO_TEST_CASE(run_all_processes_multiple_modules) {
    auto pass = std::make_unique<NoOpPass>();
    auto* raw = pass.get();

    OptimizationPipeline pipeline;
    pipeline.add_pass(std::move(pass));

    std::vector<ModuleSemantics> mods;
    mods.emplace_back();
    mods.back().name = "a";
    mods.emplace_back();
    mods.back().name = "b";
    mods.emplace_back();
    mods.back().name = "c";

    pipeline.run_all(mods);
    BOOST_CHECK_EQUAL(raw->run_count, 3);
}

BOOST_AUTO_TEST_CASE(pass_names_reported) {
    OptimizationPipeline pipeline;
    pipeline.add_pass(std::make_unique<CountingPass>("alpha", nullptr));
    pipeline.add_pass(std::make_unique<CountingPass>("beta", nullptr));

    auto names = pipeline.pass_names();
    BOOST_REQUIRE_EQUAL(names.size(), 2u);
    BOOST_CHECK_EQUAL(names[0], "alpha");
    BOOST_CHECK_EQUAL(names[1], "beta");
}

/**
 * IRVisitor smoke test
 */

BOOST_AUTO_TEST_CASE(ir_visitor_walks_const_node) {
    ModuleSemantics mod;
    mod.name = "test";
    eta::reader::parser::Span sp{};
    auto* node = mod.emplace<core::Const>(sp, core::Literal{int64_t{42}});

    struct Visitor : IRVisitor<Visitor> {
        int visited = 0;
        void pre_visit(core::Node*, bool) { ++visited; }
    };

    Visitor v;
    v.visit(node, false);
    BOOST_CHECK_EQUAL(v.visited, 1);
}

BOOST_AUTO_TEST_CASE(ir_visitor_walks_if_children) {
    ModuleSemantics mod;
    mod.name = "test";
    eta::reader::parser::Span sp{};
    auto* test_n  = mod.emplace<core::Const>(sp, core::Literal{true});
    auto* conseq  = mod.emplace<core::Const>(sp, core::Literal{int64_t{1}});
    auto* alt     = mod.emplace<core::Const>(sp, core::Literal{int64_t{2}});
    auto* if_node = mod.emplace<core::If>(sp, test_n, conseq, alt);

    struct Visitor : IRVisitor<Visitor> {
        int visited = 0;
        void pre_visit(core::Node*, bool) { ++visited; }
    };

    Visitor v;
    v.visit(if_node, false);
    BOOST_CHECK_EQUAL(v.visited, 4); ///< if + test + conseq + alt
}

/**
 */

BOOST_FIXTURE_TEST_CASE(constant_fold_add_fixnums, OptFixture) {
    /// (+ 2 3) should fold to 5 at the IR level
    std::vector<std::unique_ptr<OptimizationPass>> passes;
    passes.push_back(std::make_unique<passes::ConstantFolding>());

    auto result = run_with_passes(
        "(module m (define result (+ 2 3)))", std::move(passes));

    auto decoded = ops::decode<int64_t>(result);
    BOOST_REQUIRE(decoded.has_value());
    BOOST_CHECK_EQUAL(*decoded, 5);
}

BOOST_FIXTURE_TEST_CASE(constant_fold_add_doubles, OptFixture) {
    /// (+ 1.5 2.5) should fold to 4.0
    std::vector<std::unique_ptr<OptimizationPass>> passes;
    passes.push_back(std::make_unique<passes::ConstantFolding>());

    auto result = run_with_passes(
        "(module m (define result (+ 1.5 2.5)))", std::move(passes));

    double d = std::bit_cast<double>(result);
    if (ops::is_boxed(result)) {
        auto fix = ops::decode<int64_t>(result);
        if (fix) d = static_cast<double>(*fix);
    }
    BOOST_CHECK_CLOSE(d, 4.0, 1e-10);
}

BOOST_FIXTURE_TEST_CASE(constant_fold_sub_fixnums, OptFixture) {
    std::vector<std::unique_ptr<OptimizationPass>> passes;
    passes.push_back(std::make_unique<passes::ConstantFolding>());

    auto result = run_with_passes(
        "(module m (define result (- 10 3)))", std::move(passes));

    auto decoded = ops::decode<int64_t>(result);
    BOOST_REQUIRE(decoded.has_value());
    BOOST_CHECK_EQUAL(*decoded, 7);
}

BOOST_FIXTURE_TEST_CASE(constant_fold_mul_fixnums, OptFixture) {
    std::vector<std::unique_ptr<OptimizationPass>> passes;
    passes.push_back(std::make_unique<passes::ConstantFolding>());

    auto result = run_with_passes(
        "(module m (define result (* 4 5)))", std::move(passes));

    auto decoded = ops::decode<int64_t>(result);
    BOOST_REQUIRE(decoded.has_value());
    BOOST_CHECK_EQUAL(*decoded, 20);
}

BOOST_FIXTURE_TEST_CASE(constant_fold_div_exact, OptFixture) {
    std::vector<std::unique_ptr<OptimizationPass>> passes;
    passes.push_back(std::make_unique<passes::ConstantFolding>());

    auto result = run_with_passes(
        "(module m (define result (/ 10 2)))", std::move(passes));

    auto decoded = ops::decode<int64_t>(result);
    BOOST_REQUIRE(decoded.has_value());
    BOOST_CHECK_EQUAL(*decoded, 5);
}

BOOST_FIXTURE_TEST_CASE(no_fold_non_constant, OptFixture) {
    std::vector<std::unique_ptr<OptimizationPass>> passes;
    passes.push_back(std::make_unique<passes::ConstantFolding>());

    auto result = run_with_passes(
        "(module m (define x 10) (define result (+ x 1)))", std::move(passes));

    auto decoded = ops::decode<int64_t>(result);
    BOOST_REQUIRE(decoded.has_value());
    BOOST_CHECK_EQUAL(*decoded, 11); ///< correct result, but via runtime computation
}

BOOST_FIXTURE_TEST_CASE(constant_fold_reduces_instructions, OptFixture) {
    /**
     * With folding, (+ 2 3) should emit fewer instructions (one LoadConst
     * instead of LoadConst + LoadConst + LoadGlobal + Call)
     */
    std::vector<std::unique_ptr<OptimizationPass>> passes;
    passes.push_back(std::make_unique<passes::ConstantFolding>());

    auto& func = compile_with_passes(
        "(module m (define result (+ 2 3)))", std::move(passes));

    /// Should NOT contain a Call instruction (the call to + was folded away)
    BOOST_CHECK_EQUAL(count_opcode(func, OpCode::Call), 0);
}

BOOST_FIXTURE_TEST_CASE(no_fold_keeps_add, OptFixture) {
    /**
     * Without folding, (+ x 1) must still have a Call (the emitter compiles
     * arithmetic builtins as generic Call instructions when no primitive
     * specialisation pass is enabled.
     */
    auto& func = compile_with_passes(
        "(module m (define x 10) (define result (+ x 1)))", {});

    BOOST_CHECK_GE(count_opcode_all(OpCode::Call), 1);
}

/**
 * Primitive specialisation
 */

BOOST_FIXTURE_TEST_CASE(primitive_specialisation_arithmetic_opcodes, OptFixture) {
    std::vector<std::unique_ptr<OptimizationPass>> passes;
    passes.push_back(std::make_unique<passes::PrimitiveSpecialisation>());

    auto result = run_with_passes(
        "(module m "
        "  (define x 10) "
        "  (define y 2) "
        "  (define a (+ x y)) "
        "  (define b (- x y)) "
        "  (define c (* x y)) "
        "  (define d (/ x y)) "
        "  (define result (+ a (+ b (+ c d)))))",
        std::move(passes));

    auto decoded = ops::decode<int64_t>(result);
    BOOST_REQUIRE(decoded.has_value());
    BOOST_CHECK_EQUAL(*decoded, 45);

    BOOST_CHECK_GE(count_opcode_all(OpCode::Add), 1);
    BOOST_CHECK_GE(count_opcode_all(OpCode::Sub), 1);
    BOOST_CHECK_GE(count_opcode_all(OpCode::Mul), 1);
    BOOST_CHECK_GE(count_opcode_all(OpCode::Div), 1);
    BOOST_CHECK_EQUAL(count_opcode_all(OpCode::Call), 0);
}

BOOST_FIXTURE_TEST_CASE(primitive_specialisation_list_and_eq_opcodes, OptFixture) {
    std::vector<std::unique_ptr<OptimizationPass>> passes;
    passes.push_back(std::make_unique<passes::PrimitiveSpecialisation>());

    auto result = run_with_passes(
        "(module m "
        "  (define p (cons 1 2)) "
        "  (define l (car p)) "
        "  (define r (cdr p)) "
        "  (define same (= l 1)) "
        "  (define result (if same (+ l r) 0)))",
        std::move(passes));

    auto decoded = ops::decode<int64_t>(result);
    BOOST_REQUIRE(decoded.has_value());
    BOOST_CHECK_EQUAL(*decoded, 3);

    BOOST_CHECK_GE(count_opcode_all(OpCode::Cons), 1);
    BOOST_CHECK_GE(count_opcode_all(OpCode::Car), 1);
    BOOST_CHECK_GE(count_opcode_all(OpCode::Cdr), 1);
    /// Numeric = lowers to NumCmp, not Eq (eqv?)
    BOOST_CHECK_GE(count_opcode_all(OpCode::NumCmp), 1);
    BOOST_CHECK_EQUAL(count_opcode_all(OpCode::Eq), 0);
    BOOST_CHECK_EQUAL(count_opcode_all(OpCode::Call), 0);
}

BOOST_FIXTURE_TEST_CASE(primitive_specialisation_lowers_variadic_plus_to_chain, OptFixture) {
    std::vector<std::unique_ptr<OptimizationPass>> passes;
    passes.push_back(std::make_unique<passes::PrimitiveSpecialisation>());

    auto result = run_with_passes(
        "(module m (define result (+ 1 2 3)))",
        std::move(passes));

    auto decoded = ops::decode<int64_t>(result);
    BOOST_REQUIRE(decoded.has_value());
    BOOST_CHECK_EQUAL(*decoded, 6);

    /// (+ 1 2 3) -> (Add (Add 1 2) 3)
    BOOST_CHECK_EQUAL(count_opcode_all(OpCode::Add), 2);
    BOOST_CHECK_EQUAL(count_opcode_all(OpCode::Call), 0);
}

namespace {

/// Left-fold semantics of the variadic primitives, including inexact division.
constexpr std::string_view kVariadicArithProgram =
    "(module m "
    "  (define a 10) (define b 4) (define c 2) (define d 3) "
    "  (define result (if (= (+ a b c d) 19) "
    "                   (if (= (- a b c d) 1) "
    "                     (if (= (* a b c d) 240) "
    "                       (if (= (/ a b c) 1.25) 1 0) 0) 0) 0)))";

/// 2^62 + 2^62 overflows int64 part-way through the chain and promotes to flonum.
constexpr std::string_view kVariadicOverflowProgram =
    "(module m (define big 4611686018427387904) (define result (+ big big 1)))";

} // namespace

BOOST_FIXTURE_TEST_CASE(variadic_arith_chain_semantics, OptFixture) {
    std::vector<std::unique_ptr<OptimizationPass>> passes;
    passes.push_back(std::make_unique<passes::PrimitiveSpecialisation>());

    auto result = run_with_passes(kVariadicArithProgram, std::move(passes));
    auto decoded = ops::decode<int64_t>(result);
    BOOST_REQUIRE(decoded.has_value());
    BOOST_CHECK_EQUAL(*decoded, 1);
    BOOST_CHECK_EQUAL(count_opcode_all(OpCode::Add), 3);
    BOOST_CHECK_EQUAL(count_opcode_all(OpCode::Sub), 3);
    BOOST_CHECK_EQUAL(count_opcode_all(OpCode::Mul), 3);
    BOOST_CHECK_EQUAL(count_opcode_all(OpCode::Div), 2);
    BOOST_CHECK_EQUAL(count_opcode_all(OpCode::Call), 0);
}

BOOST_FIXTURE_TEST_CASE(variadic_arith_chain_matches_primitive, OptFixture) {
    auto result = run_with_passes(kVariadicArithProgram, {});
    auto decoded = ops::decode<int64_t>(result);
    BOOST_REQUIRE(decoded.has_value());
    BOOST_CHECK_EQUAL(*decoded, 1);
}

BOOST_FIXTURE_TEST_CASE(variadic_arith_chain_overflow_matches_primitive, OptFixture) {
    std::vector<std::unique_ptr<OptimizationPass>> passes;
    passes.push_back(std::make_unique<passes::PrimitiveSpecialisation>());
    auto chained = run_with_passes(kVariadicOverflowProgram, std::move(passes));
    BOOST_CHECK_EQUAL(count_opcode_all(OpCode::Add), 2);

    auto primitive = run_with_passes(kVariadicOverflowProgram, {});

    BOOST_REQUIRE(!ops::is_boxed(chained));
    BOOST_REQUIRE(!ops::is_boxed(primitive));
    BOOST_CHECK_EQUAL(std::bit_cast<double>(chained), 9223372036854775808.0);
    BOOST_CHECK_EQUAL(std::bit_cast<double>(chained), std::bit_cast<double>(primitive));
}

BOOST_FIXTURE_TEST_CASE(nullary_and_unary_arith_stay_calls, OptFixture) {
    std::vector<std::unique_ptr<OptimizationPass>> passes;
    passes.push_back(std::make_unique<passes::PrimitiveSpecialisation>());

    auto result = run_with_passes(
        "(module m (define a 4) "
        "  (define result (if (= (+) 0) (if (= (- a) -4) (if (= (/ a) 0.25) 1 0) 0) 0)))",
        std::move(passes));
    auto decoded = ops::decode<int64_t>(result);
    BOOST_REQUIRE(decoded.has_value());
    BOOST_CHECK_EQUAL(*decoded, 1);
    BOOST_CHECK_EQUAL(count_opcode_all(OpCode::Add), 0);
    BOOST_CHECK_EQUAL(count_opcode_all(OpCode::Sub), 0);
    BOOST_CHECK_EQUAL(count_opcode_all(OpCode::Div), 0);
    BOOST_CHECK_GE(count_opcode_all(OpCode::Call), 3);
}

BOOST_FIXTURE_TEST_CASE(variadic_arith_chain_division_by_zero_is_catchable, OptFixture) {
    std::vector<std::unique_ptr<OptimizationPass>> passes;
    passes.push_back(std::make_unique<passes::PrimitiveSpecialisation>());

    auto result = run_with_passes(
        "(module m (define a 6) (define z 0) "
        "  (define p (catch 'runtime.type-error (/ a 2 z))) "
        "  (define result (if (pair? p) 1 0)))",
        std::move(passes));
    auto decoded = ops::decode<int64_t>(result);
    BOOST_REQUIRE(decoded.has_value());
    BOOST_CHECK_EQUAL(*decoded, 1);
    BOOST_CHECK_EQUAL(count_opcode_all(OpCode::Div), 2);
}

BOOST_FIXTURE_TEST_CASE(primitive_specialisation_respects_shadowed_plus, OptFixture) {
    std::vector<std::unique_ptr<OptimizationPass>> passes;
    passes.push_back(std::make_unique<passes::PrimitiveSpecialisation>());

    auto result = run_with_passes(
        "(module m "
        "  (define + (lambda (a b) (- a b))) "
        "  (define x 7) "
        "  (define y 2) "
        "  (define result (+ x y)))",
        std::move(passes));

    auto decoded = ops::decode<int64_t>(result);
    BOOST_REQUIRE(decoded.has_value());
    BOOST_CHECK_EQUAL(*decoded, 5);

    BOOST_CHECK_EQUAL(count_opcode_all(OpCode::Add), 0);
    BOOST_CHECK_GE(count_opcode_all(OpCode::Call), 1);
}

BOOST_FIXTURE_TEST_CASE(primitive_specialisation_rewrites_tail_primitive_call, OptFixture) {
    std::vector<std::unique_ptr<OptimizationPass>> passes;
    passes.push_back(std::make_unique<passes::PrimitiveSpecialisation>());

    auto result = run_with_passes(
        "(module m "
        "  (define add2 (lambda (x y) (+ x y))) "
        "  (define result (add2 1 2)))",
        std::move(passes));

    auto decoded = ops::decode<int64_t>(result);
    BOOST_REQUIRE(decoded.has_value());
    BOOST_CHECK_EQUAL(*decoded, 3);

    BOOST_CHECK_GE(count_opcode_all(OpCode::Add), 1);
    BOOST_CHECK_EQUAL(count_opcode_all(OpCode::TailCall), 0);
}

BOOST_FIXTURE_TEST_CASE(pipeline_constant_fold_then_specialise, OptFixture) {
    std::vector<std::unique_ptr<OptimizationPass>> passes;
    passes.push_back(std::make_unique<passes::ConstantFolding>());
    passes.push_back(std::make_unique<passes::PrimitiveSpecialisation>());
    passes.push_back(std::make_unique<passes::DeadCodeElimination>());

    auto& func = compile_with_passes(
        "(module m (define result (+ 2 3)))",
        std::move(passes));

    BOOST_CHECK_EQUAL(count_opcode(func, OpCode::Call), 0);
    BOOST_CHECK_EQUAL(count_opcode(func, OpCode::Add), 0);
}

BOOST_FIXTURE_TEST_CASE(pipeline_specialises_non_constant_arithmetic, OptFixture) {
    std::vector<std::unique_ptr<OptimizationPass>> passes;
    passes.push_back(std::make_unique<passes::ConstantFolding>());
    passes.push_back(std::make_unique<passes::PrimitiveSpecialisation>());
    passes.push_back(std::make_unique<passes::DeadCodeElimination>());

    auto& func = compile_with_passes(
        "(module m (define x 10) (define result (+ x 1)))",
        std::move(passes));

    BOOST_CHECK_EQUAL(count_opcode(func, OpCode::Call), 0);
    BOOST_CHECK_GE(count_opcode(func, OpCode::Add), 1);
}

/**
 * Numeric comparison: NumCmp opcode must agree with the = < > <= >= primitives
 */

namespace {

/// Same program run with and without specialisation must give the same answer.
constexpr std::string_view kNumericEqProgram =
    "(module m "
    "  (define i 1) (define f 1.0) (define z 0.0) (define nz -0.0) (define nan +nan.0) "
    "  (define result (if (= i f) "
    "                   (if (= z nz) "
    "                     (if (= nan nan) 0 "
    "                       (if (= i 2) 0 1)) 0) 0)))";

constexpr std::string_view kComparisonProgram =
    "(module m "
    "  (define a 1) (define b 2.5) (define c 2) (define nan +nan.0) "
    "  (define result (if (< a b) "
    "                   (if (> b c) "
    "                     (if (<= c c) "
    "                       (if (>= c a) "
    "                         (if (< a nan) 0 "
    "                           (if (> a nan) 0 1)) 0) 0) 0) 0)))";

} // namespace

BOOST_FIXTURE_TEST_CASE(numcmp_numeric_eq_semantics, OptFixture) {
    std::vector<std::unique_ptr<OptimizationPass>> passes;
    passes.push_back(std::make_unique<passes::PrimitiveSpecialisation>());

    /// (= 1 1.0) #t, (= 0.0 -0.0) #t, (= nan nan) #f, (= 1 2) #f
    auto result = run_with_passes(kNumericEqProgram, std::move(passes));
    auto decoded = ops::decode<int64_t>(result);
    BOOST_REQUIRE(decoded.has_value());
    BOOST_CHECK_EQUAL(*decoded, 1);
    BOOST_CHECK_GE(count_opcode_all(OpCode::NumCmp), 4);
    BOOST_CHECK_EQUAL(count_opcode_all(OpCode::Eq), 0);
}

BOOST_FIXTURE_TEST_CASE(numcmp_numeric_eq_matches_primitive, OptFixture) {
    auto result = run_with_passes(kNumericEqProgram, {});
    auto decoded = ops::decode<int64_t>(result);
    BOOST_REQUIRE(decoded.has_value());
    BOOST_CHECK_EQUAL(*decoded, 1);
    BOOST_CHECK_EQUAL(count_opcode_all(OpCode::NumCmp), 0);
}

BOOST_FIXTURE_TEST_CASE(numcmp_ordering_semantics, OptFixture) {
    std::vector<std::unique_ptr<OptimizationPass>> passes;
    passes.push_back(std::make_unique<passes::PrimitiveSpecialisation>());

    auto result = run_with_passes(kComparisonProgram, std::move(passes));
    auto decoded = ops::decode<int64_t>(result);
    BOOST_REQUIRE(decoded.has_value());
    BOOST_CHECK_EQUAL(*decoded, 1);
    BOOST_CHECK_GE(count_opcode_all(OpCode::NumCmp), 6);
    BOOST_CHECK_EQUAL(count_opcode_all(OpCode::Call), 0);
}

BOOST_FIXTURE_TEST_CASE(numcmp_ordering_matches_primitive, OptFixture) {
    auto result = run_with_passes(kComparisonProgram, {});
    auto decoded = ops::decode<int64_t>(result);
    BOOST_REQUIRE(decoded.has_value());
    BOOST_CHECK_EQUAL(*decoded, 1);
}

BOOST_FIXTURE_TEST_CASE(numcmp_variadic_comparison_stays_call, OptFixture) {
    std::vector<std::unique_ptr<OptimizationPass>> passes;
    passes.push_back(std::make_unique<passes::PrimitiveSpecialisation>());

    auto result = run_with_passes(
        "(module m (define a 1) (define result (if (< a 2 3) 1 0)))",
        std::move(passes));
    auto decoded = ops::decode<int64_t>(result);
    BOOST_REQUIRE(decoded.has_value());
    BOOST_CHECK_EQUAL(*decoded, 1);
    BOOST_CHECK_EQUAL(count_opcode_all(OpCode::NumCmp), 0);
    BOOST_CHECK_GE(count_opcode_all(OpCode::Call), 1);
}

BOOST_FIXTURE_TEST_CASE(numcmp_type_error_is_catchable, OptFixture) {
    std::vector<std::unique_ptr<OptimizationPass>> passes;
    passes.push_back(std::make_unique<passes::PrimitiveSpecialisation>());

    auto result = run_with_passes(
        "(module m "
        "  (define a 1) "
        "  (define p (catch 'runtime.type-error (< a 'x))) "
        "  (define result (if (pair? p) (if (eq? (car p) 'runtime-error) 1 0) 0)))",
        std::move(passes));
    auto decoded = ops::decode<int64_t>(result);
    BOOST_REQUIRE(decoded.has_value());
    BOOST_CHECK_EQUAL(*decoded, 1);
    BOOST_CHECK_GE(count_opcode_all(OpCode::NumCmp), 1);
}

BOOST_FIXTURE_TEST_CASE(arith_opcode_type_error_is_catchable, OptFixture) {
    std::vector<std::unique_ptr<OptimizationPass>> passes;
    passes.push_back(std::make_unique<passes::PrimitiveSpecialisation>());

    auto result = run_with_passes(
        "(module m "
        "  (define a 1) "
        "  (define p (catch 'runtime.type-error (+ a 'x))) "
        "  (define q (catch 'runtime.type-error (/ a 0))) "
        "  (define result (if (pair? p) (if (pair? q) 1 0) 0)))",
        std::move(passes));
    auto decoded = ops::decode<int64_t>(result);
    BOOST_REQUIRE(decoded.has_value());
    BOOST_CHECK_EQUAL(*decoded, 1);
    BOOST_CHECK_GE(count_opcode_all(OpCode::Add), 1);
    BOOST_CHECK_GE(count_opcode_all(OpCode::Div), 1);
}

BOOST_FIXTURE_TEST_CASE(car_cdr_opcode_type_error_is_catchable, OptFixture) {
    std::vector<std::unique_ptr<OptimizationPass>> passes;
    passes.push_back(std::make_unique<passes::PrimitiveSpecialisation>());

    auto result = run_with_passes(
        "(module m "
        "  (define a 1) "
        "  (define p (catch 'runtime.type-error (car a))) "
        "  (define q (catch 'runtime.type-error (cdr a))) "
        "  (define result (if (pair? p) (if (pair? q) 1 0) 0)))",
        std::move(passes));
    auto decoded = ops::decode<int64_t>(result);
    BOOST_REQUIRE(decoded.has_value());
    BOOST_CHECK_EQUAL(*decoded, 1);
    BOOST_CHECK_EQUAL(count_opcode_all(OpCode::Car), 1);
    BOOST_CHECK_EQUAL(count_opcode_all(OpCode::Cdr), 1);
}

BOOST_FIXTURE_TEST_CASE(plus_negative_zero_matches_opcode, OptFixture) {
    /// (+ -0.0 -0.0) is -0.0 through both the Add opcode and the primitive
    constexpr std::string_view program =
        "(module m (define nz -0.0) (define result (+ nz nz)))";

    std::vector<std::unique_ptr<OptimizationPass>> passes;
    passes.push_back(std::make_unique<passes::PrimitiveSpecialisation>());
    auto opcode = run_with_passes(program, std::move(passes));
    auto primitive = run_with_passes(program, {});

    BOOST_REQUIRE(!ops::is_boxed(opcode));
    BOOST_REQUIRE(!ops::is_boxed(primitive));
    BOOST_CHECK(std::signbit(std::bit_cast<double>(opcode)));
    BOOST_CHECK(std::signbit(std::bit_cast<double>(primitive)));
}

BOOST_FIXTURE_TEST_CASE(constant_fold_after_specialisation_folds_chain, OptFixture) {
    /// Driver order: specialisation always runs first, then etac -O's folding
    std::vector<std::unique_ptr<OptimizationPass>> passes;
    passes.push_back(std::make_unique<passes::PrimitiveSpecialisation>());
    passes.push_back(std::make_unique<passes::ConstantFolding>());
    passes.push_back(std::make_unique<passes::DeadCodeElimination>());

    auto result = run_with_passes(
        "(module m (define result (* (+ 1 2 3) 2)))", std::move(passes));
    auto decoded = ops::decode<int64_t>(result);
    BOOST_REQUIRE(decoded.has_value());
    BOOST_CHECK_EQUAL(*decoded, 12);
    BOOST_CHECK_EQUAL(count_opcode_all(OpCode::Add), 0);
    BOOST_CHECK_EQUAL(count_opcode_all(OpCode::Mul), 0);
    BOOST_CHECK_EQUAL(count_opcode_all(OpCode::Call), 0);
}

BOOST_FIXTURE_TEST_CASE(constant_fold_after_specialisation_keeps_division_by_zero, OptFixture) {
    std::vector<std::unique_ptr<OptimizationPass>> passes;
    passes.push_back(std::make_unique<passes::PrimitiveSpecialisation>());
    passes.push_back(std::make_unique<passes::ConstantFolding>());

    auto result = run_with_passes(
        "(module m (define p (catch 'runtime.type-error (/ 1 0))) "
        "  (define result (if (pair? p) 1 0)))",
        std::move(passes));
    auto decoded = ops::decode<int64_t>(result);
    BOOST_REQUIRE(decoded.has_value());
    BOOST_CHECK_EQUAL(*decoded, 1);
    BOOST_CHECK_GE(count_opcode_all(OpCode::Div), 1);
}

BOOST_FIXTURE_TEST_CASE(constant_fold_respects_shadowed_plus, OptFixture) {
    std::vector<std::unique_ptr<OptimizationPass>> passes;
    passes.push_back(std::make_unique<passes::ConstantFolding>());

    auto result = run_with_passes(
        "(module m "
        "  (define + (lambda (a b) (- a b))) "
        "  (define result (+ 7 2)))",
        std::move(passes));
    auto decoded = ops::decode<int64_t>(result);
    BOOST_REQUIRE(decoded.has_value());
    BOOST_CHECK_EQUAL(*decoded, 5);
}

BOOST_FIXTURE_TEST_CASE(constant_fold_overflow_promotes_like_runtime, OptFixture) {
    /// 2^62 * 4 overflows int64: folding must promote to flonum, as the runtime does
    std::vector<std::unique_ptr<OptimizationPass>> passes;
    passes.push_back(std::make_unique<passes::ConstantFolding>());

    auto result = run_with_passes(
        "(module m (define result (* 4611686018427387904 4)))", std::move(passes));

    BOOST_REQUIRE(!ops::is_boxed(result));
    BOOST_CHECK_EQUAL(std::bit_cast<double>(result), 18446744073709551616.0);
}

/**
 */

BOOST_FIXTURE_TEST_CASE(dead_code_elimination_begin, OptFixture) {
    /// After dead-code elimination, only 99 remains.
    std::vector<std::unique_ptr<OptimizationPass>> passes;
    passes.push_back(std::make_unique<passes::DeadCodeElimination>());

    auto result = run_with_passes(
        "(module m (define result (begin 42 99)))", std::move(passes));

    auto decoded = ops::decode<int64_t>(result);
    BOOST_REQUIRE(decoded.has_value());
    BOOST_CHECK_EQUAL(*decoded, 99);
}

BOOST_FIXTURE_TEST_CASE(dead_code_elimination_reduces_pop, OptFixture) {
    /**
     * With dead-code elimination, (begin 42 99) should not emit a Pop
     * for the dead constant 42.  One Pop remains: the module-init top-level
     * Pop that the emitter adds after every toplevel form.
     */
    std::vector<std::unique_ptr<OptimizationPass>> passes;
    passes.push_back(std::make_unique<passes::DeadCodeElimination>());

    auto& func = compile_with_passes(
        "(module m (define result (begin 42 99)))", std::move(passes));

    /**
     * The dead constant + Pop pair inside the Begin is eliminated; only the
     * structural module-init Pop remains.
     */
    BOOST_CHECK_EQUAL(count_opcode(func, OpCode::Pop), 1);
}

BOOST_FIXTURE_TEST_CASE(dead_code_keeps_side_effects, OptFixture) {
    std::vector<std::unique_ptr<OptimizationPass>> passes;
    passes.push_back(std::make_unique<passes::DeadCodeElimination>());

    auto result = run_with_passes(
        "(module m (define x 0) (define result (begin (set! x 1) x)))",
        std::move(passes));

    auto decoded = ops::decode<int64_t>(result);
    BOOST_REQUIRE(decoded.has_value());
    BOOST_CHECK_EQUAL(*decoded, 1);
}

BOOST_FIXTURE_TEST_CASE(dead_code_multiple_dead, OptFixture) {
    std::vector<std::unique_ptr<OptimizationPass>> passes;
    passes.push_back(std::make_unique<passes::DeadCodeElimination>());

    auto result = run_with_passes(
        "(module m (define result (begin 1 2 3 4 5)))", std::move(passes));

    auto decoded = ops::decode<int64_t>(result);
    BOOST_REQUIRE(decoded.has_value());
    BOOST_CHECK_EQUAL(*decoded, 5);
}

/**
 */

BOOST_FIXTURE_TEST_CASE(pipeline_fold_then_dce, OptFixture) {
    /**
     * (begin (+ 1 2) (+ 3 4))
     * After constant folding: (begin 3 7)
     * After dead-code elimination: 7
     */
    std::vector<std::unique_ptr<OptimizationPass>> passes;
    passes.push_back(std::make_unique<passes::ConstantFolding>());
    passes.push_back(std::make_unique<passes::DeadCodeElimination>());

    auto result = run_with_passes(
        "(module m (define result (begin (+ 1 2) (+ 3 4))))",
        std::move(passes));

    auto decoded = ops::decode<int64_t>(result);
    BOOST_REQUIRE(decoded.has_value());
    BOOST_CHECK_EQUAL(*decoded, 7);
}

BOOST_AUTO_TEST_SUITE_END()

