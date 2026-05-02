#include <boost/test/unit_test.hpp>

#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <optional>
#include <string>
#include <vector>

#include <eta/runtime/builtin_env.h>
#include <eta/runtime/error.h>
#include <eta/runtime/memory/heap.h>
#include <eta/runtime/memory/intern_table.h>
#include <eta/runtime/nanbox.h>
#include <eta/runtime/numeric_value.h>
#include <eta/runtime/string_view.h>
#include <eta/runtime/time_primitives.h>
#include <eta/runtime/types/types.h>

using namespace eta::runtime;
using namespace eta::runtime::nanbox;
using namespace eta::runtime::memory::heap;
using namespace eta::runtime::memory::intern;

namespace {

std::expected<LispVal, error::RuntimeError> call_builtin(
    BuiltinEnvironment& env,
    const std::string& name,
    const std::vector<LispVal>& args)
{
    auto idx = env.lookup(name);
    if (!idx) {
        return std::unexpected(error::RuntimeError{
            error::VMError{error::RuntimeErrorCode::InternalError, "missing builtin: " + name}});
    }
    return env.specs()[*idx].func(args);
}

void expect_type_error(const std::expected<LispVal, error::RuntimeError>& result) {
    BOOST_REQUIRE(!result.has_value());
    auto* vm_error = std::get_if<error::VMError>(&result.error());
    BOOST_REQUIRE(vm_error != nullptr);
    BOOST_TEST(static_cast<int>(vm_error->code) ==
               static_cast<int>(error::RuntimeErrorCode::TypeError));
}

std::int64_t decode_int(Heap& heap, LispVal value) {
    auto n = classify_numeric(value, heap);
    BOOST_REQUIRE(n.is_valid());
    BOOST_REQUIRE(n.is_fixnum());
    return n.int_val;
}

std::string decode_string(InternTable& intern_table, LispVal value) {
    auto sv = StringView::try_from(value, intern_table);
    BOOST_REQUIRE(sv.has_value());
    return std::string(sv->view());
}

std::optional<LispVal> alist_lookup(
    Heap& heap,
    InternTable& intern_table,
    LispVal alist,
    std::string_view key)
{
    LispVal cur = alist;
    while (cur != Nil) {
        if (!ops::is_boxed(cur) || ops::tag(cur) != Tag::HeapObject) return std::nullopt;
        auto* outer = heap.try_get_as<ObjectKind::Cons, types::Cons>(ops::payload(cur));
        if (!outer) return std::nullopt;

        LispVal pair = outer->car;
        if (!ops::is_boxed(pair) || ops::tag(pair) != Tag::HeapObject) return std::nullopt;
        auto* inner = heap.try_get_as<ObjectKind::Cons, types::Cons>(ops::payload(pair));
        if (!inner) return std::nullopt;

        LispVal key_val = inner->car;
        if (ops::is_boxed(key_val) && ops::tag(key_val) == Tag::Symbol) {
            auto interned = intern_table.get_string(ops::payload(key_val));
            if (interned && *interned == key) return inner->cdr;
        }

        cur = outer->cdr;
    }
    return std::nullopt;
}

} // namespace

BOOST_AUTO_TEST_SUITE(time_primitives_tests)

BOOST_AUTO_TEST_CASE(registers_expected_builtins) {
    Heap heap(1ull << 22);
    InternTable intern_table;
    BuiltinEnvironment env;
    register_time_primitives(env, heap, intern_table, nullptr);

    BOOST_TEST(env.lookup("%time-now-ms").has_value());
    BOOST_TEST(env.lookup("%time-now-us").has_value());
    BOOST_TEST(env.lookup("%time-now-ns").has_value());
    BOOST_TEST(env.lookup("%time-monotonic-ms").has_value());
    BOOST_TEST(env.lookup("%time-sleep-ms").has_value());
    BOOST_TEST(env.lookup("%time-utc-parts").has_value());
    BOOST_TEST(env.lookup("%time-local-parts").has_value());
    BOOST_TEST(env.lookup("%time-format-iso8601-utc").has_value());
    BOOST_TEST(env.lookup("%time-format-iso8601-local").has_value());
}

