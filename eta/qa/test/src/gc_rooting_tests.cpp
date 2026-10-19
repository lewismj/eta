/**
 * @file gc_rooting_tests.cpp
 * @brief Regression tests for heap values left unrooted across an allocation.
 *
 * Each test enables Heap::set_gc_torture so that every allocation runs a full
 * collection first.  Any value held only in a C++ local (popped off the VM
 * stack, or collected into a std::vector) while the runtime allocates is then
 * swept deterministically, instead of only when the soft limit happens to be
 * crossed at that exact allocation.
 *
 * Test matrix:
 *   - torture_control:        torture mode alone does not break simple programs.
 *   - cons_opcode_*:          OpCode::Cons pops car/cdr before alloc_cons.  Only the
 *                             PrimitiveSpecialisation pass (etac -O) emits Cons, so
 *                             these compile through that pass instead of the Driver.
 *   - values_opcode_*:        OpCode::Values pops into a vector before allocating.
 *   - emitter_*:              constants are unrooted until their function reaches the registry.
 *   - rest_args_*:            VM::pack_rest_args builds the rest list in a C++ local.
 *   - call_value_*:           VM::call_value holds the caller's closure in a C++ local.
 *   - clp_get_domain_*:       %clp-get-domain conses its result with unrooted tails.
 *   - make_closure_*:         OpCode::MakeClosure pops upvalues before allocating.
 *   - copy_term_*:            VM::copy_term holds partial copies and fresh vars in C++ locals.
 *   - json_read_*:            json_to_lisp keeps children in a local vector while recursing.
 *
 * Results are inspected from C++ with bounded walks, because a dangling cons
 * ID is usually recycled and can turn a list into a cycle.
 */

#include <boost/test/unit_test.hpp>

#include <expected>
#include <memory>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <eta/interpreter/module_path.h>
#include <eta/reader/expander.h>
#include <eta/reader/lexer.h>
#include <eta/reader/module_linker.h>
#include <eta/reader/parser.h>
#include <eta/runtime/builtin_env.h>
#include <eta/runtime/core_primitives.h>
#include <eta/runtime/error.h>
#include <eta/runtime/factory.h>
#include <eta/runtime/memory/heap.h>
#include <eta/runtime/memory/intern_table.h>
#include <eta/runtime/memory/mark_sweep_gc.h>
#include <eta/runtime/nanbox.h>
#include <eta/runtime/types/types.h>
#include <eta/runtime/vm/bytecode.h>
#include <eta/runtime/vm/vm.h>
#include <eta/semantics/emitter.h>
#include <eta/semantics/optimization_pipeline.h>
#include <eta/semantics/passes/primitive_specialisation.h>
#include <eta/semantics/semantic_analyzer.h>
#include <eta/session/driver.h>

using namespace eta::runtime;
using namespace eta::runtime::nanbox;
using namespace eta::runtime::memory::heap;
using namespace eta::runtime::memory::gc;
using namespace eta::runtime::memory::intern;
using namespace eta::runtime::memory::factory;
using namespace eta::runtime::error;

namespace {

struct TortureHarness {
    eta::session::Driver driver{eta::interpreter::ModulePathResolver{}};

    [[nodiscard]] std::string diagnostics_string() const {
        std::ostringstream oss;
        driver.diagnostics().print_all(oss, /*use_color=*/false, driver.file_resolver());
        return oss.str();
    }

