#include <boost/test/unit_test.hpp>

#include <cstdint>
#include <expected>
#include <string>
#include <string_view>
#include <vector>

#include <eta/interpreter/module_path.h>
#include <eta/runtime/builtin_env.h>
#include <eta/runtime/builtin_names.h>
#include <eta/runtime/core_primitives.h>
#include <eta/runtime/error.h>
#include <eta/runtime/factory.h>
#include <eta/runtime/memory/heap.h>
#include <eta/runtime/memory/intern_table.h>
#include <eta/runtime/memory/mark_sweep_gc.h>
#include <eta/runtime/nanbox.h>
#include <eta/runtime/numeric_value.h>
#include <eta/runtime/vm/vm.h>
#include <eta/session/driver.h>

using namespace eta::runtime;
using namespace eta::runtime::nanbox;
using namespace eta::runtime::memory::heap;
using namespace eta::runtime::memory::factory;
using namespace eta::runtime::memory::gc;
using namespace eta::runtime::memory::intern;

namespace {

std::expected<LispVal, error::RuntimeError> call_builtin(
    BuiltinEnvironment& env,
    const std::string& name,
    const std::vector<LispVal>& args) {
    auto idx = env.lookup(name);
    if (!idx) {
        return std::unexpected(error::RuntimeError{
            error::VMError{error::RuntimeErrorCode::InternalError, "missing builtin: " + name}});
    }
    return env.specs()[*idx].func(args);
}

void expect_vm_error(const std::expected<LispVal, error::RuntimeError>& result,
                     const error::RuntimeErrorCode expected_code) {
    BOOST_REQUIRE(!result.has_value());
    auto* vm_error = std::get_if<error::VMError>(&result.error());
    BOOST_REQUIRE(vm_error != nullptr);
    BOOST_TEST(static_cast<int>(vm_error->code) == static_cast<int>(expected_code));
}

std::int64_t decode_int(Heap& heap, const LispVal value) {
    auto n = classify_numeric(value, heap);
    BOOST_REQUIRE(n.is_valid());
    BOOST_REQUIRE(n.is_fixnum());
    return n.int_val;
}

struct VmCallHarness {
    Heap heap{1ull << 22};
    InternTable intern_table;
    vm::VM vm{heap, intern_table};
    BuiltinEnvironment env;
    std::vector<LispVal> globals;

    VmCallHarness() {
        register_core_primitives(env, heap, intern_table, &vm);
        auto installed = env.install(heap, globals, env.size());
        BOOST_REQUIRE(installed.has_value());
    }

    std::expected<LispVal, error::RuntimeError> call_checked(
        const std::string& name,
        std::vector<LispVal> args) {
        auto idx = env.lookup(name);
        BOOST_REQUIRE(idx.has_value());
        return vm.call_value(globals[*idx], std::move(args));
    }
};

} // namespace

BOOST_AUTO_TEST_SUITE(atom_tests)

BOOST_AUTO_TEST_CASE(registers_expected_atom_builtins_and_arities) {
    Heap heap(1ull << 22);
    InternTable intern_table;
    BuiltinEnvironment env;
    register_core_primitives(env, heap, intern_table, nullptr);

    struct BuiltinExpect {
        const char* name;
        std::uint32_t arity;
        bool has_rest;
    };

    const std::vector<BuiltinExpect> expected{
        {"%atom-new", 1, false},
        {"%atom?", 1, false},
        {"%atom-deref", 1, false},
        {"%atom-reset!", 2, false},
        {"%atom-compare-and-set!", 3, false},
        {"%atom-swap!", 2, true},
    };

    for (const auto& builtin : expected) {
        auto idx = env.lookup(builtin.name);
        BOOST_TEST_CONTEXT("builtin: " << builtin.name) {
            BOOST_REQUIRE(idx.has_value());
            BOOST_TEST(env.specs()[*idx].arity == builtin.arity);
            BOOST_TEST(env.specs()[*idx].has_rest == builtin.has_rest);
        }
    }

    BuiltinEnvironment names_env;
    register_builtin_names(names_env);
    for (const auto& builtin : expected) {
        BOOST_TEST(names_env.lookup(builtin.name).has_value());
    }
}

