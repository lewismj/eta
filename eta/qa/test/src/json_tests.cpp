#include <boost/test/unit_test.hpp>

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <eta/runtime/builtin_env.h>
#include <eta/runtime/core_primitives.h>
#include <eta/runtime/error.h>
#include <eta/runtime/factory.h>
#include <eta/runtime/memory/heap.h>
#include <eta/runtime/memory/intern_table.h>
#include <eta/runtime/nanbox.h>
#include <eta/runtime/numeric_value.h>
#include <eta/runtime/port.h>
#include <eta/runtime/string_view.h>
#include <eta/runtime/types/types.h>
#include <eta/util/json.h>

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

LispVal stringv(Heap& heap, InternTable& intern_table, std::string_view text) {
    auto str = make_string(heap, intern_table, std::string(text));
    BOOST_REQUIRE(str.has_value());
    return *str;
}

LispVal symbolv(InternTable& intern_table, std::string_view text) {
    auto sym = make_symbol(intern_table, std::string(text));
    BOOST_REQUIRE(sym.has_value());
    return *sym;
}

std::string decode_string(InternTable& intern_table, LispVal value) {
    auto sv = StringView::try_from(value, intern_table);
    BOOST_REQUIRE(sv.has_value());
    return std::string(sv->view());
}

std::expected<LispVal, RuntimeError> hash_map_ref(
    BuiltinEnvironment& env,
    LispVal map,
    LispVal key) {
    return call_builtin(env, "hash-map-ref", {map, key});
}

} // namespace

BOOST_AUTO_TEST_SUITE(json_tests)

BOOST_AUTO_TEST_CASE(registers_expected_json_builtins) {
    Heap heap(1ull << 22);
    InternTable intern_table;
    BuiltinEnvironment env;
    register_core_primitives(env, heap, intern_table, nullptr);

    BOOST_TEST(env.lookup("%json-read").has_value());
    BOOST_TEST(env.lookup("%json-read-string").has_value());
    BOOST_TEST(env.lookup("%json-write").has_value());
    BOOST_TEST(env.lookup("%json-write-string").has_value());
}

BOOST_AUTO_TEST_CASE(read_string_decodes_hash_maps_vectors_and_flonums_by_default) {
    Heap heap(1ull << 22);
    InternTable intern_table;
    BuiltinEnvironment env;
    register_core_primitives(env, heap, intern_table, nullptr);

    auto parsed = call_builtin(env, "%json-read-string", {
        stringv(heap, intern_table, R"({"a":1,"b":[2,3.5],"c":true,"d":null})"),
        False
    });
    BOOST_REQUIRE(parsed.has_value());

    auto is_map = call_builtin(env, "hash-map?", {*parsed});
    BOOST_REQUIRE(is_map.has_value());
    BOOST_TEST(*is_map == True);

    auto a = hash_map_ref(env, *parsed, stringv(heap, intern_table, "a"));
    auto b = hash_map_ref(env, *parsed, stringv(heap, intern_table, "b"));
    auto c = hash_map_ref(env, *parsed, stringv(heap, intern_table, "c"));
    auto d = hash_map_ref(env, *parsed, stringv(heap, intern_table, "d"));
    BOOST_REQUIRE(a.has_value());
    BOOST_REQUIRE(b.has_value());
    BOOST_REQUIRE(c.has_value());
    BOOST_REQUIRE(d.has_value());

    auto an = classify_numeric(*a, heap);
    BOOST_TEST(an.is_flonum());
    BOOST_TEST(an.float_val == 1.0, boost::test_tools::tolerance(1e-12));
    BOOST_TEST(*c == True);
    BOOST_TEST(*d == Nil);

    BOOST_REQUIRE(ops::is_boxed(*b));
    BOOST_REQUIRE(ops::tag(*b) == Tag::HeapObject);
    auto* vec = heap.try_get_as<ObjectKind::Vector, types::Vector>(ops::payload(*b));
    BOOST_REQUIRE(vec != nullptr);
    BOOST_REQUIRE_EQUAL(vec->elements.size(), 2u);

    auto b0 = classify_numeric(vec->elements[0], heap);
    auto b1 = classify_numeric(vec->elements[1], heap);
    BOOST_TEST(b0.is_flonum());
    BOOST_TEST(b0.float_val == 2.0, boost::test_tools::tolerance(1e-12));
    BOOST_TEST(b1.is_flonum());
    BOOST_TEST(b1.float_val == 3.5, boost::test_tools::tolerance(1e-12));
}

