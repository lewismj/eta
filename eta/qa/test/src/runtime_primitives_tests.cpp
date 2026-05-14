/**
 * @file runtime_primitives_tests.cpp
 * @brief Unit tests for runtime primitive bootstrap helpers.
 */

#include <boost/test/unit_test.hpp>

#include <array>
#include <cmath>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "eta/runtime/aad_unary_helpers.h"
#include "eta/runtime/ad_error.h"
#include "eta/runtime/builtin_env.h"
#include "eta/runtime/core_primitives.h"
#include "eta/runtime/error.h"
#include "eta/runtime/extension_env.h"
#include "eta/runtime/factory.h"
#include "eta/runtime/memory/heap.h"
#include "eta/runtime/memory/intern_table.h"
#include "eta/runtime/nanbox.h"
#include "eta/runtime/numeric_value.h"
#include "eta/runtime/string_view.h"
#include "eta/runtime/types/tape.h"
#include "eta/runtime/types/tape_ref.h"
#include "eta/runtime/types/primitive.h"
#include "eta/runtime/vm/vm.h"
#include "eta/session/runtime_primitives.h"

namespace {

eta::runtime::types::PrimitiveFunc make_const_primitive(std::int64_t value) {
    return [value](eta::runtime::types::PrimitiveArgs) {
        return eta::runtime::nanbox::ops::encode(value);
    };
}

std::expected<eta::runtime::nanbox::LispVal, eta::runtime::error::RuntimeError> invoke_builtin(
    const eta::runtime::BuiltinEnvironment& builtins,
    std::string_view name,
    std::span<const eta::runtime::nanbox::LispVal> args) {
    const auto index = builtins.lookup(name);
    if (!index.has_value()) {
        return std::unexpected(eta::runtime::error::RuntimeError{
            eta::runtime::error::VMError{
                eta::runtime::error::RuntimeErrorCode::InternalError,
                std::string("missing builtin: ") + std::string(name)}});
    }
    return builtins.specs()[*index].func(args);
}

const eta::runtime::error::VMErrorField* find_vm_error_field(
    const eta::runtime::error::VMError& error,
    std::string_view key) {
    for (const auto& field : error.fields) {
        if (field.key == key) {
            return &field;
        }
    }
    return nullptr;
}

} // namespace

BOOST_AUTO_TEST_SUITE(runtime_primitives_tests)

BOOST_AUTO_TEST_CASE(extension_environment_fingerprint_is_stable_for_equal_surfaces) {
    eta::runtime::ExtensionEnvironment first;
    eta::runtime::ExtensionEnvironment same_surface;
    eta::runtime::ExtensionEnvironment changed_arity;
    eta::runtime::ExtensionEnvironment reordered;

    first.register_extension("ext.alpha", 1u, false, make_const_primitive(1));
    first.register_extension("ext.beta", 2u, true, make_const_primitive(2));

    same_surface.register_extension("ext.alpha", 1u, false, make_const_primitive(42));
    same_surface.register_extension("ext.beta", 2u, true, make_const_primitive(99));

    changed_arity.register_extension("ext.alpha", 2u, false, make_const_primitive(1));
    changed_arity.register_extension("ext.beta", 2u, true, make_const_primitive(2));

    reordered.register_extension("ext.beta", 2u, true, make_const_primitive(2));
    reordered.register_extension("ext.alpha", 1u, false, make_const_primitive(1));

    BOOST_TEST(eta::runtime::ExtensionEnvironment{}.fingerprint() == 0u);
    BOOST_TEST(first.fingerprint() == same_surface.fingerprint());
    BOOST_TEST(first.fingerprint() != changed_arity.fingerprint());
    BOOST_TEST(first.fingerprint() != reordered.fingerprint());
}

BOOST_AUTO_TEST_CASE(runtime_primitive_installer_populates_slots_and_names) {
    eta::runtime::memory::heap::Heap heap(1ull << 20);
    eta::runtime::BuiltinEnvironment builtins;
    eta::runtime::ExtensionEnvironment extensions;

    builtins.register_builtin("core.one", 0u, false, make_const_primitive(1));
    builtins.register_builtin("core.two", 1u, false, make_const_primitive(2));
    extensions.register_extension("ext.three", 0u, false, make_const_primitive(3));

    eta::session::RuntimePrimitiveInstaller installer(heap, builtins, extensions);
    BOOST_TEST(!installer.installed());
    BOOST_TEST(installer.builtin_count() == 2u);
    BOOST_TEST(installer.total_primitive_count() == 3u);

    std::vector<eta::runtime::nanbox::LispVal> globals;
    const auto install_result = installer.install_into(globals, 6u);
    BOOST_REQUIRE(install_result.has_value());
    BOOST_TEST(installer.installed());
    BOOST_TEST(globals.size() == 6u);

    const auto assert_primitive_debug_name =
        [&](std::size_t slot, const std::string& expected_name) {
            using eta::runtime::memory::heap::ObjectKind;
            using eta::runtime::nanbox::ops::payload;
            auto* primitive = heap.try_get_as<ObjectKind::Primitive, eta::runtime::types::Primitive>(
                payload(globals[slot]));
            BOOST_REQUIRE(primitive != nullptr);
            BOOST_TEST(primitive->debug_name == expected_name);
        };

    assert_primitive_debug_name(0u, "core.one");
    assert_primitive_debug_name(1u, "core.two");
    assert_primitive_debug_name(2u, "ext.three");

    std::unordered_map<uint32_t, std::string> global_names;
    installer.record_names(global_names);
    BOOST_TEST(global_names[0u] == "core.one");
    BOOST_TEST(global_names[1u] == "core.two");
    BOOST_TEST(global_names[2u] == "ext.three");

    installer.invalidate();
    BOOST_TEST(!installer.installed());
}

BOOST_AUTO_TEST_CASE(runtime_primitive_installer_does_not_reset_existing_non_primitive_globals) {
    eta::runtime::memory::heap::Heap heap(1ull << 20);
    eta::runtime::BuiltinEnvironment builtins;
    eta::runtime::ExtensionEnvironment extensions;

    builtins.register_builtin("core.only", 0u, false, make_const_primitive(1));
    eta::session::RuntimePrimitiveInstaller installer(heap, builtins, extensions);

    std::vector<eta::runtime::nanbox::LispVal> globals(8u, eta::runtime::nanbox::Nil);
    globals[6] = eta::runtime::nanbox::ops::encode(std::int64_t{77}).value();

    const auto install_result = installer.install_into(globals, 1u);
    BOOST_REQUIRE(install_result.has_value());
    BOOST_TEST(globals.size() == 8u);

    const auto preserved = eta::runtime::nanbox::ops::decode<std::int64_t>(globals[6]);
    BOOST_REQUIRE(preserved.has_value());
    BOOST_TEST(*preserved == 77);
}

BOOST_AUTO_TEST_CASE(arithmetic_primitives_preserve_numeric_and_predicate_behavior) {
    using eta::runtime::nanbox::LispVal;

    eta::runtime::memory::heap::Heap heap(1ull << 20);
    eta::runtime::memory::intern::InternTable intern_table;
    eta::runtime::vm::VM vm(heap, intern_table);

    eta::runtime::BuiltinEnvironment builtins;
    eta::runtime::register_core_primitives(builtins, heap, intern_table, &vm);

    const auto two = eta::runtime::nanbox::ops::encode(std::int64_t{2});
    const auto nine = eta::runtime::nanbox::ops::encode(std::int64_t{9});
    const auto neg_five = eta::runtime::nanbox::ops::encode(std::int64_t{-5});
    const auto three = eta::runtime::nanbox::ops::encode(std::int64_t{3});
    BOOST_REQUIRE(two.has_value());
    BOOST_REQUIRE(nine.has_value());
    BOOST_REQUIRE(neg_five.has_value());
    BOOST_REQUIRE(three.has_value());

    auto three_point_five = eta::runtime::make_flonum(3.5);
    BOOST_REQUIRE(three_point_five.has_value());

    auto add_res = invoke_builtin(
        builtins,
        "+",
        std::array<LispVal, 2>{*two, *three_point_five});
    BOOST_REQUIRE(add_res.has_value());
    auto add_numeric = eta::runtime::classify_numeric(*add_res, heap);
    BOOST_REQUIRE(add_numeric.is_valid());
    BOOST_TEST(add_numeric.as_double() == 5.5);

    auto div_res =
        invoke_builtin(builtins, "/", std::array<LispVal, 2>{*nine, *two});
    BOOST_REQUIRE(div_res.has_value());
    auto div_numeric = eta::runtime::classify_numeric(*div_res, heap);
    BOOST_REQUIRE(div_numeric.is_valid());
    BOOST_TEST(div_numeric.as_double() == 4.5);

    auto modulo_res = invoke_builtin(
        builtins,
        "modulo",
        std::array<LispVal, 2>{*neg_five, *three});
    BOOST_REQUIRE(modulo_res.has_value());
    auto modulo_value =
        eta::runtime::nanbox::ops::decode<std::int64_t>(*modulo_res);
    BOOST_REQUIRE(modulo_value.has_value());
    BOOST_TEST(*modulo_value == 1);

    auto remainder_res = invoke_builtin(
        builtins,
        "remainder",
        std::array<LispVal, 2>{*neg_five, *three});
    BOOST_REQUIRE(remainder_res.has_value());
    auto remainder_value =
        eta::runtime::nanbox::ops::decode<std::int64_t>(*remainder_res);
    BOOST_REQUIRE(remainder_value.has_value());
    BOOST_TEST(*remainder_value == -2);

    auto number_true =
        invoke_builtin(builtins, "number?", std::array<LispVal, 1>{*two});
    BOOST_REQUIRE(number_true.has_value());
    BOOST_TEST(*number_true == eta::runtime::nanbox::True);

    auto number_false = invoke_builtin(
        builtins,
        "number?",
        std::array<LispVal, 1>{eta::runtime::nanbox::True});
    BOOST_REQUIRE(number_false.has_value());
    BOOST_TEST(*number_false == eta::runtime::nanbox::False);

    auto four_exact = eta::runtime::make_flonum(4.0);
    auto four_half = eta::runtime::make_flonum(4.5);
    BOOST_REQUIRE(four_exact.has_value());
    BOOST_REQUIRE(four_half.has_value());

    auto integer_true =
        invoke_builtin(builtins, "integer?", std::array<LispVal, 1>{*four_exact});
    BOOST_REQUIRE(integer_true.has_value());
    BOOST_TEST(*integer_true == eta::runtime::nanbox::True);

    auto integer_false =
        invoke_builtin(builtins, "integer?", std::array<LispVal, 1>{*four_half});
    BOOST_REQUIRE(integer_false.has_value());
    BOOST_TEST(*integer_false == eta::runtime::nanbox::False);
}