    /// Run @p source with GC torture enabled and return the `result` binding.
    LispVal run_tortured(std::string_view source) {
        LispVal result{Nil};
        driver.heap().set_gc_torture(true);
        const bool ok = driver.run_source(source, &result, "result");
        driver.heap().set_gc_torture(false);
        BOOST_REQUIRE_MESSAGE(ok, "run_source failed under GC torture:\n" + diagnostics_string());
        return result;
    }
};

bool is_heap_ref(LispVal v) {
    return ops::is_boxed(v) && ops::tag(v) == Tag::HeapObject;
}

int64_t as_int(LispVal v) {
    auto decoded = ops::decode<int64_t>(v);
    BOOST_REQUIRE_MESSAGE(decoded.has_value(), "expected a fixnum");
    return *decoded;
}

const types::Cons* as_cons(Heap& heap, LispVal v) {
    if (!is_heap_ref(v)) return nullptr;
    return heap.cons_pool().try_get(static_cast<ObjectId>(ops::payload(v)));
}

const types::Vector* as_vector(Heap& heap, LispVal v) {
    if (!is_heap_ref(v)) return nullptr;
    return heap.try_get_as<ObjectKind::Vector, types::Vector>(ops::payload(v));
}

/// Collect the cars of a proper list, giving up after @p limit cells.
std::vector<LispVal> list_items(Heap& heap, LispVal head, std::size_t limit = 16) {
    std::vector<LispVal> out;
    while (head != Nil && out.size() <= limit) {
        const auto* c = as_cons(heap, head);
        BOOST_REQUIRE_MESSAGE(c != nullptr, "list cell " << out.size() << " is not a live pair");
        out.push_back(c->car);
        head = c->cdr;
    }
    return out;
}

void require_int_vector(Heap& heap, LispVal v, const std::vector<int64_t>& expected, const char* what) {
    const auto* vec = as_vector(heap, v);
    BOOST_REQUIRE_MESSAGE(vec != nullptr, what << " is not a live vector");
    BOOST_REQUIRE_EQUAL(vec->elements.size(), expected.size());
    for (std::size_t i = 0; i < expected.size(); ++i) {
        BOOST_CHECK_EQUAL(as_int(vec->elements[i]), expected[i]);
    }
}

std::expected<LispVal, RuntimeError> call_builtin(
    BuiltinEnvironment& env,
    const std::string& name,
    const std::vector<LispVal>& args) {
    auto idx = env.lookup(name);
    if (!idx) {
        return std::unexpected(RuntimeError{
            VMError{RuntimeErrorCode::InternalError, "missing builtin: " + name}});
    }
    return env.specs()[*idx].func(args);
}

/**
 * Compile-and-run fixture with PrimitiveSpecialisation enabled, mirroring
 * `etac -O`, so that calls to `cons` are lowered to OpCode::Cons.  The VM
 * installs its own GC callback, so torture collections mark the VM roots.
 */
struct OptimisedHarness {
    Heap heap{4 * 1024 * 1024};
    InternTable intern_table;
    eta::semantics::BytecodeFunctionRegistry registry;
    BuiltinEnvironment builtins;
    std::unique_ptr<vm::VM> machine;

    OptimisedHarness() {
        register_core_primitives(builtins, heap, intern_table);
    }

    int count_opcode(vm::OpCode op) const {
        int count = 0;
        for (const auto& func : registry.all()) {
            for (const auto& instr : func.code) {
                if (instr.opcode == op) ++count;
            }
        }
        return count;
    }

    /// Compile @p source, then execute it with GC torture enabled and return `result`.
    LispVal run_tortured(std::string_view source) {
        eta::reader::lexer::Lexer lex(0, source);
        eta::reader::parser::Parser p(lex);
        auto parsed = p.parse_toplevel();
        BOOST_REQUIRE(parsed.has_value());

        eta::reader::expander::Expander ex;
        auto expanded = ex.expand_many(*parsed);
        BOOST_REQUIRE(expanded.has_value());

        eta::reader::ModuleLinker linker;
        (void) linker.index_modules(*expanded);
        (void) linker.link();

        eta::semantics::SemanticAnalyzer sa;
        auto sem_mods = sa.analyze_all(*expanded, linker, builtins);
        BOOST_REQUIRE(sem_mods.has_value());

        eta::semantics::OptimizationPipeline pipeline;
        pipeline.add_pass(std::make_unique<eta::semantics::passes::PrimitiveSpecialisation>());
        pipeline.run_all(*sem_mods);

        auto& sem_mod = (*sem_mods)[0];
        eta::semantics::Emitter emitter(sem_mod, heap, intern_table, registry);
        auto* main_func = emitter.emit();

        machine = std::make_unique<vm::VM>(heap, intern_table);
        machine->set_function_resolver([this](uint32_t idx) { return registry.get(idx); });
        (void) builtins.install(heap, machine->globals(), sem_mod.total_globals);

        heap.set_gc_torture(true);
        auto exec_res = machine->execute(*main_func);
        heap.set_gc_torture(false);
        BOOST_REQUIRE_MESSAGE(exec_res.has_value(), "execution failed under GC torture");

        for (const auto& bi : sem_mod.bindings) {
            if (bi.name == "result") return machine->globals()[bi.slot];
        }
        BOOST_FAIL("module has no `result` binding");
        return Nil;
    }
};

/**
 * Builtin-level fixture: a heap whose only GC roots are the explicit `roots`
 * vector plus any ExternalRootFrame, matching what the VM would see while a
 * builtin runs with its arguments on the stack.
 */
struct BuiltinHarness {
    Heap heap{1ull << 22};
    InternTable intern_table;
    BuiltinEnvironment env;
    MarkSweepGC gc;
    std::vector<LispVal> roots;

