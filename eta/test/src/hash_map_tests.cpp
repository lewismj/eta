#include <boost/test/unit_test.hpp>

#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <vector>

#include <eta/runtime/builtin_env.h>
#include <eta/runtime/core_primitives.h>
#include <eta/runtime/error.h>
#include <eta/runtime/factory.h>
#include <eta/runtime/memory/heap.h>
#include <eta/runtime/memory/intern_table.h>
#include <eta/runtime/nanbox.h>
#include <eta/runtime/numeric_value.h>

using namespace eta::runtime;
using namespace eta::runtime::nanbox;
using namespace eta::runtime::memory::heap;
using namespace eta::runtime::memory::intern;
using namespace eta::runtime::memory::factory;
using namespace eta::runtime::error;

namespace {

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

LispVal symbol(InternTable& intern_table, std::string_view text) {
    auto sym = make_symbol(intern_table, std::string(text));
    BOOST_REQUIRE(sym.has_value());
    return *sym;
}

LispVal stringv(Heap& heap, InternTable& intern_table, std::string_view text) {
    auto str = make_string(heap, intern_table, std::string(text));
    BOOST_REQUIRE(str.has_value());
    return *str;
}

LispVal list_from_values(Heap& heap, const std::vector<LispVal>& values) {
    LispVal out = Nil;
    for (auto it = values.rbegin(); it != values.rend(); ++it) {
        auto cell = make_cons(heap, *it, out);
        BOOST_REQUIRE(cell.has_value());
        out = *cell;
    }
    return out;
}

std::size_t list_length(Heap& heap, LispVal list) {
    std::size_t n = 0;
    LispVal cur = list;
    while (cur != Nil) {
        BOOST_REQUIRE(ops::is_boxed(cur));
        BOOST_REQUIRE(ops::tag(cur) == Tag::HeapObject);
        auto* c = heap.try_get_as<ObjectKind::Cons, types::Cons>(ops::payload(cur));
        BOOST_REQUIRE(c != nullptr);
        ++n;
        cur = c->cdr;
    }
    return n;
}

std::int64_t decode_int(Heap& heap, LispVal value) {
    auto n = classify_numeric(value, heap);
    BOOST_REQUIRE(n.is_valid());
    BOOST_REQUIRE(n.is_fixnum());
    return n.int_val;
}

} // namespace

BOOST_AUTO_TEST_SUITE(hash_map_tests)

BOOST_AUTO_TEST_CASE(registers_expected_hash_builtins) {
    Heap heap(1ull << 22);
    InternTable intern_table;
    BuiltinEnvironment env;
    register_core_primitives(env, heap, intern_table, nullptr);

    BOOST_TEST(env.lookup("hash-map").has_value());
    BOOST_TEST(env.lookup("make-hash-map").has_value());
    BOOST_TEST(env.lookup("hash-map?").has_value());
    BOOST_TEST(env.lookup("hash-map-ref").has_value());
    BOOST_TEST(env.lookup("hash-map-assoc").has_value());
    BOOST_TEST(env.lookup("hash-map-dissoc").has_value());
    BOOST_TEST(env.lookup("hash-map-keys").has_value());
    BOOST_TEST(env.lookup("hash-map-values").has_value());
    BOOST_TEST(env.lookup("hash-map-size").has_value());
    BOOST_TEST(env.lookup("hash-map->list").has_value());
    BOOST_TEST(env.lookup("list->hash-map").has_value());
    BOOST_TEST(env.lookup("hash-map-fold").has_value());
    BOOST_TEST(env.lookup("hash").has_value());

    BOOST_TEST(env.lookup("make-hash-set").has_value());
    BOOST_TEST(env.lookup("hash-set").has_value());
    BOOST_TEST(env.lookup("hash-set?").has_value());
    BOOST_TEST(env.lookup("hash-set-add").has_value());
    BOOST_TEST(env.lookup("hash-set-remove").has_value());
    BOOST_TEST(env.lookup("hash-set-contains?").has_value());
    BOOST_TEST(env.lookup("hash-set-union").has_value());
    BOOST_TEST(env.lookup("hash-set-intersect").has_value());
    BOOST_TEST(env.lookup("hash-set-diff").has_value());
    BOOST_TEST(env.lookup("hash-set->list").has_value());
    BOOST_TEST(env.lookup("list->hash-set").has_value());
}