BOOST_AUTO_TEST_CASE(
    sequence_collection_primitives_preserve_list_map_vector_hash_and_equal_behavior) {
    using eta::runtime::nanbox::LispVal;

    eta::runtime::memory::heap::Heap heap(1ull << 20);
    eta::runtime::memory::intern::InternTable intern_table;
    eta::runtime::vm::VM vm(heap, intern_table);

    eta::runtime::BuiltinEnvironment builtins;
    eta::runtime::register_core_primitives(builtins, heap, intern_table, &vm);

    const auto one = eta::runtime::nanbox::ops::encode(std::int64_t{1});
    const auto two = eta::runtime::nanbox::ops::encode(std::int64_t{2});
    const auto three = eta::runtime::nanbox::ops::encode(std::int64_t{3});
    const auto zero = eta::runtime::nanbox::ops::encode(std::int64_t{0});
    BOOST_REQUIRE(one.has_value());
    BOOST_REQUIRE(two.has_value());
    BOOST_REQUIRE(three.has_value());
    BOOST_REQUIRE(zero.has_value());

    auto list_value = invoke_builtin(
        builtins,
        "list",
        std::array<LispVal, 3>{*one, *two, *three});
    BOOST_REQUIRE(list_value.has_value());

    auto length_value =
        invoke_builtin(builtins, "length", std::array<LispVal, 1>{*list_value});
    BOOST_REQUIRE(length_value.has_value());
    auto length_numeric = eta::runtime::classify_numeric(*length_value, heap);
    BOOST_REQUIRE(length_numeric.is_valid());
    BOOST_TEST(length_numeric.int_val == 3);

    auto list_ref_value = invoke_builtin(
        builtins,
        "list-ref",
        std::array<LispVal, 2>{*list_value, *one});
    BOOST_REQUIRE(list_ref_value.has_value());
    auto list_ref_numeric = eta::runtime::classify_numeric(*list_ref_value, heap);
    BOOST_REQUIRE(list_ref_numeric.is_valid());
    BOOST_TEST(list_ref_numeric.int_val == 2);

    auto plus_one = eta::runtime::make_primitive(
        heap,
        [&heap](std::span<const LispVal> args) -> std::expected<LispVal, eta::runtime::error::RuntimeError> {
            auto arg = eta::runtime::classify_numeric(args[0], heap);
            if (!arg.is_valid() || arg.is_flonum()) {
                return std::unexpected(eta::runtime::error::RuntimeError{
                    eta::runtime::error::VMError{
                        eta::runtime::error::RuntimeErrorCode::TypeError,
                        "map test helper: expected integer"}});
            }
            return eta::runtime::make_fixnum(heap, arg.int_val + 1);
        },
        1,
        false);
    BOOST_REQUIRE(plus_one.has_value());

    auto mapped_value = invoke_builtin(
        builtins,
        "map",
        std::array<LispVal, 2>{*plus_one, *list_value});
    BOOST_REQUIRE(mapped_value.has_value());

    auto mapped_head = invoke_builtin(
        builtins,
        "list-ref",
        std::array<LispVal, 2>{*mapped_value, *zero});
    BOOST_REQUIRE(mapped_head.has_value());
    auto mapped_head_numeric = eta::runtime::classify_numeric(*mapped_head, heap);
    BOOST_REQUIRE(mapped_head_numeric.is_valid());
    BOOST_TEST(mapped_head_numeric.int_val == 2);

    auto vector_value = invoke_builtin(
        builtins,
        "make-vector",
        std::array<LispVal, 2>{*three, *one});
    BOOST_REQUIRE(vector_value.has_value());
    auto vector_set = invoke_builtin(
        builtins,
        "vector-set!",
        std::array<LispVal, 3>{*vector_value, *one, *three});
    BOOST_REQUIRE(vector_set.has_value());
    auto vector_ref = invoke_builtin(
        builtins,
        "vector-ref",
        std::array<LispVal, 2>{*vector_value, *one});
    BOOST_REQUIRE(vector_ref.has_value());
    auto vector_numeric = eta::runtime::classify_numeric(*vector_ref, heap);
    BOOST_REQUIRE(vector_numeric.is_valid());
    BOOST_TEST(vector_numeric.int_val == 3);

    auto key = eta::runtime::make_symbol(intern_table, "k");
    BOOST_REQUIRE(key.has_value());
    auto hash_map = invoke_builtin(
        builtins,
        "hash-map",
        std::array<LispVal, 2>{*key, *two});
    BOOST_REQUIRE(hash_map.has_value());

    auto hash_map_ref = invoke_builtin(
        builtins,
        "hash-map-ref",
        std::array<LispVal, 2>{*hash_map, *key});
    BOOST_REQUIRE(hash_map_ref.has_value());
    auto map_ref_numeric = eta::runtime::classify_numeric(*hash_map_ref, heap);
    BOOST_REQUIRE(map_ref_numeric.is_valid());
    BOOST_TEST(map_ref_numeric.int_val == 2);

    auto map_as_list = invoke_builtin(
        builtins,
        "hash-map->list",
        std::array<LispVal, 1>{*hash_map});
    BOOST_REQUIRE(map_as_list.has_value());
    auto map_roundtrip = invoke_builtin(
        builtins,
        "list->hash-map",
        std::array<LispVal, 1>{*map_as_list});
    BOOST_REQUIRE(map_roundtrip.has_value());
    auto equal_res = invoke_builtin(
        builtins,
        "equal?",
        std::array<LispVal, 2>{*hash_map, *map_roundtrip});
    BOOST_REQUIRE(equal_res.has_value());
    BOOST_TEST(*equal_res == eta::runtime::nanbox::True);
}

BOOST_AUTO_TEST_CASE(
    string_symbol_and_char_primitives_preserve_conversion_and_comparison_behavior) {
    using eta::runtime::nanbox::LispVal;

    eta::runtime::memory::heap::Heap heap(1ull << 20);
    eta::runtime::memory::intern::InternTable intern_table;
    eta::runtime::vm::VM vm(heap, intern_table);

    eta::runtime::BuiltinEnvironment builtins;
    eta::runtime::register_core_primitives(builtins, heap, intern_table, &vm);

    auto symbol_alpha = eta::runtime::make_symbol(intern_table, "alpha");
    BOOST_REQUIRE(symbol_alpha.has_value());
    auto symbol_to_string = invoke_builtin(
        builtins, "symbol->string", std::array<LispVal, 1>{*symbol_alpha});
    BOOST_REQUIRE(symbol_to_string.has_value());
    auto alpha_sv = eta::runtime::StringView::try_from(*symbol_to_string, intern_table);
    BOOST_REQUIRE(alpha_sv.has_value());
    BOOST_TEST(alpha_sv->view() == "alpha");

    auto string_alpha = eta::runtime::make_string(heap, intern_table, "alpha");
    BOOST_REQUIRE(string_alpha.has_value());
    auto string_to_symbol = invoke_builtin(
        builtins, "string->symbol", std::array<LispVal, 1>{*string_alpha});
    BOOST_REQUIRE(string_to_symbol.has_value());
    BOOST_TEST(*string_to_symbol == *symbol_alpha);

    auto length_res = invoke_builtin(
        builtins, "string-length", std::array<LispVal, 1>{*string_alpha});
    BOOST_REQUIRE(length_res.has_value());
    auto length_numeric = eta::runtime::classify_numeric(*length_res, heap);
    BOOST_REQUIRE(length_numeric.is_valid());
    BOOST_TEST(length_numeric.int_val == 5);

    auto string_e = eta::runtime::make_string(heap, intern_table, "et");
    auto string_a = eta::runtime::make_string(heap, intern_table, "a");
    BOOST_REQUIRE(string_e.has_value());
    BOOST_REQUIRE(string_a.has_value());
    auto append_res = invoke_builtin(
        builtins, "string-append", std::array<LispVal, 2>{*string_e, *string_a});
    BOOST_REQUIRE(append_res.has_value());
    auto append_sv = eta::runtime::StringView::try_from(*append_res, intern_table);
    BOOST_REQUIRE(append_sv.has_value());
    BOOST_TEST(append_sv->view() == "eta");

    const auto forty_two = eta::runtime::nanbox::ops::encode(std::int64_t{42});
    BOOST_REQUIRE(forty_two.has_value());
    auto number_to_string = invoke_builtin(
        builtins, "number->string", std::array<LispVal, 1>{*forty_two});
    BOOST_REQUIRE(number_to_string.has_value());
    auto number_text_sv = eta::runtime::StringView::try_from(*number_to_string, intern_table);
    BOOST_REQUIRE(number_text_sv.has_value());
    BOOST_TEST(number_text_sv->view() == "42");

    auto string_42 = eta::runtime::make_string(heap, intern_table, "42");
    auto string_3_5 = eta::runtime::make_string(heap, intern_table, "3.5");
    auto string_invalid = eta::runtime::make_string(heap, intern_table, "not-a-number");
    BOOST_REQUIRE(string_42.has_value());
    BOOST_REQUIRE(string_3_5.has_value());
    BOOST_REQUIRE(string_invalid.has_value());

    auto parse_int = invoke_builtin(
        builtins, "string->number", std::array<LispVal, 1>{*string_42});
    BOOST_REQUIRE(parse_int.has_value());
    auto parse_int_numeric = eta::runtime::classify_numeric(*parse_int, heap);
    BOOST_REQUIRE(parse_int_numeric.is_valid());
    BOOST_TEST(parse_int_numeric.int_val == 42);

    auto parse_float = invoke_builtin(
        builtins, "string->number", std::array<LispVal, 1>{*string_3_5});
    BOOST_REQUIRE(parse_float.has_value());
    auto parse_float_numeric = eta::runtime::classify_numeric(*parse_float, heap);
    BOOST_REQUIRE(parse_float_numeric.is_valid());
    BOOST_TEST(parse_float_numeric.as_double() == 3.5);

    auto parse_fail = invoke_builtin(
        builtins, "string->number", std::array<LispVal, 1>{*string_invalid});
    BOOST_REQUIRE(parse_fail.has_value());
    BOOST_TEST(*parse_fail == eta::runtime::nanbox::False);

    auto string_eta = eta::runtime::make_string(heap, intern_table, "Eta");
    const auto one = eta::runtime::nanbox::ops::encode(std::int64_t{1});
    BOOST_REQUIRE(string_eta.has_value());
    BOOST_REQUIRE(one.has_value());
    auto string_ref = invoke_builtin(
        builtins, "string-ref", std::array<LispVal, 2>{*string_eta, *one});
    BOOST_REQUIRE(string_ref.has_value());
    auto ref_char = eta::runtime::nanbox::ops::decode<char32_t>(*string_ref);
    BOOST_REQUIRE(ref_char.has_value());
    BOOST_TEST(
        static_cast<std::uint32_t>(*ref_char)
        == static_cast<std::uint32_t>(U't'));

    auto string_runtime = eta::runtime::make_string(heap, intern_table, "runtime");
    BOOST_REQUIRE(string_runtime.has_value());
    auto four = eta::runtime::nanbox::ops::encode(std::int64_t{4});
    BOOST_REQUIRE(four.has_value());
    auto substring_res = invoke_builtin(
        builtins, "substring", std::array<LispVal, 3>{*string_runtime, *one, *four});
    BOOST_REQUIRE(substring_res.has_value());
    auto substring_sv = eta::runtime::StringView::try_from(*substring_res, intern_table);
    BOOST_REQUIRE(substring_sv.has_value());
    BOOST_TEST(substring_sv->view() == "unt");

    auto string_abc = eta::runtime::make_string(heap, intern_table, "abc");
    auto string_abd = eta::runtime::make_string(heap, intern_table, "abd");
    BOOST_REQUIRE(string_abc.has_value());
    BOOST_REQUIRE(string_abd.has_value());

    auto lt_res = invoke_builtin(
        builtins, "string<?", std::array<LispVal, 2>{*string_abc, *string_abd});
    BOOST_REQUIRE(lt_res.has_value());
    BOOST_TEST(*lt_res == eta::runtime::nanbox::True);

    auto ge_res = invoke_builtin(
        builtins, "string>=?", std::array<LispVal, 2>{*string_abd, *string_abc});
    BOOST_REQUIRE(ge_res.has_value());
    BOOST_TEST(*ge_res == eta::runtime::nanbox::True);

    const auto char_a = eta::runtime::nanbox::ops::encode(U'A');
    BOOST_REQUIRE(char_a.has_value());
    auto char_to_integer = invoke_builtin(
        builtins, "char->integer", std::array<LispVal, 1>{*char_a});
    BOOST_REQUIRE(char_to_integer.has_value());
    auto char_code = eta::runtime::nanbox::ops::decode<std::int64_t>(*char_to_integer);
    BOOST_REQUIRE(char_code.has_value());
    BOOST_TEST(*char_code == 65);

    const auto sixty_five = eta::runtime::nanbox::ops::encode(std::int64_t{65});
    BOOST_REQUIRE(sixty_five.has_value());
    auto integer_to_char = invoke_builtin(
        builtins, "integer->char", std::array<LispVal, 1>{*sixty_five});
    BOOST_REQUIRE(integer_to_char.has_value());
    auto roundtrip_char = eta::runtime::nanbox::ops::decode<char32_t>(*integer_to_char);
    BOOST_REQUIRE(roundtrip_char.has_value());
    BOOST_TEST(
        static_cast<std::uint32_t>(*roundtrip_char)
        == static_cast<std::uint32_t>(U'A'));
}