    BuiltinHarness() {
        register_core_primitives(env, heap, intern_table, nullptr);
        heap.set_gc_callback([this]() {
            gc.collect(heap, [this](auto&& visit) {
                for (auto v : roots) visit(v);
                for (auto v : heap.external_roots()) visit(v);
            });
        });
    }

    LispVal read_json_tortured(std::string_view text) {
        auto input = make_string(heap, intern_table, std::string(text));
        BOOST_REQUIRE(input.has_value());
        roots.push_back(*input);

        heap.set_gc_torture(true);
        auto parsed = call_builtin(env, "%json-read-string", {*input, True});
        heap.set_gc_torture(false);
        BOOST_REQUIRE_MESSAGE(parsed.has_value(), "%json-read-string failed under GC torture");
        roots.push_back(*parsed);
        return *parsed;
    }
};

} // namespace

BOOST_AUTO_TEST_SUITE(gc_rooting_tests)

/**
 * @test torture_control
 *
 * Compiles and runs a program that keeps no heap temporaries across an
 * allocation.  If this fails, the failure is in compilation or builtin
 * installation under torture, not in the opcode paths tested below.
 */
BOOST_AUTO_TEST_CASE(torture_control) {
    TortureHarness h;
    auto result = h.run_tortured(R"eta(
(module gc_rooting.control
  (define (add a b) (+ a b))
  (define result (add 40 2)))
)eta");
    BOOST_TEST(as_int(result) == 42);
}

/**
 * @test cons_opcode_keeps_heap_operands_alive
 *
 * Both operands of the Cons are fresh vectors that live only on the VM stack.
 * OpCode::Cons pops them before alloc_cons, so under torture they are swept
 * and the new pair points at dead objects.
 */
BOOST_AUTO_TEST_CASE(cons_opcode_keeps_heap_operands_alive) {
    OptimisedHarness h;
    auto result = h.run_tortured(R"eta(
(module gc_rooting.cons_vectors
  (define (build n) (cons (vector n (+ n 1)) (vector (+ n 2) (+ n 3))))
  (define result (build 1)))
)eta");
    BOOST_REQUIRE_MESSAGE(h.count_opcode(vm::OpCode::Cons) > 0, "cons was not lowered to OpCode::Cons");

    auto& heap = h.heap;
    const auto* pair = as_cons(heap, result);
    BOOST_REQUIRE_MESSAGE(pair != nullptr, "result is not a live pair");
    require_int_vector(heap, pair->car, {1, 2}, "car");
    require_int_vector(heap, pair->cdr, {3, 4}, "cdr");
}

/**
 * @test cons_opcode_nested_list_is_not_aliased
 *
 * Each inner pair exists only on the VM stack when the outer Cons allocates.
 * Because cons-pool IDs are recycled, a swept inner pair is typically reused
 * for the outer pair, producing a cycle or a list with the wrong contents.
 */