BOOST_AUTO_TEST_CASE(read_string_option_keeps_integers_exact) {
    Heap heap(1ull << 22);
    InternTable intern_table;
    BuiltinEnvironment env;
    register_core_primitives(env, heap, intern_table, nullptr);

    auto parsed = call_builtin(env, "%json-read-string", {
        stringv(heap, intern_table, R"({"x":1,"arr":[2,3.25]})"),
        True
    });
    BOOST_REQUIRE(parsed.has_value());

    auto x = hash_map_ref(env, *parsed, stringv(heap, intern_table, "x"));
    auto arr = hash_map_ref(env, *parsed, stringv(heap, intern_table, "arr"));
    BOOST_REQUIRE(x.has_value());
    BOOST_REQUIRE(arr.has_value());

    auto xn = classify_numeric(*x, heap);
    BOOST_TEST(xn.is_fixnum());
    BOOST_TEST(xn.int_val == 1);

    BOOST_REQUIRE(ops::is_boxed(*arr));
    BOOST_REQUIRE(ops::tag(*arr) == Tag::HeapObject);
    auto* vec = heap.try_get_as<ObjectKind::Vector, types::Vector>(ops::payload(*arr));
    BOOST_REQUIRE(vec != nullptr);
    BOOST_REQUIRE_EQUAL(vec->elements.size(), 2u);

    auto a0 = classify_numeric(vec->elements[0], heap);
    auto a1 = classify_numeric(vec->elements[1], heap);
    BOOST_TEST(a0.is_fixnum());
    BOOST_TEST(a0.int_val == 2);
    BOOST_TEST(a1.is_flonum());
    BOOST_TEST(a1.float_val == 3.25, boost::test_tools::tolerance(1e-12));
}

BOOST_AUTO_TEST_CASE(write_and_read_round_trip_through_ports) {
    Heap heap(1ull << 22);
    InternTable intern_table;
    BuiltinEnvironment env;
    register_core_primitives(env, heap, intern_table, nullptr);

    auto input_backend = std::make_shared<StringPort>(
        StringPort::Mode::Input,
        R"({"k":"v","n":7})");
    auto input_port = make_port(heap, input_backend);
    BOOST_REQUIRE(input_port.has_value());

    auto parsed = call_builtin(env, "%json-read", {*input_port, True});
    BOOST_REQUIRE(parsed.has_value());

    auto output_backend = std::make_shared<StringPort>(StringPort::Mode::Output);
    auto output_port = make_port(heap, output_backend);
    BOOST_REQUIRE(output_port.has_value());

    auto wrote = call_builtin(env, "%json-write", {*parsed, *output_port});
    BOOST_REQUIRE(wrote.has_value());
    BOOST_TEST(*wrote == Nil);

    auto reparsed = eta::json::parse(output_backend->get_string());
    BOOST_TEST(reparsed.is_object());
    BOOST_TEST(reparsed["k"].is_string());
    BOOST_TEST(reparsed["k"].as_string() == "v");
    BOOST_TEST(reparsed["n"].is_int());
    BOOST_TEST(reparsed["n"].as_int() == 7);
}

BOOST_AUTO_TEST_CASE(write_string_accepts_symbol_keys) {
    Heap heap(1ull << 22);
    InternTable intern_table;
    BuiltinEnvironment env;
    register_core_primitives(env, heap, intern_table, nullptr);

    auto map = call_builtin(env, "hash-map", {
        symbolv(intern_table, "answer"), *ops::encode<int64_t>(42),
        stringv(heap, intern_table, "name"), stringv(heap, intern_table, "eta")
    });
    BOOST_REQUIRE(map.has_value());

    auto out = call_builtin(env, "%json-write-string", {*map});
    BOOST_REQUIRE(out.has_value());
    const std::string text = decode_string(intern_table, *out);

    auto parsed = eta::json::parse(text);
    BOOST_TEST(parsed.is_object());
    BOOST_TEST(parsed["answer"].is_int());
    BOOST_TEST(parsed["answer"].as_int() == 42);
    BOOST_TEST(parsed["name"].is_string());
    BOOST_TEST(parsed["name"].as_string() == "eta");
}

BOOST_AUTO_TEST_CASE(malformed_json_reports_tagged_user_error) {
    Heap heap(1ull << 22);
    InternTable intern_table;
    BuiltinEnvironment env;
    register_core_primitives(env, heap, intern_table, nullptr);

    auto bad = call_builtin(env, "%json-read-string", {
        stringv(heap, intern_table, "{"),
        False
    });
    BOOST_REQUIRE(!bad.has_value());

    auto* vm_error = std::get_if<VMError>(&bad.error());
    BOOST_REQUIRE(vm_error != nullptr);
    BOOST_TEST(static_cast<int>(vm_error->code) ==
               static_cast<int>(RuntimeErrorCode::UserError));
    BOOST_TEST(vm_error->tag_override == "runtime.json-error");
}

BOOST_AUTO_TEST_SUITE_END()