BOOST_AUTO_TEST_CASE(
    misc_primitives_preserve_runtime_error_profiler_guardian_and_eval_behavior) {
    using eta::runtime::nanbox::LispVal;

    eta::runtime::memory::heap::Heap heap(1ull << 20);
    eta::runtime::memory::intern::InternTable intern_table;
    eta::runtime::vm::VM vm(heap, intern_table);

    eta::runtime::BuiltinEnvironment builtins;
    eta::runtime::register_core_primitives(builtins, heap, intern_table, &vm);

    auto seven = eta::runtime::nanbox::ops::encode(std::int64_t{7});
    auto boom = eta::runtime::make_string(heap, intern_table, "boom");
    BOOST_REQUIRE(seven.has_value());
    BOOST_REQUIRE(boom.has_value());

    auto error_result = invoke_builtin(
        builtins, "error", std::array<LispVal, 2>{*boom, *seven});
    BOOST_REQUIRE(!error_result.has_value());
    auto* error_vm_error =
        std::get_if<eta::runtime::error::VMError>(&error_result.error());
    BOOST_REQUIRE(error_vm_error != nullptr);
    BOOST_TEST(
        static_cast<int>(error_vm_error->code)
        == static_cast<int>(eta::runtime::error::RuntimeErrorCode::UserError));
    BOOST_TEST(error_vm_error->message == "boom 7");

    auto platform_result =
        invoke_builtin(builtins, "platform", std::array<LispVal, 0>{});
    BOOST_REQUIRE(platform_result.has_value());
    auto platform_name =
        eta::runtime::get_symbol_name(*platform_result, intern_table);
    BOOST_REQUIRE(platform_name.has_value());
#if defined(_WIN32)
    BOOST_TEST(*platform_name == "Win32");
#elif defined(__APPLE__)
    BOOST_TEST(*platform_name == "Darwin");
#elif defined(__linux__)
    BOOST_TEST(*platform_name == "Linux");
#else
    BOOST_TEST(*platform_name == "Unknown");
#endif

    auto prof_enabled =
        invoke_builtin(builtins, "%prof-enabled?", std::array<LispVal, 0>{});
    BOOST_REQUIRE(prof_enabled.has_value());
    const bool prof_enabled_is_boolean =
        (*prof_enabled == eta::runtime::nanbox::True)
        || (*prof_enabled == eta::runtime::nanbox::False);
    BOOST_TEST(prof_enabled_is_boolean);

    auto invalid_mode = eta::runtime::make_string(heap, intern_table, "invalid");
    BOOST_REQUIRE(invalid_mode.has_value());
    auto prof_start_invalid = invoke_builtin(
        builtins, "%prof-start", std::array<LispVal, 1>{*invalid_mode});
    BOOST_REQUIRE(!prof_start_invalid.has_value());
    auto* prof_start_error =
        std::get_if<eta::runtime::error::VMError>(&prof_start_invalid.error());
    BOOST_REQUIRE(prof_start_error != nullptr);
    BOOST_TEST(
        static_cast<int>(prof_start_error->code)
        == static_cast<int>(eta::runtime::error::RuntimeErrorCode::UserError));
    BOOST_TEST(
        prof_start_error->message
        == "%prof-start: mode must be 'trace' or 'sample'");

    auto prof_report_arity = invoke_builtin(
        builtins,
        "%prof-report",
        std::array<LispVal, 3>{*seven, *seven, *seven});
    BOOST_REQUIRE(!prof_report_arity.has_value());
    auto* prof_report_error =
        std::get_if<eta::runtime::error::VMError>(&prof_report_arity.error());
    BOOST_REQUIRE(prof_report_error != nullptr);
    BOOST_TEST(
        static_cast<int>(prof_report_error->code)
        == static_cast<int>(eta::runtime::error::RuntimeErrorCode::InvalidArity));

    auto object_value = eta::runtime::make_vector(heap, std::vector<LispVal>{*seven});
    BOOST_REQUIRE(object_value.has_value());
    auto finalizer_proc = eta::runtime::make_primitive(
        heap,
        [](std::span<const LispVal>) -> std::expected<LispVal, eta::runtime::error::RuntimeError> {
            return eta::runtime::nanbox::Nil;
        },
        1,
        false);
    BOOST_REQUIRE(finalizer_proc.has_value());

    auto register_finalizer = invoke_builtin(
        builtins,
        "register-finalizer!",
        std::array<LispVal, 2>{*object_value, *finalizer_proc});
    BOOST_REQUIRE(register_finalizer.has_value());
    BOOST_TEST(*register_finalizer == eta::runtime::nanbox::True);

    auto unregister_finalizer = invoke_builtin(
        builtins,
        "unregister-finalizer!",
        std::array<LispVal, 1>{*object_value});
    BOOST_REQUIRE(unregister_finalizer.has_value());
    BOOST_TEST(*unregister_finalizer == eta::runtime::nanbox::True);

    auto unregister_again = invoke_builtin(
        builtins,
        "unregister-finalizer!",
        std::array<LispVal, 1>{*object_value});
    BOOST_REQUIRE(unregister_again.has_value());
    BOOST_TEST(*unregister_again == eta::runtime::nanbox::False);

    auto guardian = invoke_builtin(
        builtins, "make-guardian", std::array<LispVal, 0>{});
    BOOST_REQUIRE(guardian.has_value());

    auto track_result = invoke_builtin(
        builtins,
        "guardian-track!",
        std::array<LispVal, 2>{*guardian, *object_value});
    BOOST_REQUIRE(track_result.has_value());
    BOOST_TEST(*track_result == eta::runtime::nanbox::True);

    auto collect_result = invoke_builtin(
        builtins,
        "guardian-collect",
        std::array<LispVal, 1>{*guardian});
    BOOST_REQUIRE(collect_result.has_value());
    BOOST_TEST(*collect_result == eta::runtime::nanbox::False);

    auto eval_result =
        invoke_builtin(builtins, "eval", std::array<LispVal, 1>{*seven});
    BOOST_REQUIRE(!eval_result.has_value());
    auto* eval_vm_error =
        std::get_if<eta::runtime::error::VMError>(&eval_result.error());
    BOOST_REQUIRE(eval_vm_error != nullptr);
    BOOST_TEST(
        static_cast<int>(eval_vm_error->code)
        == static_cast<int>(eta::runtime::error::RuntimeErrorCode::InternalError));
    BOOST_TEST(
        eval_vm_error->message
        == "eval: runtime stub invoked before driver installation");
}