BOOST_AUTO_TEST_CASE(cons_opcode_nested_list_is_not_aliased) {
    OptimisedHarness h;
    auto result = h.run_tortured(R"eta(
(module gc_rooting.cons_nested
  (define (build n) (cons n (cons (+ n 1) (cons (+ n 2) '()))))
  (define result (build 1)))
)eta");
    BOOST_REQUIRE_MESSAGE(h.count_opcode(vm::OpCode::Cons) >= 3, "cons was not lowered to OpCode::Cons");

    auto items = list_items(h.heap, result);
    BOOST_REQUIRE_EQUAL(items.size(), 3u);
    BOOST_CHECK_EQUAL(as_int(items[0]), 1);
    BOOST_CHECK_EQUAL(as_int(items[1]), 2);
    BOOST_CHECK_EQUAL(as_int(items[2]), 3);
}

/**
 * @test values_opcode_keeps_heap_values_alive
 *
 * OpCode::Values pops its operands into a local std::vector and then calls
 * make_multiple_values, which allocates.  The consumer reads the vectors back.
 */
BOOST_AUTO_TEST_CASE(values_opcode_keeps_heap_values_alive) {
    TortureHarness h;
    auto result = h.run_tortured(R"eta(
(module gc_rooting.values
  (define (produce n) (values (vector n (+ n 1)) (vector (+ n 2) (+ n 3))))
  (define result
    (call-with-values
      (lambda () (produce 1))
      (lambda (a b)
        (+ (vector-ref a 0) (vector-ref a 1) (vector-ref b 0) (vector-ref b 1))))))
)eta");
    BOOST_TEST(as_int(result) == 10);
}

/**
 * @test emitter_keeps_earlier_constants_alive
 *
 * While a function is being emitted its constants live only in the emitter's
 * Context, not the registry.  Building the second quoted list allocates and
 * must not sweep the first.
 */
BOOST_AUTO_TEST_CASE(emitter_keeps_earlier_constants_alive) {
    TortureHarness h;
    auto result = h.run_tortured(R"eta(
(module gc_rooting.two_quotes
  (define (two-lists) (cons '(1 2) '(3 4)))
  (define result (two-lists)))
)eta");

    auto& heap = h.driver.heap();
    const auto* pair = as_cons(heap, result);
    BOOST_REQUIRE_MESSAGE(pair != nullptr, "result is not a live pair");
    auto first = list_items(heap, pair->car);
    auto second = list_items(heap, pair->cdr);
    BOOST_REQUIRE_EQUAL(first.size(), 2u);
    BOOST_REQUIRE_EQUAL(second.size(), 2u);
    BOOST_CHECK_EQUAL(as_int(first[0]), 1);
    BOOST_CHECK_EQUAL(as_int(first[1]), 2);
    BOOST_CHECK_EQUAL(as_int(second[0]), 3);
    BOOST_CHECK_EQUAL(as_int(second[1]), 4);
}

/**
 * @test rest_args_list_is_not_aliased
 *
 * VM::pack_rest_args conses the rest list one cell at a time with the list
 * so far held only in a C++ local.  A swept cell is reused for the next one,
 * turning the rest list into a cycle.
 */
BOOST_AUTO_TEST_CASE(rest_args_list_is_not_aliased) {
    TortureHarness h;
    auto result = h.run_tortured(R"eta(
(module gc_rooting.rest_args
  (define (collect a . rest) rest)
  (define result (collect 0 1 2 3)))
)eta");

    auto items = list_items(h.driver.heap(), result);
    BOOST_REQUIRE_EQUAL(items.size(), 3u);
    BOOST_CHECK_EQUAL(as_int(items[0]), 1);
    BOOST_CHECK_EQUAL(as_int(items[1]), 2);
    BOOST_CHECK_EQUAL(as_int(items[2]), 3);
}

/**
 * @test call_value_keeps_caller_closure_alive
 *
 * When a builtin such as map calls back into Eta, VM::call_value saves the
 * running closure in a C++ local and clears current_closure_.  The caller's
 * closure must stay live so its upvalues can be read after the callback.
 */
BOOST_AUTO_TEST_CASE(call_value_keeps_caller_closure_alive) {
    TortureHarness h;
    auto result = h.run_tortured(R"eta(
(module gc_rooting.call_value
  (define (make-counter k)
    (lambda (xs)
      (let ((boxed (map (lambda (x) (vector x)) xs)))
        (+ k (vector-ref (car boxed) 0)))))
  (define result ((make-counter 100) '(5 6 7))))
)eta");
    BOOST_TEST(as_int(result) == 105);
}

/**
 * @test clp_get_domain_list_is_not_aliased
 *
 * %clp-get-domain builds (z lo hi) one cell at a time with each partial tail
 * held only in a C++ local.
 */
BOOST_AUTO_TEST_CASE(clp_get_domain_list_is_not_aliased) {
    TortureHarness h;
    auto result = h.run_tortured(R"eta(
(module gc_rooting.clp_domain
  (define v (logic-var))
  (%clp-domain-z! v 1 3)
  (define result (%clp-get-domain v)))
)eta");

    auto items = list_items(h.driver.heap(), result);
    BOOST_REQUIRE_EQUAL(items.size(), 3u);
    BOOST_CHECK(ops::is_boxed(items[0]) && ops::tag(items[0]) == Tag::Symbol);
    BOOST_CHECK_EQUAL(as_int(items[1]), 1);
    BOOST_CHECK_EQUAL(as_int(items[2]), 3);
}

/**
 * @test make_closure_keeps_upvalues_alive
 *
 * OpCode::MakeClosure pops its upvalues into a local std::vector before
 * make_closure allocates.  The captured vector is also held in a local slot
 * of the enclosing frame, so this is expected to pass today; it guards the
 * pattern if the emitter ever captures temporaries directly.
 */
BOOST_AUTO_TEST_CASE(make_closure_keeps_upvalues_alive) {
    TortureHarness h;
    auto result = h.run_tortured(R"eta(
(module gc_rooting.closure
  (define (make-getter n)
    (let ((v (vector n (+ n 1))))
      (lambda () v)))
  (define result ((make-getter 7))))
)eta");
    require_int_vector(h.driver.heap(), result, {7, 8}, "captured upvalue");
}

/**
 * @test copy_term_keeps_partial_copy_alive
 *
 * VM::copy_term holds the copied car in a C++ local while it walks, and
 * allocates for, the cdr.  The copied vectors must survive until the new
 * list is built.
 */
BOOST_AUTO_TEST_CASE(copy_term_keeps_partial_copy_alive) {
    TortureHarness h;
    auto result = h.run_tortured(R"eta(
(module gc_rooting.copy_term_list
  (define (build n) (cons (vector n (+ n 1)) (cons (vector (+ n 2) (+ n 3)) '())))
  (define result (copy-term (build 1))))
)eta");

    auto& heap = h.driver.heap();
    auto items = list_items(heap, result);
    BOOST_REQUIRE_EQUAL(items.size(), 2u);
    require_int_vector(heap, items[0], {1, 2}, "element 0");
    require_int_vector(heap, items[1], {3, 4}, "element 1");
}

/**
 * @test copy_term_keeps_fresh_vars_alive
 *
 * Fresh logic variables live only in copy_term's memo map until the copied
 * pair is allocated.  Binding the copy's car must be visible through its cdr.
 */
BOOST_AUTO_TEST_CASE(copy_term_keeps_fresh_vars_alive) {
    TortureHarness h;
    auto result = h.run_tortured(R"eta(
(module gc_rooting.copy_term_vars
  (define x (logic-var))
  (define c (copy-term (cons x x)))
  (unify (car c) 99)
  (define result (deref-lvar (cdr c))))
)eta");
    BOOST_TEST(as_int(result) == 99);
}

/**
 * @test json_read_nested_arrays_keep_children_alive
 *
 * json_to_lisp holds the first inner vector in a local std::vector while it
 * recurses into, and allocates, the second.
 */
BOOST_AUTO_TEST_CASE(json_read_nested_arrays_keep_children_alive) {
    BuiltinHarness h;
    auto parsed = h.read_json_tortured("[[1,2],[3,4]]");

    const auto* outer = as_vector(h.heap, parsed);
    BOOST_REQUIRE_MESSAGE(outer != nullptr, "outer value is not a live vector");
    BOOST_REQUIRE_EQUAL(outer->elements.size(), 2u);
    require_int_vector(h.heap, outer->elements[0], {1, 2}, "element 0");
    require_int_vector(h.heap, outer->elements[1], {3, 4}, "element 1");
}

/**
 * @test json_read_object_keeps_values_alive
 *
 * For objects, json_to_lisp collects key/value pairs in a local vector and
 * only builds the hash map at the end, so earlier values are unrooted while
 * later ones are decoded.
 */
BOOST_AUTO_TEST_CASE(json_read_object_keeps_values_alive) {
    BuiltinHarness h;
    auto parsed = h.read_json_tortured(R"({"a":[1,2],"b":[3,4]})");

    for (const auto& [key, expected] : std::vector<std::pair<std::string, std::vector<int64_t>>>{
             {"a", {1, 2}}, {"b", {3, 4}}}) {
        auto key_val = make_string(h.heap, h.intern_table, key);
        BOOST_REQUIRE(key_val.has_value());
        auto value = call_builtin(h.env, "hash-map-ref", {parsed, *key_val});
        BOOST_REQUIRE_MESSAGE(value.has_value(), "hash-map-ref failed for key " << key);
        require_int_vector(h.heap, *value, expected, key.c_str());
    }
}

BOOST_AUTO_TEST_SUITE_END()