BOOST_AUTO_TEST_CASE(hash_map_core_lookup_update_and_missing_default) {
    Heap heap(1ull << 22);
    InternTable intern_table;
    BuiltinEnvironment env;
    register_core_primitives(env, heap, intern_table, nullptr);

    auto m = call_builtin(env, "hash-map", {
        symbol(intern_table, "a"), *ops::encode<int64_t>(10),
        stringv(heap, intern_table, "b"), *ops::encode<int64_t>(20)
    });
    BOOST_REQUIRE(m.has_value());

    auto size = call_builtin(env, "hash-map-size", {*m});
    BOOST_REQUIRE(size.has_value());
    BOOST_TEST(decode_int(heap, *size) == 2);

    auto av = call_builtin(env, "hash-map-ref", {*m, symbol(intern_table, "a")});
    BOOST_REQUIRE(av.has_value());
    BOOST_TEST(decode_int(heap, *av) == 10);

    auto miss_with_default = call_builtin(env, "hash-map-ref", {
        *m, symbol(intern_table, "missing"), *ops::encode<int64_t>(99)});
    BOOST_REQUIRE(miss_with_default.has_value());
    BOOST_TEST(decode_int(heap, *miss_with_default) == 99);

    auto miss_without_default = call_builtin(env, "hash-map-ref", {*m, symbol(intern_table, "missing")});
    BOOST_REQUIRE(!miss_without_default.has_value());
    auto* vm_error = std::get_if<VMError>(&miss_without_default.error());
    BOOST_REQUIRE(vm_error != nullptr);
    BOOST_TEST(static_cast<int>(vm_error->code) == static_cast<int>(RuntimeErrorCode::UserError));
}

BOOST_AUTO_TEST_CASE(hash_map_distinguishes_fixnum_and_flonum_keys) {
    Heap heap(1ull << 22);
    InternTable intern_table;
    BuiltinEnvironment env;
    register_core_primitives(env, heap, intern_table, nullptr);

    auto empty = call_builtin(env, "make-hash-map", {});
    BOOST_REQUIRE(empty.has_value());

    auto m1 = call_builtin(env, "hash-map-assoc", {*empty, *ops::encode<int64_t>(1), symbol(intern_table, "int")});
    BOOST_REQUIRE(m1.has_value());
    auto m2 = call_builtin(env, "hash-map-assoc", {*m1, *ops::encode<double>(1.0), symbol(intern_table, "float")});
    BOOST_REQUIRE(m2.has_value());

    auto size = call_builtin(env, "hash-map-size", {*m2});
    BOOST_REQUIRE(size.has_value());
    BOOST_TEST(decode_int(heap, *size) == 2);

    auto int_ref = call_builtin(env, "hash-map-ref", {*m2, *ops::encode<int64_t>(1)});
    auto float_ref = call_builtin(env, "hash-map-ref", {*m2, *ops::encode<double>(1.0)});
    BOOST_REQUIRE(int_ref.has_value());
    BOOST_REQUIRE(float_ref.has_value());
    BOOST_TEST(*int_ref != *float_ref);
}