BOOST_AUTO_TEST_CASE(now_variants_return_numbers) {
    Heap heap(1ull << 22);
    InternTable intern_table;
    BuiltinEnvironment env;
    register_time_primitives(env, heap, intern_table, nullptr);

    auto ms = call_builtin(env, "%time-now-ms", {});
    auto us = call_builtin(env, "%time-now-us", {});
    auto ns = call_builtin(env, "%time-now-ns", {});

    BOOST_REQUIRE(ms.has_value());
    BOOST_REQUIRE(us.has_value());
    BOOST_REQUIRE(ns.has_value());

    auto ms_num = classify_numeric(*ms, heap);
    auto us_num = classify_numeric(*us, heap);
    auto ns_num = classify_numeric(*ns, heap);
    BOOST_TEST(ms_num.is_valid());
    BOOST_TEST(us_num.is_valid());
    BOOST_TEST(ns_num.is_valid());
}

BOOST_AUTO_TEST_CASE(monotonic_is_non_decreasing) {
    Heap heap(1ull << 22);
    InternTable intern_table;
    BuiltinEnvironment env;
    register_time_primitives(env, heap, intern_table, nullptr);

    auto t1 = call_builtin(env, "%time-monotonic-ms", {});
    auto t2 = call_builtin(env, "%time-monotonic-ms", {});
    auto t3 = call_builtin(env, "%time-monotonic-ms", {});

    BOOST_REQUIRE(t1.has_value());
    BOOST_REQUIRE(t2.has_value());
    BOOST_REQUIRE(t3.has_value());

    const auto a = decode_int(heap, *t1);
    const auto b = decode_int(heap, *t2);
    const auto c = decode_int(heap, *t3);

    BOOST_TEST(b >= a);
    BOOST_TEST(c >= b);
}

BOOST_AUTO_TEST_CASE(sleep_respects_lower_bound_and_validates_input) {
    Heap heap(1ull << 22);
    InternTable intern_table;
    BuiltinEnvironment env;
    register_time_primitives(env, heap, intern_table, nullptr);

    auto sleep_arg = ops::encode<std::int64_t>(30);
    BOOST_REQUIRE(sleep_arg.has_value());

    auto start = call_builtin(env, "%time-monotonic-ms", {});
    BOOST_REQUIRE(start.has_value());
    auto sleep_result = call_builtin(env, "%time-sleep-ms", {*sleep_arg});
    BOOST_REQUIRE(sleep_result.has_value());
    BOOST_TEST(*sleep_result == Nil);
    auto end = call_builtin(env, "%time-monotonic-ms", {});
    BOOST_REQUIRE(end.has_value());

    const auto elapsed = decode_int(heap, *end) - decode_int(heap, *start);
    BOOST_TEST(elapsed >= 20);

    auto neg = ops::encode<std::int64_t>(-1);
    BOOST_REQUIRE(neg.has_value());
    expect_type_error(call_builtin(env, "%time-sleep-ms", {*neg}));

    auto not_number = ops::box(Tag::Symbol, 1);
    expect_type_error(call_builtin(env, "%time-sleep-ms", {not_number}));
}