BOOST_AUTO_TEST_CASE(
    logic_primitives_preserve_attr_occurs_term_dual_and_prop_attr_behavior) {
    using eta::runtime::nanbox::LispVal;

    eta::runtime::memory::heap::Heap heap(1ull << 20);
    eta::runtime::memory::intern::InternTable intern_table;
    eta::runtime::vm::VM vm(heap, intern_table);

    eta::runtime::BuiltinEnvironment builtins;
    eta::runtime::register_core_primitives(builtins, heap, intern_table, &vm);

    auto one = eta::runtime::nanbox::ops::encode(std::int64_t{1});
    auto two = eta::runtime::nanbox::ops::encode(std::int64_t{2});
    BOOST_REQUIRE(one.has_value());
    BOOST_REQUIRE(two.has_value());

    auto module_symbol = eta::runtime::make_symbol(intern_table, "logic.mod");
    auto never_symbol = eta::runtime::make_symbol(intern_table, "never");
    auto error_symbol = eta::runtime::make_symbol(intern_table, "error");
    auto functor_symbol = eta::runtime::make_symbol(intern_table, "f");
    auto var_name = eta::runtime::make_string(heap, intern_table, "eta-var");
    BOOST_REQUIRE(module_symbol.has_value());
    BOOST_REQUIRE(never_symbol.has_value());
    BOOST_REQUIRE(error_symbol.has_value());
    BOOST_REQUIRE(functor_symbol.has_value());
    BOOST_REQUIRE(var_name.has_value());

    auto logic_var = invoke_builtin(
        builtins, "logic-var/named", std::array<LispVal, 1>{*var_name});
    BOOST_REQUIRE(logic_var.has_value());

    auto logic_var_pred = invoke_builtin(
        builtins, "logic-var?", std::array<LispVal, 1>{*logic_var});
    BOOST_REQUIRE(logic_var_pred.has_value());
    BOOST_TEST(*logic_var_pred == eta::runtime::nanbox::True);

    auto var_name_result = invoke_builtin(
        builtins, "var-name", std::array<LispVal, 1>{*logic_var});
    BOOST_REQUIRE(var_name_result.has_value());
    auto var_name_view =
        eta::runtime::StringView::try_from(*var_name_result, intern_table);
    BOOST_REQUIRE(var_name_view.has_value());
    BOOST_TEST(var_name_view->view() == "eta-var");

    auto put_attr = invoke_builtin(
        builtins,
        "put-attr",
        std::array<LispVal, 3>{*logic_var, *module_symbol, *one});
    BOOST_REQUIRE(put_attr.has_value());
    BOOST_TEST(*put_attr == eta::runtime::nanbox::True);

    auto get_attr = invoke_builtin(
        builtins,
        "get-attr",
        std::array<LispVal, 2>{*logic_var, *module_symbol});
    BOOST_REQUIRE(get_attr.has_value());
    BOOST_TEST(*get_attr == *one);

    auto attr_var_true = invoke_builtin(
        builtins, "attr-var?", std::array<LispVal, 1>{*logic_var});
    BOOST_REQUIRE(attr_var_true.has_value());
    BOOST_TEST(*attr_var_true == eta::runtime::nanbox::True);

    auto del_attr = invoke_builtin(
        builtins,
        "del-attr",
        std::array<LispVal, 2>{*logic_var, *module_symbol});
    BOOST_REQUIRE(del_attr.has_value());
    BOOST_TEST(*del_attr == eta::runtime::nanbox::True);

    auto attr_var_false = invoke_builtin(
        builtins, "attr-var?", std::array<LispVal, 1>{*logic_var});
    BOOST_REQUIRE(attr_var_false.has_value());
    BOOST_TEST(*attr_var_false == eta::runtime::nanbox::False);

    auto get_attr_missing = invoke_builtin(
        builtins,
        "get-attr",
        std::array<LispVal, 2>{*logic_var, *module_symbol});
    BOOST_REQUIRE(get_attr_missing.has_value());
    BOOST_TEST(*get_attr_missing == eta::runtime::nanbox::False);

    auto hook_proc = eta::runtime::make_primitive(
        heap,
        [](std::span<const LispVal>) -> std::expected<LispVal, eta::runtime::error::RuntimeError> {
            return eta::runtime::nanbox::True;
        },
        4,
        false);
    BOOST_REQUIRE(hook_proc.has_value());

    auto register_hook = invoke_builtin(
        builtins,
        "register-attr-hook!",
        std::array<LispVal, 2>{*module_symbol, *hook_proc});
    BOOST_REQUIRE(register_hook.has_value());
    BOOST_TEST(*register_hook == eta::runtime::nanbox::True);

    const auto module_id = static_cast<eta::runtime::memory::intern::InternId>(
        eta::runtime::nanbox::ops::payload(*module_symbol));
    BOOST_TEST(vm.attr_unify_hooks().contains(module_id));
    BOOST_TEST(vm.attr_unify_hooks().at(module_id) == *hook_proc);

    auto set_never = invoke_builtin(
        builtins,
        "set-occurs-check!",
        std::array<LispVal, 1>{*never_symbol});
    BOOST_REQUIRE(set_never.has_value());
    BOOST_TEST(*set_never == eta::runtime::nanbox::True);

    auto occurs_never = invoke_builtin(
        builtins, "occurs-check-mode", std::array<LispVal, 0>{});
    BOOST_REQUIRE(occurs_never.has_value());
    BOOST_TEST(*occurs_never == *never_symbol);

    auto set_error = invoke_builtin(
        builtins,
        "set-occurs-check!",
        std::array<LispVal, 1>{*error_symbol});
    BOOST_REQUIRE(set_error.has_value());
    BOOST_TEST(*set_error == eta::runtime::nanbox::True);

    auto occurs_error = invoke_builtin(
        builtins, "occurs-check-mode", std::array<LispVal, 0>{});
    BOOST_REQUIRE(occurs_error.has_value());
    BOOST_TEST(*occurs_error == *error_symbol);

    auto ground_var = invoke_builtin(
        builtins, "ground?", std::array<LispVal, 1>{*logic_var});
    BOOST_REQUIRE(ground_var.has_value());
    BOOST_TEST(*ground_var == eta::runtime::nanbox::False);

    auto compound_term = invoke_builtin(
        builtins,
        "term",
        std::array<LispVal, 3>{*functor_symbol, *one, *two});
    BOOST_REQUIRE(compound_term.has_value());

    auto compound_pred = invoke_builtin(
        builtins, "compound?", std::array<LispVal, 1>{*compound_term});
    BOOST_REQUIRE(compound_pred.has_value());
    BOOST_TEST(*compound_pred == eta::runtime::nanbox::True);

    auto ground_term = invoke_builtin(
        builtins, "ground?", std::array<LispVal, 1>{*compound_term});
    BOOST_REQUIRE(ground_term.has_value());
    BOOST_TEST(*ground_term == eta::runtime::nanbox::True);

    auto functor = invoke_builtin(
        builtins, "functor", std::array<LispVal, 1>{*compound_term});
    BOOST_REQUIRE(functor.has_value());
    BOOST_TEST(*functor == *functor_symbol);

    auto arity = invoke_builtin(
        builtins, "arity", std::array<LispVal, 1>{*compound_term});
    BOOST_REQUIRE(arity.has_value());
    auto arity_numeric = eta::runtime::classify_numeric(*arity, heap);
    BOOST_REQUIRE(arity_numeric.is_valid());
    BOOST_TEST(arity_numeric.int_val == 2);

    auto arg_first = invoke_builtin(
        builtins, "arg", std::array<LispVal, 2>{*one, *compound_term});
    BOOST_REQUIRE(arg_first.has_value());
    BOOST_TEST(*arg_first == *one);

    auto dual_pred = invoke_builtin(
        builtins, "dual?", std::array<LispVal, 1>{*one});
    BOOST_REQUIRE(dual_pred.has_value());
    BOOST_TEST(*dual_pred == eta::runtime::nanbox::False);

    auto dual_primal = invoke_builtin(
        builtins, "dual-primal", std::array<LispVal, 1>{*one});
    BOOST_REQUIRE(dual_primal.has_value());
    BOOST_TEST(*dual_primal == *one);

    auto dual_backprop = invoke_builtin(
        builtins, "dual-backprop", std::array<LispVal, 1>{*one});
    BOOST_REQUIRE(dual_backprop.has_value());
    BOOST_REQUIRE(eta::runtime::nanbox::ops::is_boxed(*dual_backprop));
    BOOST_TEST(
        eta::runtime::nanbox::ops::tag(*dual_backprop)
        == eta::runtime::nanbox::Tag::HeapObject);
    auto* backprop_primitive = heap.try_get_as<
        eta::runtime::memory::heap::ObjectKind::Primitive,
        eta::runtime::types::Primitive>(
        eta::runtime::nanbox::ops::payload(*dual_backprop));
    BOOST_REQUIRE(backprop_primitive != nullptr);
    const std::array<LispVal, 1> backprop_args{*one};
    auto backprop_eval =
        backprop_primitive->func(std::span<const LispVal>(backprop_args));
    BOOST_REQUIRE(backprop_eval.has_value());
    BOOST_TEST(*backprop_eval == eta::runtime::nanbox::Nil);

    auto make_dual = invoke_builtin(
        builtins, "make-dual", std::array<LispVal, 2>{*one, *two});
    BOOST_REQUIRE(!make_dual.has_value());
    auto* make_dual_error =
        std::get_if<eta::runtime::error::VMError>(&make_dual.error());
    BOOST_REQUIRE(make_dual_error != nullptr);
    BOOST_TEST(
        make_dual_error->message
        == "make-dual: Dual AD has been removed  -  use tape-based AD instead");

    auto register_prop_attr = invoke_builtin(
        builtins,
        "register-prop-attr!",
        std::array<LispVal, 1>{*module_symbol});
    BOOST_REQUIRE(register_prop_attr.has_value());
    BOOST_TEST(*register_prop_attr == eta::runtime::nanbox::True);
    BOOST_TEST(vm.async_thunk_attrs().contains(module_id));
}

BOOST_AUTO_TEST_CASE(
    clp_primitives_preserve_domain_lookup_and_queue_size_behavior) {
    using eta::runtime::nanbox::LispVal;

    eta::runtime::memory::heap::Heap heap(1ull << 20);
    eta::runtime::memory::intern::InternTable intern_table;
    eta::runtime::vm::VM vm(heap, intern_table);

    eta::runtime::BuiltinEnvironment builtins;
    eta::runtime::register_core_primitives(builtins, heap, intern_table, &vm);

    auto one = eta::runtime::nanbox::ops::encode(std::int64_t{1});
    auto three = eta::runtime::nanbox::ops::encode(std::int64_t{3});
    BOOST_REQUIRE(one.has_value());
    BOOST_REQUIRE(three.has_value());

    auto var_name = eta::runtime::make_string(heap, intern_table, "clp-var");
    BOOST_REQUIRE(var_name.has_value());

    auto logic_var = invoke_builtin(
        builtins, "logic-var/named", std::array<LispVal, 1>{*var_name});
    BOOST_REQUIRE(logic_var.has_value());

    auto set_domain = invoke_builtin(
        builtins,
        "%clp-domain-z!",
        std::array<LispVal, 3>{*logic_var, *one, *three});
    BOOST_REQUIRE(set_domain.has_value());
    BOOST_TEST(*set_domain == eta::runtime::nanbox::True);

    auto get_domain = invoke_builtin(
        builtins, "%clp-get-domain", std::array<LispVal, 1>{*logic_var});
    BOOST_REQUIRE(get_domain.has_value());
    BOOST_REQUIRE(*get_domain != eta::runtime::nanbox::False);

    auto* domain_cell = heap.try_get_as<
        eta::runtime::memory::heap::ObjectKind::Cons,
        eta::runtime::types::Cons>(eta::runtime::nanbox::ops::payload(*get_domain));
    BOOST_REQUIRE(domain_cell != nullptr);
    auto domain_kind = eta::runtime::get_symbol_name(domain_cell->car, intern_table);
    BOOST_REQUIRE(domain_kind.has_value());
    BOOST_TEST(*domain_kind == "z");

    BOOST_REQUIRE(eta::runtime::nanbox::ops::is_boxed(domain_cell->cdr));
    BOOST_TEST(
        eta::runtime::nanbox::ops::tag(domain_cell->cdr)
        == eta::runtime::nanbox::Tag::HeapObject);
    auto* lo_cell = heap.try_get_as<
        eta::runtime::memory::heap::ObjectKind::Cons,
        eta::runtime::types::Cons>(eta::runtime::nanbox::ops::payload(domain_cell->cdr));
    BOOST_REQUIRE(lo_cell != nullptr);
    auto lo = eta::runtime::nanbox::ops::decode<std::int64_t>(lo_cell->car);
    BOOST_REQUIRE(lo.has_value());
    BOOST_TEST(*lo == 1);

    BOOST_REQUIRE(eta::runtime::nanbox::ops::is_boxed(lo_cell->cdr));
    BOOST_TEST(
        eta::runtime::nanbox::ops::tag(lo_cell->cdr)
        == eta::runtime::nanbox::Tag::HeapObject);
    auto* hi_cell = heap.try_get_as<
        eta::runtime::memory::heap::ObjectKind::Cons,
        eta::runtime::types::Cons>(eta::runtime::nanbox::ops::payload(lo_cell->cdr));
    BOOST_REQUIRE(hi_cell != nullptr);
    auto hi = eta::runtime::nanbox::ops::decode<std::int64_t>(hi_cell->car);
    BOOST_REQUIRE(hi.has_value());
    BOOST_TEST(*hi == 3);
    BOOST_TEST(hi_cell->cdr == eta::runtime::nanbox::Nil);

    auto queue_size = invoke_builtin(
        builtins, "%clp-prop-queue-size", std::array<LispVal, 0>{});
    BOOST_REQUIRE(queue_size.has_value());
    auto queue_size_value =
        eta::runtime::nanbox::ops::decode<std::int64_t>(*queue_size);
    BOOST_REQUIRE(queue_size_value.has_value());
    BOOST_TEST(*queue_size_value >= 0);
}

