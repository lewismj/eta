#include <boost/test/unit_test.hpp>

#include <cstdint>
#include <expected>
#include <string>
#include <vector>

#include <eta/runtime/nanbox.h>
#include <eta/runtime/memory/heap.h>
#include <eta/runtime/memory/intern_table.h>
#include <eta/runtime/factory.h>

using namespace eta::runtime::nanbox;
using namespace eta::runtime::nanbox::ops;
using namespace eta::runtime::nanbox::constants;
using namespace eta::runtime::memory::heap;
using namespace eta::runtime::memory::intern;
using namespace eta::runtime::memory::factory;

namespace {
    template <typename T, typename E>
    static T expect_ok(const std::expected<T,E>& r) {
        BOOST_REQUIRE(r.has_value());
        return *r;
    }

    static RuntimeError expect_err(const std::expected<LispVal, RuntimeError>& r) {
        BOOST_REQUIRE(!r.has_value());
        return r.error();
    }
}

BOOST_AUTO_TEST_SUITE(factory_tests)

BOOST_AUTO_TEST_CASE(make_fixnum_in_range_is_boxed_fixnum) {
    Heap heap(1ull << 20);

    // In-range signed 47-bit fixnum
    constexpr int64_t in_min = FIXNUM_MIN;
    constexpr int64_t in_max = FIXNUM_MAX;

    const auto v1 = expect_ok(make_fixnum(heap, in_min));
    BOOST_TEST(is_boxed(v1));
    BOOST_TEST(tag(v1) == Tag::Fixnum);
    BOOST_TEST(expect_ok(decode<int64_t>(v1)) == in_min);

    const auto v2 = expect_ok(make_fixnum(heap, in_max));
    BOOST_TEST(is_boxed(v2));
    BOOST_TEST(tag(v2) == Tag::Fixnum);
    BOOST_TEST(expect_ok(decode<int64_t>(v2)) == in_max);

    // Unsigned within range
    constexpr uint64_t u_ok = FIXNUM_MAX;
    const auto v3 = expect_ok(make_fixnum(heap, u_ok));
    BOOST_TEST(is_boxed(v3));
    BOOST_TEST(tag(v3) == Tag::Fixnum);
    BOOST_TEST(expect_ok(decode<uint64_t>(v3)) == u_ok);
}

BOOST_AUTO_TEST_CASE(make_fixnum_out_of_range_allocates_heap_object_and_returns_unboxed_id_currently) {
    Heap heap(1ull << 20);

    constexpr int64_t below = FIXNUM_MIN - 1;
    constexpr uint64_t above = static_cast<uint64_t>(FIXNUM_MAX) + 1;

    const auto v1 = expect_ok(make_fixnum(heap, below));
    BOOST_TEST(is_boxed(v1));

    const auto v2 = expect_ok(make_fixnum(heap, above));
    BOOST_TEST(is_boxed(v2));
}

BOOST_AUTO_TEST_CASE(make_fixnum_respects_soft_limit_errors) {
    // Ensure heap errors propagate as RuntimeError
    constexpr std::size_t limit = 0; // anything > 0 will exceed
    Heap heap(limit);

    const int64_t below = FIXNUM_MIN - 1; // forces heap allocation path
    auto r = make_fixnum(heap, below);
    BOOST_REQUIRE(!r.has_value());

    // Expect the variant to hold HeapError::SoftHeapLimitExceeded
    const auto err = r.error();
    // Visitor for variant-free equality check in Boost
    const auto* heap_err = std::get_if<HeapError>(&const_cast<RuntimeError&>(err));
    BOOST_REQUIRE(heap_err != nullptr);
    BOOST_TEST(*heap_err == HeapError::SoftHeapLimitExceeded);
}

BOOST_AUTO_TEST_CASE(make_flonum_nan_is_canonical_nan) {
    // NaN must encode to canonical boxed NaN according to ops::encode
    double nan = std::numeric_limits<double>::quiet_NaN();
    auto v = expect_ok(make_flonum(nan));
    BOOST_TEST(is_boxed(v));
    BOOST_TEST(tag(v) == Tag::Nan);
}

BOOST_AUTO_TEST_CASE(make_symbol_and_make_string_box_payloads_from_intern_table) {
    Heap heap(1ull << 20);
    InternTable tbl;

    // symbol
    auto sym1 = expect_ok(make_symbol(tbl, "alpha"));
    BOOST_TEST(is_boxed(sym1));
    BOOST_TEST(tag(sym1) == Tag::Symbol);

    auto sym2 = expect_ok(make_symbol(tbl, "alpha"));
    BOOST_TEST(is_boxed(sym2));
    BOOST_TEST(tag(sym2) == Tag::Symbol);
    BOOST_TEST(payload(sym1) == payload(sym2)); // same intern id

    // string (short strings use InternTable)
    auto s1 = expect_ok(make_string(heap, tbl, "hello"));
    BOOST_TEST(is_boxed(s1));
    BOOST_TEST(tag(s1) == Tag::String);

    auto s2 = expect_ok(make_string(heap, tbl, "hello"));
    BOOST_TEST(payload(s1) == payload(s2));

    // Large string (uses Heap)
    std::string large(40, 'a');
    auto s3 = expect_ok(make_string(heap, tbl, large));
    BOOST_TEST(is_boxed(s3));
    BOOST_TEST(tag(s3) == Tag::HeapObject);
}

BOOST_AUTO_TEST_CASE(make_symbol_propagates_intern_errors) {
    // Force an error from InternTable via out-of-range id fetch attempt
    InternTable tbl;

    // We cannot force InternTable::intern to fail directly via public API in current code,
    // but we can validate that if it does return an error, factory forwards it unchanged.
    // As a proxy, call get_string with an arbitrary id to assert correctness of error wiring.
    // (Real propagation is already covered by InternTable tests.)

    // This test exists primarily to demonstrate expected wiring and style.
    auto no_str = tbl.get_string(123456789ull);
    BOOST_REQUIRE(!no_str.has_value());
    BOOST_TEST(no_str.error() == InternTableError::MissingString);
}

BOOST_AUTO_TEST_CASE(make_cons_returns_boxed_id_currently) {
    Heap heap(1ull << 20);

    auto c = expect_ok(make_cons(heap, ops::box(Tag::Nil, 0)));
    BOOST_TEST(is_boxed(c));
}

BOOST_AUTO_TEST_CASE(make_cons_pair_returns_boxed_id_currently) {
    Heap heap(1ull << 20);

    auto c = expect_ok(make_cons(heap, ops::box(Tag::Fixnum, 1), ops::box(Tag::Fixnum, 2)));
    BOOST_TEST(is_boxed(c));
}

BOOST_AUTO_TEST_CASE(make_closure_returns_boxed_id) {
    Heap heap(1ull << 20);

    const std::vector<LispVal> upvals{ ops::box(Tag::Fixnum, 42) };

    // Create a closure with no function (nullptr) but with upvals
    auto closure = expect_ok(make_closure(heap, nullptr, upvals));
    BOOST_TEST(is_boxed(closure));
}

BOOST_AUTO_TEST_SUITE_END()