BOOST_AUTO_TEST_CASE(utc_and_local_parts_have_expected_shape) {
    Heap heap(1ull << 22);
    InternTable intern_table;
    BuiltinEnvironment env;
    register_time_primitives(env, heap, intern_table, nullptr);

    auto now = call_builtin(env, "%time-now-ms", {});
    BOOST_REQUIRE(now.has_value());

    auto utc_parts = call_builtin(env, "%time-utc-parts", {*now});
    auto local_parts = call_builtin(env, "%time-local-parts", {*now});
    BOOST_REQUIRE(utc_parts.has_value());
    BOOST_REQUIRE(local_parts.has_value());

    auto check_parts = [&](LispVal parts, bool expect_utc_offset) {
        auto year = alist_lookup(heap, intern_table, parts, "year");
        auto month = alist_lookup(heap, intern_table, parts, "month");
        auto day = alist_lookup(heap, intern_table, parts, "day");
        auto hour = alist_lookup(heap, intern_table, parts, "hour");
        auto minute = alist_lookup(heap, intern_table, parts, "minute");
        auto second = alist_lookup(heap, intern_table, parts, "second");
        auto weekday = alist_lookup(heap, intern_table, parts, "weekday");
        auto yearday = alist_lookup(heap, intern_table, parts, "yearday");
        auto is_dst = alist_lookup(heap, intern_table, parts, "is-dst");
        auto offset = alist_lookup(heap, intern_table, parts, "offset-minutes");

        BOOST_REQUIRE(year.has_value());
        BOOST_REQUIRE(month.has_value());
        BOOST_REQUIRE(day.has_value());
        BOOST_REQUIRE(hour.has_value());
        BOOST_REQUIRE(minute.has_value());
        BOOST_REQUIRE(second.has_value());
        BOOST_REQUIRE(weekday.has_value());
        BOOST_REQUIRE(yearday.has_value());
        BOOST_REQUIRE(is_dst.has_value());
        BOOST_REQUIRE(offset.has_value());

        BOOST_TEST(decode_int(heap, *year) >= 1900);
        BOOST_TEST(decode_int(heap, *month) >= 1);
        BOOST_TEST(decode_int(heap, *month) <= 12);
        BOOST_TEST(decode_int(heap, *day) >= 1);
        BOOST_TEST(decode_int(heap, *day) <= 31);
        BOOST_TEST(decode_int(heap, *hour) >= 0);
        BOOST_TEST(decode_int(heap, *hour) <= 23);
        BOOST_TEST(decode_int(heap, *minute) >= 0);
        BOOST_TEST(decode_int(heap, *minute) <= 59);
        BOOST_TEST(decode_int(heap, *second) >= 0);
        BOOST_TEST(decode_int(heap, *second) <= 60);
        BOOST_TEST(decode_int(heap, *weekday) >= 0);
        BOOST_TEST(decode_int(heap, *weekday) <= 6);
        BOOST_TEST(decode_int(heap, *yearday) >= 0);
        BOOST_TEST(decode_int(heap, *yearday) <= 365);

        const bool is_dst_boolean = (*is_dst == True) || (*is_dst == False);
        BOOST_TEST(is_dst_boolean);

        const auto offset_minutes = decode_int(heap, *offset);
        if (expect_utc_offset) {
            BOOST_TEST(offset_minutes == 0);
        } else {
            BOOST_TEST(std::llabs(offset_minutes) <= 24 * 60);
        }
    };

    check_parts(*utc_parts, true);
    check_parts(*local_parts, false);
}

BOOST_AUTO_TEST_CASE(iso8601_formatting_shape) {
    Heap heap(1ull << 22);
    InternTable intern_table;
    BuiltinEnvironment env;
    register_time_primitives(env, heap, intern_table, nullptr);

    auto zero = ops::encode<std::int64_t>(0);
    BOOST_REQUIRE(zero.has_value());

    auto utc = call_builtin(env, "%time-format-iso8601-utc", {*zero});
    auto local = call_builtin(env, "%time-format-iso8601-local", {*zero});
    BOOST_REQUIRE(utc.has_value());
    BOOST_REQUIRE(local.has_value());

    const auto utc_text = decode_string(intern_table, *utc);
    BOOST_TEST(utc_text == "1970-01-01T00:00:00Z");

    const auto local_text = decode_string(intern_table, *local);
    BOOST_TEST(local_text.size() == 25u);
    BOOST_TEST(local_text[4] == '-');
    BOOST_TEST(local_text[7] == '-');
    BOOST_TEST(local_text[10] == 'T');
    BOOST_TEST(local_text[13] == ':');
    BOOST_TEST(local_text[16] == ':');
    BOOST_TEST(local_text[22] == ':');
    const bool has_offset_sign = (local_text[19] == '+') || (local_text[19] == '-');
    BOOST_TEST(has_offset_sign);
    BOOST_TEST(std::isdigit(static_cast<unsigned char>(local_text[0])) != 0);
    BOOST_TEST(std::isdigit(static_cast<unsigned char>(local_text[1])) != 0);
    BOOST_TEST(std::isdigit(static_cast<unsigned char>(local_text[2])) != 0);
    BOOST_TEST(std::isdigit(static_cast<unsigned char>(local_text[3])) != 0);
    BOOST_TEST(std::isdigit(static_cast<unsigned char>(local_text[20])) != 0);
    BOOST_TEST(std::isdigit(static_cast<unsigned char>(local_text[21])) != 0);
    BOOST_TEST(std::isdigit(static_cast<unsigned char>(local_text[23])) != 0);
    BOOST_TEST(std::isdigit(static_cast<unsigned char>(local_text[24])) != 0);
}

BOOST_AUTO_TEST_SUITE_END()