BOOST_AUTO_TEST_CASE(
    aad_tape_control_primitives_preserve_lifecycle_and_reference_access_behavior) {
    using eta::runtime::nanbox::LispVal;

    eta::runtime::memory::heap::Heap heap(1ull << 20);
    eta::runtime::memory::intern::InternTable intern_table;
    eta::runtime::vm::VM vm(heap, intern_table);

    eta::runtime::BuiltinEnvironment builtins;
    eta::runtime::register_core_primitives(builtins, heap, intern_table, &vm);

    auto tape_res = invoke_builtin(builtins, "tape-new", std::array<LispVal, 0>{});
    BOOST_REQUIRE(tape_res.has_value());
    const LispVal tape_val = *tape_res;

    auto tape_ref_pred_non_ref =
        invoke_builtin(builtins, "tape-ref?", std::array<LispVal, 1>{tape_val});
    BOOST_REQUIRE(tape_ref_pred_non_ref.has_value());
    BOOST_TEST(*tape_ref_pred_non_ref == eta::runtime::nanbox::False);

    auto start_res =
        invoke_builtin(builtins, "tape-start!", std::array<LispVal, 1>{tape_val});
    BOOST_REQUIRE(start_res.has_value());
    BOOST_TEST(*start_res == eta::runtime::nanbox::True);

    auto input = eta::runtime::make_flonum(2.5);
    BOOST_REQUIRE(input.has_value());
    auto ref_res =
        invoke_builtin(builtins, "tape-var", std::array<LispVal, 2>{tape_val, *input});
    BOOST_REQUIRE(ref_res.has_value());
    const LispVal tape_ref = *ref_res;

    auto tape_ref_pred = invoke_builtin(
        builtins, "tape-ref?", std::array<LispVal, 1>{tape_ref});
    BOOST_REQUIRE(tape_ref_pred.has_value());
    BOOST_TEST(*tape_ref_pred == eta::runtime::nanbox::True);

    auto ref_index_res = invoke_builtin(
        builtins, "tape-ref-index", std::array<LispVal, 1>{tape_ref});
    BOOST_REQUIRE(ref_index_res.has_value());
    auto ref_index = eta::runtime::nanbox::ops::decode<std::int64_t>(*ref_index_res);
    BOOST_REQUIRE(ref_index.has_value());
    BOOST_TEST(*ref_index >= 0);

    auto tape_size_res = invoke_builtin(
        builtins, "tape-size", std::array<LispVal, 1>{tape_val});
    BOOST_REQUIRE(tape_size_res.has_value());
    auto tape_size = eta::runtime::nanbox::ops::decode<std::int64_t>(*tape_size_res);
    BOOST_REQUIRE(tape_size.has_value());
    BOOST_TEST(*tape_size >= *ref_index + 1);

    auto primal_res = invoke_builtin(
        builtins, "tape-primal", std::array<LispVal, 2>{tape_val, tape_ref});
    BOOST_REQUIRE(primal_res.has_value());
    auto primal = eta::runtime::classify_numeric(*primal_res, heap);
    BOOST_REQUIRE(primal.is_valid());
    BOOST_TEST(primal.as_double() == 2.5);

    auto value_of_res = invoke_builtin(
        builtins, "tape-ref-value-of", std::array<LispVal, 2>{tape_val, tape_ref});
    BOOST_REQUIRE(value_of_res.has_value());
    auto value_of = eta::runtime::classify_numeric(*value_of_res, heap);
    BOOST_REQUIRE(value_of.is_valid());
    BOOST_TEST(value_of.as_double() == 2.5);

    auto value_res = invoke_builtin(
        builtins, "tape-ref-value", std::array<LispVal, 1>{tape_ref});
    BOOST_REQUIRE(value_res.has_value());
    auto value = eta::runtime::classify_numeric(*value_res, heap);
    BOOST_REQUIRE(value.is_valid());
    BOOST_TEST(value.as_double() == 2.5);

    auto stop_res = invoke_builtin(builtins, "tape-stop!", std::array<LispVal, 0>{});
    BOOST_REQUIRE(stop_res.has_value());
    BOOST_TEST(*stop_res == eta::runtime::nanbox::True);

    auto no_active_value_res = invoke_builtin(
        builtins, "tape-ref-value", std::array<LispVal, 1>{tape_ref});
    BOOST_REQUIRE(!no_active_value_res.has_value());
    auto* no_active_error =
        std::get_if<eta::runtime::error::VMError>(&no_active_value_res.error());
    BOOST_REQUIRE(no_active_error != nullptr);
    BOOST_TEST(no_active_error->tag_override == std::string(eta::runtime::ad::kTagNoActiveTape));
    BOOST_TEST(no_active_error->message == "tape-ref-value: no active tape");

    auto seven = eta::runtime::nanbox::ops::encode(std::int64_t{7});
    BOOST_REQUIRE(seven.has_value());
    auto passthrough_res = invoke_builtin(
        builtins, "tape-ref-value", std::array<LispVal, 1>{*seven});
    BOOST_REQUIRE(passthrough_res.has_value());
    BOOST_TEST(*passthrough_res == *seven);

    auto clear_res = invoke_builtin(
        builtins, "tape-clear!", std::array<LispVal, 1>{tape_val});
    BOOST_REQUIRE(clear_res.has_value());

    auto stale_primal_res = invoke_builtin(
        builtins, "tape-primal", std::array<LispVal, 2>{tape_val, tape_ref});
    BOOST_REQUIRE(!stale_primal_res.has_value());
    auto* stale_primal_error =
        std::get_if<eta::runtime::error::VMError>(&stale_primal_res.error());
    BOOST_REQUIRE(stale_primal_error != nullptr);
    BOOST_TEST(stale_primal_error->tag_override == std::string(eta::runtime::ad::kTagStaleRef));
    BOOST_TEST(stale_primal_error->message == "tape-primal: stale TapeRef generation");
}

BOOST_AUTO_TEST_CASE(
    stats_primitives_preserve_fact_table_hash_and_stats_behavior) {
    using eta::runtime::nanbox::LispVal;
    using eta::runtime::nanbox::ops::payload;
    using eta::runtime::memory::heap::ObjectKind;

    eta::runtime::memory::heap::Heap heap(1ull << 20);
    eta::runtime::memory::intern::InternTable intern_table;
    eta::runtime::vm::VM vm(heap, intern_table);

    eta::runtime::BuiltinEnvironment builtins;
    eta::runtime::register_core_primitives(builtins, heap, intern_table, &vm);

    auto one = eta::runtime::nanbox::ops::encode(std::int64_t{1});
    auto two = eta::runtime::nanbox::ops::encode(std::int64_t{2});
    auto three = eta::runtime::nanbox::ops::encode(std::int64_t{3});
    auto four = eta::runtime::nanbox::ops::encode(std::int64_t{4});
    auto six = eta::runtime::nanbox::ops::encode(std::int64_t{6});
    auto ten = eta::runtime::nanbox::ops::encode(std::int64_t{10});
    auto twenty = eta::runtime::nanbox::ops::encode(std::int64_t{20});
    auto depth = eta::runtime::nanbox::ops::encode(std::int64_t{8});
    auto zero = eta::runtime::nanbox::ops::encode(std::int64_t{0});
    BOOST_REQUIRE(one.has_value());
    BOOST_REQUIRE(two.has_value());
    BOOST_REQUIRE(three.has_value());
    BOOST_REQUIRE(four.has_value());
    BOOST_REQUIRE(six.has_value());
    BOOST_REQUIRE(ten.has_value());
    BOOST_REQUIRE(twenty.has_value());
    BOOST_REQUIRE(depth.has_value());
    BOOST_REQUIRE(zero.has_value());

    auto sym_x = eta::runtime::make_symbol(intern_table, "x");
    auto sym_y = eta::runtime::make_symbol(intern_table, "y");
    BOOST_REQUIRE(sym_x.has_value());
    BOOST_REQUIRE(sym_y.has_value());

    auto name_tail = eta::runtime::make_cons(heap, *sym_y, eta::runtime::nanbox::Nil);
    BOOST_REQUIRE(name_tail.has_value());
    auto name_list = eta::runtime::make_cons(heap, *sym_x, *name_tail);
    BOOST_REQUIRE(name_list.has_value());

    auto table_res = invoke_builtin(
        builtins,
        "%make-fact-table",
        std::array<LispVal, 1>{*name_list});
    BOOST_REQUIRE(table_res.has_value());
    const LispVal table = *table_res;

    auto table_pred = invoke_builtin(
        builtins,
        "fact-table?",
        std::array<LispVal, 1>{table});
    BOOST_REQUIRE(table_pred.has_value());
    BOOST_TEST(*table_pred == eta::runtime::nanbox::True);

    auto make_row = [&](LispVal first, LispVal second) -> LispVal {
        auto tail = eta::runtime::make_cons(heap, second, eta::runtime::nanbox::Nil);
        BOOST_REQUIRE(tail.has_value());
        auto head = eta::runtime::make_cons(heap, first, *tail);
        BOOST_REQUIRE(head.has_value());
        return *head;
    };

    const LispVal row1 = make_row(*one, *ten);
    const LispVal row2 = make_row(*one, *twenty);
    const LispVal row3 = make_row(*two, *six);

    auto insert1 = invoke_builtin(
        builtins,
        "%fact-table-insert!",
        std::array<LispVal, 2>{table, row1});
    auto insert2 = invoke_builtin(
        builtins,
        "%fact-table-insert!",
        std::array<LispVal, 2>{table, row2});
    auto insert3 = invoke_builtin(
        builtins,
        "%fact-table-insert!",
        std::array<LispVal, 2>{table, row3});
    BOOST_REQUIRE(insert1.has_value());
    BOOST_REQUIRE(insert2.has_value());
    BOOST_REQUIRE(insert3.has_value());
    BOOST_TEST(*insert1 == eta::runtime::nanbox::True);
    BOOST_TEST(*insert2 == eta::runtime::nanbox::True);
    BOOST_TEST(*insert3 == eta::runtime::nanbox::True);

    auto row_count_res = invoke_builtin(
        builtins,
        "%fact-table-row-count",
        std::array<LispVal, 1>{table});
    BOOST_REQUIRE(row_count_res.has_value());
    auto row_count = eta::runtime::nanbox::ops::decode<std::int64_t>(*row_count_res);
    BOOST_REQUIRE(row_count.has_value());
    BOOST_TEST(*row_count == 3);

    auto parse_numeric_alist =
        [&](LispVal alist) -> std::unordered_map<std::int64_t, double> {
            std::unordered_map<std::int64_t, double> out;
            LispVal cur = alist;
            while (cur != eta::runtime::nanbox::Nil) {
                BOOST_REQUIRE(eta::runtime::nanbox::ops::is_boxed(cur));
                BOOST_TEST(
                    eta::runtime::nanbox::ops::tag(cur)
                    == eta::runtime::nanbox::Tag::HeapObject);
                auto* cell =
                    heap.try_get_as<ObjectKind::Cons, eta::runtime::types::Cons>(payload(cur));
                BOOST_REQUIRE(cell != nullptr);

                BOOST_REQUIRE(eta::runtime::nanbox::ops::is_boxed(cell->car));
                BOOST_TEST(
                    eta::runtime::nanbox::ops::tag(cell->car)
                    == eta::runtime::nanbox::Tag::HeapObject);
                auto* pair =
                    heap.try_get_as<ObjectKind::Cons, eta::runtime::types::Cons>(payload(cell->car));
                BOOST_REQUIRE(pair != nullptr);

                auto key = eta::runtime::nanbox::ops::decode<std::int64_t>(pair->car);
                BOOST_REQUIRE(key.has_value());
                auto value = eta::runtime::classify_numeric(pair->cdr, heap);
                BOOST_REQUIRE(value.is_valid());
                out[*key] = value.as_double();

                cur = cell->cdr;
            }
            return out;
        };

    auto group_count_res = invoke_builtin(
        builtins,
        "%fact-table-group-count",
        std::array<LispVal, 2>{table, *zero});
    BOOST_REQUIRE(group_count_res.has_value());
    auto group_count = parse_numeric_alist(*group_count_res);
    BOOST_TEST(group_count.at(1) == 2.0);
    BOOST_TEST(group_count.at(2) == 1.0);

    auto value_column = eta::runtime::nanbox::ops::encode(std::int64_t{1});
    BOOST_REQUIRE(value_column.has_value());
    auto group_sum_res = invoke_builtin(
        builtins,
        "%fact-table-group-sum",
        std::array<LispVal, 3>{table, *zero, *value_column});
    BOOST_REQUIRE(group_sum_res.has_value());
    auto group_sum = parse_numeric_alist(*group_sum_res);
    BOOST_TEST(group_sum.at(1) == 30.0);
    BOOST_TEST(group_sum.at(2) == 6.0);

    auto term_hash_res = invoke_builtin(
        builtins,
        "term-hash",
        std::array<LispVal, 2>{table, *depth});
    BOOST_REQUIRE(term_hash_res.has_value());
    auto term_hash = eta::runtime::nanbox::ops::decode<std::int64_t>(*term_hash_res);
    BOOST_REQUIRE(term_hash.has_value());

    auto name_a = eta::runtime::make_string(heap, intern_table, "a");
    auto name_b = eta::runtime::make_string(heap, intern_table, "b");
    auto name_u = eta::runtime::make_string(heap, intern_table, "u");
    auto name_v = eta::runtime::make_string(heap, intern_table, "v");
    BOOST_REQUIRE(name_a.has_value());
    BOOST_REQUIRE(name_b.has_value());
    BOOST_REQUIRE(name_u.has_value());
    BOOST_REQUIRE(name_v.has_value());

    auto var_a = invoke_builtin(
        builtins, "logic-var/named", std::array<LispVal, 1>{*name_a});
    auto var_b = invoke_builtin(
        builtins, "logic-var/named", std::array<LispVal, 1>{*name_b});
    auto var_u = invoke_builtin(
        builtins, "logic-var/named", std::array<LispVal, 1>{*name_u});
    auto var_v = invoke_builtin(
        builtins, "logic-var/named", std::array<LispVal, 1>{*name_v});
    BOOST_REQUIRE(var_a.has_value());
    BOOST_REQUIRE(var_b.has_value());
    BOOST_REQUIRE(var_u.has_value());
    BOOST_REQUIRE(var_v.has_value());

    auto pair_functor = eta::runtime::make_symbol(intern_table, "pair");
    BOOST_REQUIRE(pair_functor.has_value());
    auto term_left = invoke_builtin(
        builtins,
        "term",
        std::array<LispVal, 3>{*pair_functor, *var_a, *var_b});
    auto term_right = invoke_builtin(
        builtins,
        "term",
        std::array<LispVal, 3>{*pair_functor, *var_u, *var_v});
    BOOST_REQUIRE(term_left.has_value());
    BOOST_REQUIRE(term_right.has_value());

    auto variant_left = invoke_builtin(
        builtins,
        "term-variant-hash",
        std::array<LispVal, 2>{*term_left, *depth});
    auto variant_right = invoke_builtin(
        builtins,
        "term-variant-hash",
        std::array<LispVal, 2>{*term_right, *depth});
    BOOST_REQUIRE(variant_left.has_value());
    BOOST_REQUIRE(variant_right.has_value());
    auto variant_left_value =
        eta::runtime::nanbox::ops::decode<std::int64_t>(*variant_left);
    auto variant_right_value =
        eta::runtime::nanbox::ops::decode<std::int64_t>(*variant_right);
    BOOST_REQUIRE(variant_left_value.has_value());
    BOOST_REQUIRE(variant_right_value.has_value());
    BOOST_TEST(*variant_left_value == *variant_right_value);

    auto list_tail = eta::runtime::make_cons(heap, *three, eta::runtime::nanbox::Nil);
    BOOST_REQUIRE(list_tail.has_value());
    auto list_mid = eta::runtime::make_cons(heap, *two, *list_tail);
    BOOST_REQUIRE(list_mid.has_value());
    auto values = eta::runtime::make_cons(heap, *one, *list_mid);
    BOOST_REQUIRE(values.has_value());

    auto doubled_tail = eta::runtime::make_cons(heap, *six, eta::runtime::nanbox::Nil);
    BOOST_REQUIRE(doubled_tail.has_value());
    auto doubled_mid = eta::runtime::make_cons(heap, *four, *doubled_tail);
    BOOST_REQUIRE(doubled_mid.has_value());
    auto doubled_values = eta::runtime::make_cons(heap, *two, *doubled_mid);
    BOOST_REQUIRE(doubled_values.has_value());

    auto mean_res = invoke_builtin(
        builtins,
        "%stats-mean",
        std::array<LispVal, 1>{*values});
    BOOST_REQUIRE(mean_res.has_value());
    auto mean_value = eta::runtime::classify_numeric(*mean_res, heap);
    BOOST_REQUIRE(mean_value.is_valid());
    BOOST_TEST(mean_value.as_double() == 2.0);

    auto correlation_res = invoke_builtin(
        builtins,
        "%stats-correlation",
        std::array<LispVal, 2>{*values, *doubled_values});
    BOOST_REQUIRE(correlation_res.has_value());
    auto correlation_value = eta::runtime::classify_numeric(*correlation_res, heap);
    BOOST_REQUIRE(correlation_value.is_valid());
    BOOST_TEST(std::abs(correlation_value.as_double() - 1.0) < 1e-12);
}