BOOST_AUTO_TEST_CASE(hash_set_algebra_and_membership) {
    Heap heap(1ull << 22);
    InternTable intern_table;
    BuiltinEnvironment env;
    register_core_primitives(env, heap, intern_table, nullptr);

    auto a = call_builtin(env, "hash-set", {
        *ops::encode<int64_t>(1), *ops::encode<int64_t>(2), *ops::encode<int64_t>(3)});
    auto b = call_builtin(env, "hash-set", {
        *ops::encode<int64_t>(3), *ops::encode<int64_t>(4)});
    BOOST_REQUIRE(a.has_value());
    BOOST_REQUIRE(b.has_value());

    auto union_set = call_builtin(env, "hash-set-union", {*a, *b});
    BOOST_REQUIRE(union_set.has_value());
    auto union_list = call_builtin(env, "hash-set->list", {*union_set});
    BOOST_REQUIRE(union_list.has_value());
    BOOST_TEST(list_length(heap, *union_list) == 4u);

    auto contains_four = call_builtin(env, "hash-set-contains?", {*union_set, *ops::encode<int64_t>(4)});
    BOOST_REQUIRE(contains_four.has_value());
    BOOST_TEST(*contains_four == True);

    auto inter_set = call_builtin(env, "hash-set-intersect", {*a, *b});
    BOOST_REQUIRE(inter_set.has_value());
    auto inter_list = call_builtin(env, "hash-set->list", {*inter_set});
    BOOST_REQUIRE(inter_list.has_value());
    BOOST_TEST(list_length(heap, *inter_list) == 1u);

    auto diff_set = call_builtin(env, "hash-set-diff", {*a, *b});
    BOOST_REQUIRE(diff_set.has_value());
    auto diff_has_one = call_builtin(env, "hash-set-contains?", {*diff_set, *ops::encode<int64_t>(1)});
    auto diff_has_three = call_builtin(env, "hash-set-contains?", {*diff_set, *ops::encode<int64_t>(3)});
    BOOST_REQUIRE(diff_has_one.has_value());
    BOOST_REQUIRE(diff_has_three.has_value());
    BOOST_TEST(*diff_has_one == True);
    BOOST_TEST(*diff_has_three == False);
}

BOOST_AUTO_TEST_CASE(list_roundtrip_and_hash_builtin_error_path) {
    Heap heap(1ull << 22);
    InternTable intern_table;
    BuiltinEnvironment env;
    register_core_primitives(env, heap, intern_table, nullptr);

    auto k1 = symbol(intern_table, "x");
    auto v1 = *ops::encode<int64_t>(11);
    auto k2 = symbol(intern_table, "y");
    auto v2 = *ops::encode<int64_t>(22);

    auto p1 = make_cons(heap, k1, v1);
    auto p2 = make_cons(heap, k2, v2);
    BOOST_REQUIRE(p1.has_value());
    BOOST_REQUIRE(p2.has_value());

    auto alist = list_from_values(heap, {*p1, *p2});
    auto map_from_alist = call_builtin(env, "list->hash-map", {alist});
    BOOST_REQUIRE(map_from_alist.has_value());

    auto as_list = call_builtin(env, "hash-map->list", {*map_from_alist});
    BOOST_REQUIRE(as_list.has_value());
    auto map_roundtrip = call_builtin(env, "list->hash-map", {*as_list});
    BOOST_REQUIRE(map_roundtrip.has_value());

    auto eq = call_builtin(env, "equal?", {*map_from_alist, *map_roundtrip});
    BOOST_REQUIRE(eq.has_value());
    BOOST_TEST(*eq == True);

    auto prim = make_primitive(
        heap,
        [](std::span<const LispVal>) -> std::expected<LispVal, RuntimeError> { return Nil; },
        0, false);
    BOOST_REQUIRE(prim.has_value());

    auto hash_bad = call_builtin(env, "hash", {*prim});
    BOOST_REQUIRE(!hash_bad.has_value());
    auto* vm_error = std::get_if<VMError>(&hash_bad.error());
    BOOST_REQUIRE(vm_error != nullptr);
    BOOST_TEST(static_cast<int>(vm_error->code) == static_cast<int>(RuntimeErrorCode::TypeError));
}

BOOST_AUTO_TEST_SUITE_END()