BOOST_AUTO_TEST_CASE(atom_new_predicate_deref_and_reset) {
    Heap heap(1ull << 22);
    InternTable intern_table;
    BuiltinEnvironment env;
    register_core_primitives(env, heap, intern_table, nullptr);

    const auto forty_one = *ops::encode<std::int64_t>(41);
    auto atom_value = call_builtin(env, "%atom-new", {forty_one});
    BOOST_REQUIRE(atom_value.has_value());

    auto is_atom = call_builtin(env, "%atom?", {*atom_value});
    auto is_atom_fixnum = call_builtin(env, "%atom?", {forty_one});
    BOOST_REQUIRE(is_atom.has_value());
    BOOST_REQUIRE(is_atom_fixnum.has_value());
    BOOST_TEST(*is_atom == True);
    BOOST_TEST(*is_atom_fixnum == False);

    auto deref_1 = call_builtin(env, "%atom-deref", {*atom_value});
    BOOST_REQUIRE(deref_1.has_value());
    BOOST_TEST(decode_int(heap, *deref_1) == 41);

    const auto ninety_nine = *ops::encode<std::int64_t>(99);
    auto reset = call_builtin(env, "%atom-reset!", {*atom_value, ninety_nine});
    BOOST_REQUIRE(reset.has_value());
    BOOST_TEST(*reset == ninety_nine);

    auto deref_2 = call_builtin(env, "%atom-deref", {*atom_value});
    BOOST_REQUIRE(deref_2.has_value());
    BOOST_TEST(decode_int(heap, *deref_2) == 99);

    expect_vm_error(call_builtin(env, "%atom-deref", {forty_one}), error::RuntimeErrorCode::TypeError);
    expect_vm_error(call_builtin(env, "%atom-reset!", {forty_one, ninety_nine}), error::RuntimeErrorCode::TypeError);
}

BOOST_AUTO_TEST_CASE(compare_and_set_uses_raw_lispval_equality) {
    Heap heap(1ull << 22);
    InternTable intern_table;
    BuiltinEnvironment env;
    register_core_primitives(env, heap, intern_table, nullptr);

    const auto one = *ops::encode<std::int64_t>(1);
    const auto two = *ops::encode<std::int64_t>(2);
    auto c1 = make_cons(heap, one, two);
    auto c2 = make_cons(heap, one, two);
    BOOST_REQUIRE(c1.has_value());
    BOOST_REQUIRE(c2.has_value());
    BOOST_TEST(*c1 != *c2);

    auto atom_value = call_builtin(env, "%atom-new", {*c1});
    BOOST_REQUIRE(atom_value.has_value());

    auto cas_fail = call_builtin(env, "%atom-compare-and-set!", {*atom_value, *c2, Nil});
    BOOST_REQUIRE(cas_fail.has_value());
    BOOST_TEST(*cas_fail == False);

    auto cas_ok = call_builtin(env, "%atom-compare-and-set!", {*atom_value, *c1, Nil});
    BOOST_REQUIRE(cas_ok.has_value());
    BOOST_TEST(*cas_ok == True);

    auto deref_after = call_builtin(env, "%atom-deref", {*atom_value});
    BOOST_REQUIRE(deref_after.has_value());
    BOOST_TEST(*deref_after == Nil);
}