BOOST_AUTO_TEST_CASE(aad_taped_min_strict_mode_reports_nondiff_error_shape) {
    using eta::runtime::nanbox::LispVal;

    eta::runtime::memory::heap::Heap heap(1ull << 20);
    eta::runtime::memory::intern::InternTable intern_table;
    eta::runtime::vm::VM vm(heap, intern_table);

    eta::runtime::BuiltinEnvironment builtins;
    eta::runtime::register_core_primitives(builtins, heap, intern_table, &vm);

    auto strict_symbol = eta::runtime::make_symbol(intern_table, "strict");
    BOOST_REQUIRE(strict_symbol.has_value());
    auto set_policy = invoke_builtin(
        builtins, "set-aad-nondiff-policy!", std::array<LispVal, 1>{*strict_symbol});
    BOOST_REQUIRE(set_policy.has_value());

    auto tape_res = invoke_builtin(builtins, "tape-new", std::array<LispVal, 0>{});
    BOOST_REQUIRE(tape_res.has_value());
    const LispVal tape_val = *tape_res;

    auto start_res =
        invoke_builtin(builtins, "tape-start!", std::array<LispVal, 1>{tape_val});
    BOOST_REQUIRE(start_res.has_value());

    auto value = eta::runtime::make_flonum(3.0);
    BOOST_REQUIRE(value.has_value());
    auto lhs = invoke_builtin(
        builtins, "tape-var", std::array<LispVal, 2>{tape_val, *value});
    auto rhs = invoke_builtin(
        builtins, "tape-var", std::array<LispVal, 2>{tape_val, *value});
    BOOST_REQUIRE(lhs.has_value());
    BOOST_REQUIRE(rhs.has_value());

    auto min_res = invoke_builtin(
        builtins, "min", std::array<LispVal, 2>{*lhs, *rhs});
    BOOST_REQUIRE(!min_res.has_value());

    auto* vm_error = std::get_if<eta::runtime::error::VMError>(&min_res.error());
    BOOST_REQUIRE(vm_error != nullptr);
    BOOST_TEST(vm_error->tag_override == std::string(eta::runtime::ad::kTagNondiffStrict));
    BOOST_TEST(vm_error->message == "min: non-differentiable point reached in strict mode");

    auto* op_field = find_vm_error_field(*vm_error, "op");
    BOOST_REQUIRE(op_field != nullptr);
    auto* op_value = std::get_if<std::string>(&op_field->value);
    BOOST_REQUIRE(op_value != nullptr);
    BOOST_TEST(*op_value == "min");

    auto* detail_field = find_vm_error_field(*vm_error, "detail");
    BOOST_REQUIRE(detail_field != nullptr);
    auto* detail_value = std::get_if<std::string>(&detail_field->value);
    BOOST_REQUIRE(detail_value != nullptr);
    BOOST_TEST(*detail_value == "tie (a == b)");

    auto stop_res = invoke_builtin(builtins, "tape-stop!", std::array<LispVal, 0>{});
    BOOST_REQUIRE(stop_res.has_value());
}

BOOST_AUTO_TEST_CASE(aad_unary_helper_reports_no_active_tape) {
    eta::runtime::memory::heap::Heap heap(1ull << 20);
    eta::runtime::memory::intern::InternTable intern_table;
    eta::runtime::vm::VM vm(heap, intern_table);

    auto active_tape = eta::runtime::detail::aad_unary::active_tape_for_op(heap, &vm, "sin");
    BOOST_REQUIRE(!active_tape.has_value());

    auto* vm_error = std::get_if<eta::runtime::error::VMError>(&active_tape.error());
    BOOST_REQUIRE(vm_error != nullptr);
    BOOST_TEST(vm_error->tag_override == std::string(eta::runtime::ad::kTagNoActiveTape));
    BOOST_TEST(vm_error->message == "sin: no active tape");
}

BOOST_AUTO_TEST_CASE(aad_unary_helper_rejects_mismatched_and_stale_tape_refs) {
    eta::runtime::types::Tape tape;
    tape.tape_id = 9;
    tape.generation = 4;
    tape.entries.push_back({eta::runtime::types::TapeOp::Var, 0, 0, 1.0, 0.0});

    const auto mismatched_ref = eta::runtime::types::tape_ref::make(tape.tape_id + 1u, tape.generation, 0u);
    auto mismatched = eta::runtime::detail::aad_unary::validate_tape_ref_for_op(
        &tape, mismatched_ref, "sin", "arg");
    BOOST_REQUIRE(!mismatched.has_value());
    auto* mismatched_error = std::get_if<eta::runtime::error::VMError>(&mismatched.error());
    BOOST_REQUIRE(mismatched_error != nullptr);
    BOOST_TEST(mismatched_error->tag_override == std::string(eta::runtime::ad::kTagMixedTape));
    BOOST_TEST(mismatched_error->message == "sin: reference belongs to a different tape");

    const auto stale_ref = eta::runtime::types::tape_ref::make(
        tape.tape_id, eta::runtime::types::tape_ref::next_generation(tape.generation), 0u);
    auto stale = eta::runtime::detail::aad_unary::validate_tape_ref_for_op(
        &tape, stale_ref, "sin", "arg");
    BOOST_REQUIRE(!stale.has_value());
    auto* stale_error = std::get_if<eta::runtime::error::VMError>(&stale.error());
    BOOST_REQUIRE(stale_error != nullptr);
    BOOST_TEST(stale_error->tag_override == std::string(eta::runtime::ad::kTagStaleRef));
    BOOST_TEST(stale_error->message == "sin: stale TapeRef generation");
}

BOOST_AUTO_TEST_CASE(aad_unary_helper_checks_tape_ref_index_capacity) {
    eta::runtime::types::Tape tape;
    tape.tape_id = 5;
    tape.generation = 2;

    auto ref = eta::runtime::detail::aad_unary::make_tape_ref_result(
        &tape, eta::runtime::types::tape_ref::MAX_NODE_INDEX + 1u, "sin");
    BOOST_REQUIRE(!ref.has_value());

    auto* vm_error = std::get_if<eta::runtime::error::VMError>(&ref.error());
    BOOST_REQUIRE(vm_error != nullptr);
    BOOST_CHECK(vm_error->code == eta::runtime::error::RuntimeErrorCode::InternalError);
    BOOST_TEST(vm_error->message == "sin: tape node index exceeds TapeRef capacity");
}

BOOST_AUTO_TEST_CASE(aad_unary_sin_cos_exp_match_primal_op_type_and_backward_gradient) {
    using eta::runtime::nanbox::LispVal;
    using eta::runtime::nanbox::ops::payload;
    using eta::runtime::memory::heap::ObjectKind;
    using eta::runtime::types::TapeOp;

    eta::runtime::memory::heap::Heap heap(1ull << 20);
    eta::runtime::memory::intern::InternTable intern_table;
    eta::runtime::vm::VM vm(heap, intern_table);

    eta::runtime::BuiltinEnvironment builtins;
    eta::runtime::register_core_primitives(builtins, heap, intern_table, &vm);

    const std::array<LispVal, 0> no_args{};
    auto tape_res = invoke_builtin(builtins, "tape-new", no_args);
    BOOST_REQUIRE(tape_res.has_value());
    const LispVal tape_val = *tape_res;

    auto* tape = heap.try_get_as<ObjectKind::Tape, eta::runtime::types::Tape>(payload(tape_val));
    BOOST_REQUIRE(tape != nullptr);

    struct UnaryCase {
        const char* name;
        double input;
        TapeOp expected_op;
        double expected_primal;
        double expected_gradient;
    };

    const std::array<UnaryCase, 3> cases{{
        {"sin", 0.5, TapeOp::Sin, std::sin(0.5), std::cos(0.5)},
        {"cos", 0.5, TapeOp::Cos, std::cos(0.5), -std::sin(0.5)},
        {"exp", 0.5, TapeOp::Exp, std::exp(0.5), std::exp(0.5)},
    }};

    constexpr double kTol = 1e-12;
    for (const auto& test_case : cases) {
        auto clear_res = invoke_builtin(builtins, "tape-clear!", std::array<LispVal, 1>{tape_val});
        BOOST_REQUIRE(clear_res.has_value());

        auto start_res = invoke_builtin(builtins, "tape-start!", std::array<LispVal, 1>{tape_val});
        BOOST_REQUIRE(start_res.has_value());

        auto x_input = eta::runtime::make_flonum(test_case.input);
        BOOST_REQUIRE(x_input.has_value());
        auto x_res = invoke_builtin(builtins, "tape-var", std::array<LispVal, 2>{tape_val, *x_input});
        BOOST_REQUIRE(x_res.has_value());
        const LispVal x_ref = *x_res;
        BOOST_REQUIRE(eta::runtime::types::tape_ref::is_tape_ref(x_ref));

        auto z_res = invoke_builtin(builtins, test_case.name, std::array<LispVal, 1>{x_ref});
        BOOST_REQUIRE(z_res.has_value());
        const LispVal z_ref = *z_res;
        BOOST_REQUIRE(eta::runtime::types::tape_ref::is_tape_ref(z_ref));

        auto stop_res = invoke_builtin(builtins, "tape-stop!", no_args);
        BOOST_REQUIRE(stop_res.has_value());

        auto primal_res = invoke_builtin(builtins, "tape-primal", std::array<LispVal, 2>{tape_val, z_ref});
        BOOST_REQUIRE(primal_res.has_value());
        auto primal = eta::runtime::classify_numeric(*primal_res, heap);
        BOOST_REQUIRE(primal.is_valid());
        BOOST_TEST(primal.as_double() == test_case.expected_primal, boost::test_tools::tolerance(kTol));

        const auto parts = eta::runtime::types::tape_ref::decode(z_ref);
        BOOST_REQUIRE(parts.node_index < tape->entries.size());
        BOOST_CHECK(tape->entries[parts.node_index].op == test_case.expected_op);

        auto backward_res = invoke_builtin(builtins, "tape-backward!", std::array<LispVal, 2>{tape_val, z_ref});
        BOOST_REQUIRE(backward_res.has_value());

        auto grad_res = invoke_builtin(builtins, "tape-adjoint", std::array<LispVal, 2>{tape_val, x_ref});
        BOOST_REQUIRE(grad_res.has_value());
        auto gradient = eta::runtime::classify_numeric(*grad_res, heap);
        BOOST_REQUIRE(gradient.is_valid());
        BOOST_TEST(gradient.as_double() == test_case.expected_gradient, boost::test_tools::tolerance(kTol));
    }
}

BOOST_AUTO_TEST_CASE(aad_unary_domain_sensitive_ops_match_primal_op_type_and_backward_gradient) {
    using eta::runtime::nanbox::LispVal;
    using eta::runtime::nanbox::ops::payload;
    using eta::runtime::memory::heap::ObjectKind;
    using eta::runtime::types::TapeOp;

    eta::runtime::memory::heap::Heap heap(1ull << 20);
    eta::runtime::memory::intern::InternTable intern_table;
    eta::runtime::vm::VM vm(heap, intern_table);

    eta::runtime::BuiltinEnvironment builtins;
    eta::runtime::register_core_primitives(builtins, heap, intern_table, &vm);

    const std::array<LispVal, 0> no_args{};
    auto tape_res = invoke_builtin(builtins, "tape-new", no_args);
    BOOST_REQUIRE(tape_res.has_value());
    const LispVal tape_val = *tape_res;

    auto* tape = heap.try_get_as<ObjectKind::Tape, eta::runtime::types::Tape>(payload(tape_val));
    BOOST_REQUIRE(tape != nullptr);

    struct UnaryCase {
        const char* name;
        double input;
        TapeOp expected_op;
        double expected_primal;
        double expected_gradient;
    };

    const std::array<UnaryCase, 6> cases{{
        {"tan", 0.25, TapeOp::Tan, std::tan(0.25), 1.0 / (std::cos(0.25) * std::cos(0.25))},
        {"atan", 0.25, TapeOp::Atan, std::atan(0.25), 1.0 / (1.0 + 0.25 * 0.25)},
        {"log", 2.0, TapeOp::Log, std::log(2.0), 1.0 / 2.0},
        {"sqrt", 4.0, TapeOp::Sqrt, std::sqrt(4.0), 1.0 / (2.0 * std::sqrt(4.0))},
        {"asin", 0.5, TapeOp::Asin, std::asin(0.5), 1.0 / std::sqrt(1.0 - 0.5 * 0.5)},
        {"acos", 0.5, TapeOp::Acos, std::acos(0.5), -1.0 / std::sqrt(1.0 - 0.5 * 0.5)},
    }};

    constexpr double kTol = 1e-12;
    for (const auto& test_case : cases) {
        auto clear_res = invoke_builtin(builtins, "tape-clear!", std::array<LispVal, 1>{tape_val});
        BOOST_REQUIRE(clear_res.has_value());

        auto start_res = invoke_builtin(builtins, "tape-start!", std::array<LispVal, 1>{tape_val});
        BOOST_REQUIRE(start_res.has_value());

        auto x_input = eta::runtime::make_flonum(test_case.input);
        BOOST_REQUIRE(x_input.has_value());
        auto x_res = invoke_builtin(builtins, "tape-var", std::array<LispVal, 2>{tape_val, *x_input});
        BOOST_REQUIRE(x_res.has_value());
        const LispVal x_ref = *x_res;
        BOOST_REQUIRE(eta::runtime::types::tape_ref::is_tape_ref(x_ref));

        auto z_res = invoke_builtin(builtins, test_case.name, std::array<LispVal, 1>{x_ref});
        BOOST_REQUIRE(z_res.has_value());
        const LispVal z_ref = *z_res;
        BOOST_REQUIRE(eta::runtime::types::tape_ref::is_tape_ref(z_ref));

        auto stop_res = invoke_builtin(builtins, "tape-stop!", no_args);
        BOOST_REQUIRE(stop_res.has_value());

        auto primal_res = invoke_builtin(builtins, "tape-primal", std::array<LispVal, 2>{tape_val, z_ref});
        BOOST_REQUIRE(primal_res.has_value());
        auto primal = eta::runtime::classify_numeric(*primal_res, heap);
        BOOST_REQUIRE(primal.is_valid());
        BOOST_TEST(primal.as_double() == test_case.expected_primal, boost::test_tools::tolerance(kTol));

        const auto parts = eta::runtime::types::tape_ref::decode(z_ref);
        BOOST_REQUIRE(parts.node_index < tape->entries.size());
        BOOST_CHECK(tape->entries[parts.node_index].op == test_case.expected_op);

        auto backward_res = invoke_builtin(builtins, "tape-backward!", std::array<LispVal, 2>{tape_val, z_ref});
        BOOST_REQUIRE(backward_res.has_value());

        auto grad_res = invoke_builtin(builtins, "tape-adjoint", std::array<LispVal, 2>{tape_val, x_ref});
        BOOST_REQUIRE(grad_res.has_value());
        auto gradient = eta::runtime::classify_numeric(*grad_res, heap);
        BOOST_REQUIRE(gradient.is_valid());
        BOOST_TEST(gradient.as_double() == test_case.expected_gradient, boost::test_tools::tolerance(kTol));
    }
}

BOOST_AUTO_TEST_CASE(aad_unary_domain_sensitive_ops_report_domain_error_tag_and_message) {
    using eta::runtime::nanbox::LispVal;

    eta::runtime::memory::heap::Heap heap(1ull << 20);
    eta::runtime::memory::intern::InternTable intern_table;
    eta::runtime::vm::VM vm(heap, intern_table);

    eta::runtime::BuiltinEnvironment builtins;
    eta::runtime::register_core_primitives(builtins, heap, intern_table, &vm);

    auto tape_res = invoke_builtin(builtins, "tape-new", std::array<LispVal, 0>{});
    BOOST_REQUIRE(tape_res.has_value());
    const LispVal tape_val = *tape_res;

    struct DomainCase {
        const char* name;
        double input;
        const char* detail;
    };

    const std::array<DomainCase, 4> cases{{
        {"log", 0.0, "requires x > 0"},
        {"sqrt", -1.0, "requires x >= 0"},
        {"asin", 1.2, "requires -1 <= x <= 1"},
        {"acos", -1.2, "requires -1 <= x <= 1"},
    }};

    for (const auto& test_case : cases) {
        auto clear_res = invoke_builtin(builtins, "tape-clear!", std::array<LispVal, 1>{tape_val});
        BOOST_REQUIRE(clear_res.has_value());

        auto start_res = invoke_builtin(builtins, "tape-start!", std::array<LispVal, 1>{tape_val});
        BOOST_REQUIRE(start_res.has_value());

        auto x_input = eta::runtime::make_flonum(test_case.input);
        BOOST_REQUIRE(x_input.has_value());
        auto x_res = invoke_builtin(builtins, "tape-var", std::array<LispVal, 2>{tape_val, *x_input});
        BOOST_REQUIRE(x_res.has_value());

        auto op_res = invoke_builtin(builtins, test_case.name, std::array<LispVal, 1>{*x_res});
        BOOST_REQUIRE(!op_res.has_value());

        auto* vm_error = std::get_if<eta::runtime::error::VMError>(&op_res.error());
        BOOST_REQUIRE(vm_error != nullptr);
        BOOST_TEST(vm_error->tag_override == std::string(eta::runtime::ad::kTagDomain));
        BOOST_TEST(vm_error->message == std::string(test_case.name) + ": domain violation");

        auto* op_field = find_vm_error_field(*vm_error, "op");
        BOOST_REQUIRE(op_field != nullptr);
        auto* op_value = std::get_if<std::string>(&op_field->value);
        BOOST_REQUIRE(op_value != nullptr);
        BOOST_TEST(*op_value == std::string(test_case.name));

        auto* detail_field = find_vm_error_field(*vm_error, "detail");
        BOOST_REQUIRE(detail_field != nullptr);
        auto* detail_value = std::get_if<std::string>(&detail_field->value);
        BOOST_REQUIRE(detail_value != nullptr);
        BOOST_TEST(*detail_value == std::string(test_case.detail));

        auto stop_res = invoke_builtin(builtins, "tape-stop!", std::array<LispVal, 0>{});
        BOOST_REQUIRE(stop_res.has_value());
    }
}