BOOST_AUTO_TEST_CASE(atom_swap_primitive_path_and_extra_args_forwarding) {
    Heap heap(1ull << 22);
    InternTable intern_table;
    BuiltinEnvironment env;
    register_core_primitives(env, heap, intern_table, nullptr);

    auto sum_proc = make_primitive(
        heap,
        [&heap](std::span<const LispVal> args) -> std::expected<LispVal, error::RuntimeError> {
            std::int64_t total = 0;
            for (auto v : args) {
                auto n = classify_numeric(v, heap);
                if (!n.is_valid() || n.is_flonum()) {
                    return std::unexpected(error::RuntimeError{error::VMError{
                        error::RuntimeErrorCode::TypeError,
                        "%atom-swap!: callback expected integer arguments"}});
                }
                total += n.int_val;
            }
            return make_fixnum(heap, total);
        },
        1,
        true);
    BOOST_REQUIRE(sum_proc.has_value());

    auto atom_value = call_builtin(env, "%atom-new", {*ops::encode<std::int64_t>(10)});
    BOOST_REQUIRE(atom_value.has_value());

    auto swapped = call_builtin(env, "%atom-swap!", {
        *atom_value,
        *sum_proc,
        *ops::encode<std::int64_t>(1),
        *ops::encode<std::int64_t>(2)});
    BOOST_REQUIRE(swapped.has_value());
    BOOST_TEST(decode_int(heap, *swapped) == 13);

    auto deref_after = call_builtin(env, "%atom-deref", {*atom_value});
    BOOST_REQUIRE(deref_after.has_value());
    BOOST_TEST(decode_int(heap, *deref_after) == 13);

    auto closure = make_closure(heap, nullptr, {});
    BOOST_REQUIRE(closure.has_value());
    auto no_vm_for_closure = call_builtin(env, "%atom-swap!", {*atom_value, *closure});
    expect_vm_error(no_vm_for_closure, error::RuntimeErrorCode::TypeError);
}

BOOST_AUTO_TEST_CASE(atom_swap_vm_path_supports_closure_callback) {
    eta::session::Driver driver(eta::interpreter::ModulePathResolver{});
    LispVal result = Nil;
    const bool ok = driver.run_source(R"eta(
(module atom.tests.swap-closure
  (define result
    (let ((a (%atom-new 5)))
      (let ((updated (%atom-swap! a (lambda (old inc) (+ old inc)) 7)))
        (if (= updated (%atom-deref a)) updated -1)))))
)eta", &result, "result");
    BOOST_REQUIRE_MESSAGE(ok, "driver.run_source failed for atom closure swap test");

    auto decoded = ops::decode<std::int64_t>(result);
    BOOST_REQUIRE(decoded.has_value());
    BOOST_TEST(*decoded == 12);
}

BOOST_AUTO_TEST_CASE(atom_cell_value_is_marked_by_gc) {
    Heap heap(1ull << 22);
    InternTable intern_table;
    BuiltinEnvironment env;
    register_core_primitives(env, heap, intern_table, nullptr);

    auto kept = make_vector(heap, {*ops::encode<std::int64_t>(7)});
    auto garbage = make_vector(heap, {*ops::encode<std::int64_t>(99)});
    BOOST_REQUIRE(kept.has_value());
    BOOST_REQUIRE(garbage.has_value());

    const auto kept_id = static_cast<ObjectId>(ops::payload(*kept));
    const auto garbage_id = static_cast<ObjectId>(ops::payload(*garbage));

    auto atom_value = call_builtin(env, "%atom-new", {*kept});
    BOOST_REQUIRE(atom_value.has_value());

    MarkSweepGC gc;
    std::vector<LispVal> roots{*atom_value};
    GCStats stats{};
    gc.collect(heap, roots.begin(), roots.end(), &stats);

    HeapEntry entry{};
    BOOST_TEST(heap.try_get(kept_id, entry));
    BOOST_TEST(!heap.try_get(garbage_id, entry));

    auto deref_after = call_builtin(env, "%atom-deref", {*atom_value});
    BOOST_REQUIRE(deref_after.has_value());
    BOOST_TEST(*deref_after == *kept);
}

BOOST_AUTO_TEST_CASE(atom_builtins_report_arity_errors_via_vm_dispatch) {
    VmCallHarness harness;

    const auto one = *ops::encode<std::int64_t>(1);

    expect_vm_error(harness.call_checked("%atom-new", {}), error::RuntimeErrorCode::InvalidArity);
    expect_vm_error(harness.call_checked("%atom-reset!", {one}), error::RuntimeErrorCode::InvalidArity);
    expect_vm_error(harness.call_checked("%atom-compare-and-set!", {one, one}),
                    error::RuntimeErrorCode::InvalidArity);
    expect_vm_error(harness.call_checked("%atom-swap!", {one}), error::RuntimeErrorCode::InvalidArity);
}

BOOST_AUTO_TEST_SUITE_END()