BOOST_AUTO_TEST_CASE(aad_tape_comparison_strict_mode_reports_nondiff_error_shape) {
    using eta::runtime::nanbox::LispVal;

    eta::runtime::memory::heap::Heap heap(1ull << 20);
    eta::runtime::memory::intern::InternTable intern_table;
    eta::runtime::vm::VM vm(heap, intern_table);

    eta::runtime::BuiltinEnvironment builtins;
    eta::runtime::register_core_primitives(builtins, heap, intern_table, &vm);

    auto tape_res = invoke_builtin(builtins, "tape-new", std::array<LispVal, 0>{});
    BOOST_REQUIRE(tape_res.has_value());
    const LispVal tape_val = *tape_res;

    auto start_res = invoke_builtin(builtins, "tape-start!", std::array<LispVal, 1>{tape_val});
    BOOST_REQUIRE(start_res.has_value());

    auto x_input = eta::runtime::make_flonum(1.25);
    BOOST_REQUIRE(x_input.has_value());
    auto x_res = invoke_builtin(builtins, "tape-var", std::array<LispVal, 2>{tape_val, *x_input});
    BOOST_REQUIRE(x_res.has_value());

    auto eq_res = invoke_builtin(builtins, "=", std::array<LispVal, 2>{*x_res, *x_res});
    BOOST_REQUIRE(!eq_res.has_value());

    auto* vm_error = std::get_if<eta::runtime::error::VMError>(&eq_res.error());
    BOOST_REQUIRE(vm_error != nullptr);
    BOOST_TEST(vm_error->tag_override == std::string(eta::runtime::ad::kTagNondiffStrict));
    BOOST_TEST(vm_error->message == "=: non-differentiable point reached in strict mode");

    auto* op_field = find_vm_error_field(*vm_error, "op");
    BOOST_REQUIRE(op_field != nullptr);
    auto* op_value = std::get_if<std::string>(&op_field->value);
    BOOST_REQUIRE(op_value != nullptr);
    BOOST_TEST(*op_value == "=");

    auto* detail_field = find_vm_error_field(*vm_error, "detail");
    BOOST_REQUIRE(detail_field != nullptr);
    auto* detail_value = std::get_if<std::string>(&detail_field->value);
    BOOST_REQUIRE(detail_value != nullptr);
    BOOST_TEST(*detail_value == "comparison");

    auto stop_res = invoke_builtin(builtins, "tape-stop!", std::array<LispVal, 0>{});
    BOOST_REQUIRE(stop_res.has_value());
}

BOOST_AUTO_TEST_CASE(aad_taped_pow_strict_mode_reports_domain_error_shape) {
    using eta::runtime::nanbox::LispVal;

    eta::runtime::memory::heap::Heap heap(1ull << 20);
    eta::runtime::memory::intern::InternTable intern_table;
    eta::runtime::vm::VM vm(heap, intern_table);

    eta::runtime::BuiltinEnvironment builtins;
    eta::runtime::register_core_primitives(builtins, heap, intern_table, &vm);

    auto strict_symbol = eta::runtime::make_symbol(intern_table, "strict");
    BOOST_REQUIRE(strict_symbol.has_value());
    auto set_policy = invoke_builtin(
        builtins, "set-aad-nondiff-policy!", std::array<LispVal, 1>{*strict_symbol});
    BOOST_REQUIRE(set_policy.has_value());

    auto tape_res = invoke_builtin(builtins, "tape-new", std::array<LispVal, 0>{});
    BOOST_REQUIRE(tape_res.has_value());
    const LispVal tape_val = *tape_res;

    auto start_res = invoke_builtin(builtins, "tape-start!", std::array<LispVal, 1>{tape_val});
    BOOST_REQUIRE(start_res.has_value());

    auto zero = eta::runtime::make_flonum(0.0);
    BOOST_REQUIRE(zero.has_value());
    auto base_res = invoke_builtin(builtins, "tape-var", std::array<LispVal, 2>{tape_val, *zero});
    BOOST_REQUIRE(base_res.has_value());
    auto exponent_res = invoke_builtin(builtins, "tape-var", std::array<LispVal, 2>{tape_val, *zero});
    BOOST_REQUIRE(exponent_res.has_value());

    auto pow_res = invoke_builtin(builtins, "pow", std::array<LispVal, 2>{*base_res, *exponent_res});
    BOOST_REQUIRE(!pow_res.has_value());

    auto* vm_error = std::get_if<eta::runtime::error::VMError>(&pow_res.error());
    BOOST_REQUIRE(vm_error != nullptr);
    BOOST_TEST(vm_error->tag_override == std::string(eta::runtime::ad::kTagDomain));
    BOOST_TEST(vm_error->message == "pow: domain violation");

    auto* op_field = find_vm_error_field(*vm_error, "op");
    BOOST_REQUIRE(op_field != nullptr);
    auto* op_value = std::get_if<std::string>(&op_field->value);
    BOOST_REQUIRE(op_value != nullptr);
    BOOST_TEST(*op_value == "pow");

    auto* detail_field = find_vm_error_field(*vm_error, "detail");
    BOOST_REQUIRE(detail_field != nullptr);
    auto* detail_value = std::get_if<std::string>(&detail_field->value);
    BOOST_REQUIRE(detail_value != nullptr);
    BOOST_TEST(*detail_value == "strict mode rejects derivative at pow(0, 0)");

    auto stop_res = invoke_builtin(builtins, "tape-stop!", std::array<LispVal, 0>{});
    BOOST_REQUIRE(stop_res.has_value());
}

BOOST_AUTO_TEST_CASE(aad_nondiff_policy_primitives_roundtrip_and_validate_input) {
    using eta::runtime::nanbox::LispVal;

    eta::runtime::memory::heap::Heap heap(1ull << 20);
    eta::runtime::memory::intern::InternTable intern_table;
    eta::runtime::vm::VM vm(heap, intern_table);

    eta::runtime::BuiltinEnvironment builtins;
    eta::runtime::register_core_primitives(builtins, heap, intern_table, &vm);

    auto strict_symbol = eta::runtime::make_symbol(intern_table, "strict");
    auto zero_subgrad_symbol = eta::runtime::make_symbol(intern_table, "zero-subgrad");
    auto invalid_symbol = eta::runtime::make_symbol(intern_table, "invalid-policy");
    auto one = eta::runtime::nanbox::ops::encode(std::int64_t{1});
    BOOST_REQUIRE(strict_symbol.has_value());
    BOOST_REQUIRE(zero_subgrad_symbol.has_value());
    BOOST_REQUIRE(invalid_symbol.has_value());
    BOOST_REQUIRE(one.has_value());

    auto default_policy =
        invoke_builtin(builtins, "aad-nondiff-policy", std::array<LispVal, 0>{});
    BOOST_REQUIRE(default_policy.has_value());
    const bool default_policy_is_known =
        (*default_policy == *strict_symbol) || (*default_policy == *zero_subgrad_symbol);
    BOOST_TEST(default_policy_is_known);

    auto set_strict = invoke_builtin(
        builtins, "set-aad-nondiff-policy!", std::array<LispVal, 1>{*strict_symbol});
    BOOST_REQUIRE(set_strict.has_value());
    BOOST_TEST(*set_strict == eta::runtime::nanbox::True);

    auto strict_policy =
        invoke_builtin(builtins, "aad-nondiff-policy", std::array<LispVal, 0>{});
    BOOST_REQUIRE(strict_policy.has_value());
    BOOST_TEST(*strict_policy == *strict_symbol);

    auto set_zero_subgrad = invoke_builtin(
        builtins, "set-aad-nondiff-policy!", std::array<LispVal, 1>{*zero_subgrad_symbol});
    BOOST_REQUIRE(set_zero_subgrad.has_value());
    BOOST_TEST(*set_zero_subgrad == eta::runtime::nanbox::True);

    auto invalid_symbol_result = invoke_builtin(
        builtins, "set-aad-nondiff-policy!", std::array<LispVal, 1>{*invalid_symbol});
    BOOST_REQUIRE(!invalid_symbol_result.has_value());
    auto* invalid_symbol_error =
        std::get_if<eta::runtime::error::VMError>(&invalid_symbol_result.error());
    BOOST_REQUIRE(invalid_symbol_error != nullptr);
    BOOST_TEST(
        invalid_symbol_error->message
        == "set-aad-nondiff-policy!: expected 'strict or 'zero-subgrad");

    auto non_symbol_result = invoke_builtin(
        builtins, "set-aad-nondiff-policy!", std::array<LispVal, 1>{*one});
    BOOST_REQUIRE(!non_symbol_result.has_value());
    auto* non_symbol_error =
        std::get_if<eta::runtime::error::VMError>(&non_symbol_result.error());
    BOOST_REQUIRE(non_symbol_error != nullptr);
    BOOST_TEST(
        non_symbol_error->message
        == "set-aad-nondiff-policy!: argument must be a symbol ('strict or 'zero-subgrad)");
}

BOOST_AUTO_TEST_CASE(aad_unary_domain_sensitive_ops_preserve_singular_gradient_behavior_in_strict_mode) {
    using eta::runtime::nanbox::LispVal;

    eta::runtime::memory::heap::Heap heap(1ull << 20);
    eta::runtime::memory::intern::InternTable intern_table;
    eta::runtime::vm::VM vm(heap, intern_table);

    eta::runtime::BuiltinEnvironment builtins;
    eta::runtime::register_core_primitives(builtins, heap, intern_table, &vm);

    auto tape_res = invoke_builtin(builtins, "tape-new", std::array<LispVal, 0>{});
    BOOST_REQUIRE(tape_res.has_value());
    const LispVal tape_val = *tape_res;

    auto strict_sym = eta::runtime::make_symbol(intern_table, "strict");
    BOOST_REQUIRE(strict_sym.has_value());
    auto zero_subgrad_sym = eta::runtime::make_symbol(intern_table, "zero-subgrad");
    BOOST_REQUIRE(zero_subgrad_sym.has_value());

    struct SingularCase {
        const char* name;
        double input;
        bool positive_infinity;
    };

    const std::array<SingularCase, 3> singular_cases{{
        {"sqrt", 0.0, true},
        {"asin", 1.0, true},
        {"acos", 1.0, false},
    }};

    const std::array<LispVal, 2> policies{*strict_sym, *zero_subgrad_sym};
    for (const LispVal policy : policies) {
        auto set_policy = invoke_builtin(
            builtins, "set-aad-nondiff-policy!", std::array<LispVal, 1>{policy});
        BOOST_REQUIRE(set_policy.has_value());

        for (const auto& test_case : singular_cases) {
            auto clear_res = invoke_builtin(builtins, "tape-clear!", std::array<LispVal, 1>{tape_val});
            BOOST_REQUIRE(clear_res.has_value());

            auto start_res = invoke_builtin(builtins, "tape-start!", std::array<LispVal, 1>{tape_val});
            BOOST_REQUIRE(start_res.has_value());

            auto x_input = eta::runtime::make_flonum(test_case.input);
            BOOST_REQUIRE(x_input.has_value());
            auto x_res = invoke_builtin(builtins, "tape-var", std::array<LispVal, 2>{tape_val, *x_input});
            BOOST_REQUIRE(x_res.has_value());

            auto z_res = invoke_builtin(builtins, test_case.name, std::array<LispVal, 1>{*x_res});
            BOOST_REQUIRE(z_res.has_value());

            auto stop_res = invoke_builtin(builtins, "tape-stop!", std::array<LispVal, 0>{});
            BOOST_REQUIRE(stop_res.has_value());

            auto backward_res = invoke_builtin(
                builtins, "tape-backward!", std::array<LispVal, 2>{tape_val, *z_res});
            BOOST_REQUIRE(backward_res.has_value());

            auto grad_res = invoke_builtin(
                builtins, "tape-adjoint", std::array<LispVal, 2>{tape_val, *x_res});
            BOOST_REQUIRE(grad_res.has_value());
            auto gradient = eta::runtime::classify_numeric(*grad_res, heap);
            BOOST_REQUIRE(gradient.is_valid());
            BOOST_TEST(std::isinf(gradient.as_double()));
            BOOST_TEST((gradient.as_double() > 0.0) == test_case.positive_infinity);
        }
    }
}

BOOST_AUTO_TEST_SUITE_END()
