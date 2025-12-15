#include <cmath>
#include <cstdint>
#include <iomanip>

#include <boost/test/unit_test.hpp>

#include <eta/runtime/nanbox.h>

using namespace eta::runtime::nanbox;
using namespace eta::runtime::nanbox::constants;

namespace std {
    inline std::ostream& operator<<(std::ostream& os, char8_t c) {
        return os << "u8'" << static_cast<unsigned int>(c) << "'";
    }

    inline std::ostream& operator<<(std::ostream& os, char32_t c) {
        return os << "U'0x" << std::hex << std::setfill('0') << std::setw(4)
                  << static_cast<uint32_t>(c) << std::dec << "'";
    }
}


BOOST_AUTO_TEST_SUITE(nanbox_tests)

BOOST_AUTO_TEST_CASE(test_bool_encode_decode) {
    using namespace eta::runtime::nanbox;
    using namespace eta::runtime::nanbox::ops;

    // Test true
    {
        constexpr auto encoded = encode(true);
        BOOST_REQUIRE(encoded.has_value());
        BOOST_CHECK(is_boxed(encoded.value()));
        BOOST_CHECK_EQUAL(tag(encoded.value()), Tag::Fixnum);

        const auto decoded = decode<bool>(encoded.value());
        BOOST_REQUIRE(decoded.has_value());
        BOOST_CHECK_EQUAL(decoded.value(), true);
    }

    // Test false
    {
        constexpr auto encoded = encode(false);
        BOOST_REQUIRE(encoded.has_value());
        BOOST_CHECK(is_boxed(encoded.value()));
        BOOST_CHECK_EQUAL(tag(encoded.value()), Tag::Fixnum);

        const auto decoded = decode<bool>(encoded.value());
        BOOST_REQUIRE(decoded.has_value());
        BOOST_CHECK_EQUAL(decoded.value(), false);
    }

    // Test that true and false encode to different values
    {
        constexpr auto encoded_true = encode(true);
        constexpr auto encoded_false = encode(false);
        BOOST_REQUIRE(encoded_true.has_value());
        BOOST_REQUIRE(encoded_false.has_value());
        BOOST_CHECK_NE(encoded_true.value(), encoded_false.value());
    }

    // Test round-trip for multiple bool values
    for (bool b : {false, true}) {
        const auto encoded = encode(b);
        BOOST_REQUIRE(encoded.has_value());

        const auto decoded = decode<bool>(encoded.value());
        BOOST_REQUIRE(decoded.has_value());
        BOOST_CHECK_EQUAL(decoded.value(), b);
    }

    // Test wrong-type decode
    {
        const auto encoded = encode(true);
        BOOST_REQUIRE(encoded.has_value());

        // Try to decode as char32_t - should fail
        const auto wrong_decode = decode<char32_t>(encoded.value());
        BOOST_CHECK(!wrong_decode.has_value());
        BOOST_CHECK_EQUAL(wrong_decode.error(), NaNBoxError::InvalidTag);
    }
}



BOOST_AUTO_TEST_CASE(box_sets_top_bits_and_masks_payload) {
    constexpr LispVal raw_payload = 0xFFFFFFFFFFFFFFFFull; // beyond payload width
    constexpr LispVal v = ops::box(Tag::Fixnum, raw_payload);

    BOOST_TEST((v & BOXED_PATTERN_MASK) == BOXED_PATTERN_MASK);
    BOOST_TEST(ops::tag(v) == Tag::Fixnum);
    BOOST_TEST(ops::payload(v) == (raw_payload & PAYLOAD_MASK));
}

BOOST_AUTO_TEST_CASE(is_boxed_accepts_only_full_signature) {
    constexpr LispVal v = ops::box(Tag::Fixnum, 1);
    BOOST_TEST(ops::is_boxed(v));

    constexpr LispVal without_marker = v & ~MARKER_BIT;
    BOOST_TEST(!ops::is_boxed(without_marker));

    constexpr LispVal without_qnan = v & ~QNAN_BIT;
    BOOST_TEST(!ops::is_boxed(without_qnan));

    constexpr LispVal without_exp = v & ~QNAN_EXP_BITS;
    BOOST_TEST(!ops::is_boxed(without_exp));
}

// --- Tag extraction for each Tag value ---
BOOST_AUTO_TEST_CASE(tag_extraction_nil)       { BOOST_TEST(ops::tag(ops::box(Tag::Nil,       0)) == Tag::Nil); }
BOOST_AUTO_TEST_CASE(tag_extraction_char)      { BOOST_TEST(ops::tag(ops::box(Tag::Char,      'A')) == Tag::Char); }
BOOST_AUTO_TEST_CASE(tag_extraction_int)       { BOOST_TEST(ops::tag(ops::box(Tag::Fixnum,       42)) == Tag::Fixnum); }
BOOST_AUTO_TEST_CASE(tag_extraction_string)    { BOOST_TEST(ops::tag(ops::box(Tag::String,    0x1234)) == Tag::String); }
BOOST_AUTO_TEST_CASE(tag_extraction_symbol)    { BOOST_TEST(ops::tag(ops::box(Tag::Symbol,    0x5678)) == Tag::Symbol); }
BOOST_AUTO_TEST_CASE(tag_extraction_nan)       { BOOST_TEST(ops::tag(ops::box(Tag::Nan,       0x9ABC)) == Tag::Nan); }
BOOST_AUTO_TEST_CASE(tag_extraction_heapobj)   { BOOST_TEST(ops::tag(ops::box(Tag::HeapObject,0xDEF0)) == Tag::HeapObject); }

// --- Payload boundaries and masking ---
BOOST_AUTO_TEST_CASE(payload_zero_and_max) {
    constexpr LispVal zero = ops::box(Tag::Fixnum, 0);
    BOOST_TEST(ops::payload(zero) == 0);

    constexpr LispVal max_payload_in = PAYLOAD_MASK;
    constexpr LispVal max = ops::box(Tag::Fixnum, max_payload_in);
    BOOST_TEST(ops::payload(max) == max_payload_in);
}

BOOST_AUTO_TEST_CASE(payload_masks_high_bits) {
    constexpr LispVal beyond = (1ull << (TAG_SHIFT)) | 0x12345ull; // sets bit 47 which must be masked out
    constexpr LispVal v = ops::box(Tag::Fixnum, beyond);
    BOOST_TEST(ops::payload(v) == (beyond & PAYLOAD_MASK));
}

BOOST_AUTO_TEST_CASE(int32_positive) {
    constexpr std::int32_t i = 123456789;
    constexpr LispVal v = ops::box(Tag::Fixnum, i);
    BOOST_TEST(ops::is_boxed(v));
    BOOST_TEST(ops::tag(v) == Tag::Fixnum);
    BOOST_TEST(ops::payload(v) == static_cast<std::uint32_t>(i));
}

BOOST_AUTO_TEST_CASE(int32_negative_two_complement_bits_preserved) {
    constexpr std::int32_t i = -42;
    constexpr LispVal v = ops::box(Tag::Fixnum, static_cast<std::uint32_t>(i));
    BOOST_TEST(ops::payload(v) == static_cast<std::uint32_t>(i));
}

BOOST_AUTO_TEST_CASE(uint32_max) {
    constexpr std::uint32_t u = 0xFFFFFFFFu;
    constexpr LispVal v = ops::box(Tag::Fixnum, u);
    BOOST_TEST(ops::payload(v) == u);
}

BOOST_AUTO_TEST_CASE(uint32_various_values) {
    for (constexpr std::uint32_t values[] = {0u, 1u, 0x7FFFFFFFu, 0x80000000u, 0xFFFFFFFFu}; auto u : values) {
        const LispVal v = ops::box(Tag::Fixnum, u);
        BOOST_TEST(ops::payload(v) == u);
        BOOST_TEST(ops::tag(v) == Tag::Fixnum);
        BOOST_TEST(ops::is_boxed(v));
    }
}

BOOST_AUTO_TEST_CASE(char_ascii_A) {
    constexpr unsigned char ch = 'A';
    constexpr LispVal v = ops::box(Tag::Char, ch);
    BOOST_TEST(ops::tag(v) == Tag::Char);
    BOOST_TEST(ops::payload(v) == static_cast<LispVal>(ch));
}

BOOST_AUTO_TEST_CASE(char_high_byte) {
    constexpr unsigned char ch = 0xFEu;
    constexpr LispVal v = ops::box(Tag::Char, ch);
    BOOST_TEST(ops::payload(v) == static_cast<LispVal>(ch));
}


static LispVal as_bits(double d) {
    static_assert(sizeof(double) == sizeof(std::uint64_t));
    std::uint64_t u;
    std::memcpy(&u, &d, sizeof(u));
    return u;
}

BOOST_AUTO_TEST_CASE(double_normal_numbers_are_not_boxed) {
    for (constexpr double xs[] = {0.0, -0.0, 1.0, -2.5, 1e300, 1e-300}; const double d : xs) {
        const LispVal bits = as_bits(d);
        BOOST_TEST(!ops::is_boxed(bits));
    }
}

BOOST_AUTO_TEST_CASE(double_infinities_are_not_boxed) {
    constexpr double pos_inf = std::numeric_limits<double>::infinity();
    constexpr double neg_inf = -std::numeric_limits<double>::infinity();
    BOOST_TEST(!ops::is_boxed(as_bits(pos_inf)));
    BOOST_TEST(!ops::is_boxed(as_bits(neg_inf)));
}

BOOST_AUTO_TEST_CASE(double_quiet_nan_without_marker_not_boxed) {
    // Create a quiet NaN by setting exponent=all ones and QNAN_BIT, but NOT MARKER_BIT
    constexpr LispVal qnan_no_marker = QNAN_EXP_BITS | QNAN_BIT | 0x1234ull;
    BOOST_TEST(!ops::is_boxed(qnan_no_marker));
}

BOOST_AUTO_TEST_CASE(double_nan_with_full_signature_is_considered_boxed) {
    constexpr LispVal boxed_like = BOXED_PATTERN_MASK | 0x55AAull; // any payload
    BOOST_TEST(ops::is_boxed(boxed_like));
}

BOOST_AUTO_TEST_CASE(payload_highest_bit_inside_payload) {
    constexpr LispVal p = (1ull << 47) - 1;
    constexpr LispVal v = ops::box(Tag::Fixnum, p);
    BOOST_TEST(ops::payload(v) == p);
}

BOOST_AUTO_TEST_CASE(tag_uses_only_three_bits) {
    constexpr LispVal v = ops::box(Tag::HeapObject, PAYLOAD_MASK);
    BOOST_TEST(ops::tag(v) == Tag::HeapObject);
}

BOOST_AUTO_TEST_CASE(fixnum_encode_boundaries_ok) {
    // Boundaries are inclusive: [-2^46, 2^46-1]
    constexpr int64_t minv = FIXNUM_MIN;
    constexpr int64_t maxv = FIXNUM_MAX;

    auto rmin = ops::encode<int64_t>(minv);
    auto rmax = ops::encode<int64_t>(maxv);

    BOOST_TEST(rmin.has_value());
    BOOST_TEST(rmax.has_value());

    // Check tag and payload match
    BOOST_TEST(ops::tag(rmin.value()) == Tag::Fixnum);
    BOOST_TEST(ops::tag(rmax.value()) == Tag::Fixnum);

    // Payload should equal masked representation of the value
    BOOST_TEST(ops::payload(rmin.value()) == (static_cast<LispVal>(minv) & PAYLOAD_MASK));
    BOOST_TEST(ops::payload(rmax.value()) == static_cast<LispVal>(maxv));
}

BOOST_AUTO_TEST_CASE(fixnum_encode_int64_out_of_range_errors) {
    // Values that require more than 47 bits should error
    constexpr int64_t below_min = FIXNUM_MIN - 1;     // -2^46 - 1
    constexpr int64_t above_max = FIXNUM_MAX + 1;     //  2^46

    auto rlow = ops::encode<int64_t>(below_min);
    auto rhigh = ops::encode<int64_t>(above_max);

    BOOST_TEST(!rlow.has_value());
    BOOST_TEST(!rhigh.has_value());

    BOOST_TEST(rlow.error() == NaNBoxError::OutOfRange);
    BOOST_TEST(rhigh.error() == NaNBoxError::OutOfRange);
}

BOOST_AUTO_TEST_CASE(fixnum_encode_uint64_out_of_range_errors) {
    constexpr uint64_t above_max = static_cast<uint64_t>(FIXNUM_MAX) + 1ull; // 2^46
    constexpr uint64_t huge = std::numeric_limits<uint64_t>::max();

    auto r1 = ops::encode<uint64_t>(above_max);
    auto r2 = ops::encode<uint64_t>(huge);

    BOOST_TEST(!r1.has_value());
    BOOST_TEST(!r2.has_value());

    BOOST_TEST(r1.error() == NaNBoxError::OutOfRange);
    BOOST_TEST(r2.error() == NaNBoxError::OutOfRange);
}

BOOST_AUTO_TEST_CASE(dangerous_nan_patterns_are_canonicalized) {
    // Construct a NaN that looks like a boxed value
    constexpr uint64_t dangerous_nan = BOXED_PATTERN_MASK | 0x1234ull;
    const auto d = std::bit_cast<double>(dangerous_nan);

    BOOST_TEST(std::isnan(d)); // Verify it's actually a NaN

    // Encoding should canonicalize it
    auto result = ops::encode(d);
    BOOST_TEST(result.has_value());
    BOOST_TEST(ops::is_boxed(result.value()));
    BOOST_TEST(ops::tag(result.value()) == Tag::Nan);

    // It should NOT be stored as the raw dangerous pattern
    BOOST_TEST(result.value() != dangerous_nan);
}

BOOST_AUTO_TEST_CASE(char_roundtrip_all_bytes) {
    // Test all possible byte values (0-255)
    for (int i = 0; i < 256; ++i) {
        char ch = static_cast<char>(i);
        auto encoded = ops::encode(ch);
        BOOST_TEST(encoded.has_value());
        BOOST_TEST(ops::tag(encoded.value()) == Tag::Char);

        auto decoded = ops::decode<char>(encoded.value());
        BOOST_TEST(decoded.has_value());

        // Bit pattern should be preserved
        BOOST_TEST(static_cast<unsigned char>(decoded.value()) == static_cast<unsigned char>(ch));
    }
}

BOOST_AUTO_TEST_CASE(unsigned_char_roundtrip) {
    for (int i = 0; i < 256; ++i) {
        auto uch = static_cast<unsigned char>(i);
        auto encoded = ops::encode(uch);
        BOOST_TEST(encoded.has_value());

        auto decoded = ops::decode<unsigned char>(encoded.value());
        BOOST_TEST(decoded.has_value());
        BOOST_TEST(decoded.value() == uch);
    }
}

BOOST_AUTO_TEST_CASE(signed_char_treated_as_unsigned) {
    // Negative signed chars should roundtrip preserving bit pattern
    const signed char neg = -1;  // 0xFF in two's complement
    const auto encoded = ops::encode(neg);
    BOOST_TEST(encoded.has_value());

    auto decoded = ops::decode<signed char>(encoded.value());
    BOOST_TEST(decoded.has_value());

    // Bit pattern preserved (both are 0xFF)
    BOOST_TEST(static_cast<unsigned char>(decoded.value()) == 0xFF);
}

BOOST_AUTO_TEST_CASE(char8_t_utf8_code_units) {
    // UTF-8 code units are 0-255
    for (int i = 0; i < 256; ++i) {
        auto c8 = static_cast<char8_t>(i);
        auto encoded = ops::encode(c8);
        BOOST_TEST(encoded.has_value());
        BOOST_TEST(ops::tag(encoded.value()) == Tag::Char);

        auto decoded = ops::decode<char8_t>(encoded.value());
        BOOST_TEST(decoded.has_value());
        BOOST_TEST(decoded.value() == c8);
    }
}

BOOST_AUTO_TEST_CASE(char32_t_unicode_codepoints) {
    // Test various Unicode code points
    constexpr char32_t test_codepoints[] = {
        U'\0',        // NULL
        U'A',         // ASCII
        U'\u00E9',    // é (Latin-1 Supplement)
        U'\u4E2D',    // 中 (CJK)
        U'\U0001F600', // 😀 (Emoji)
        0x10FFFF      // Maximum valid Unicode
    };

    for (char32_t cp : test_codepoints) {
        auto encoded = ops::encode(cp);
        BOOST_TEST(encoded.has_value());
        BOOST_TEST(ops::tag(encoded.value()) == Tag::Char);

        auto decoded = ops::decode<char32_t>(encoded.value());
        BOOST_TEST(decoded.has_value());
        BOOST_TEST(decoded.value() == cp);
    }
}

BOOST_AUTO_TEST_CASE(char32_t_invalid_codepoint_rejected) {
    // Code points beyond U+10FFFF are invalid
    char32_t invalid = 0x110000;
    auto result = ops::encode(invalid);
    BOOST_TEST(!result.has_value());
    BOOST_TEST(result.error() == NaNBoxError::OutOfRange);

    // Also test very large values
    invalid = 0xFFFFFFFF;
    result = ops::encode(invalid);
    BOOST_TEST(!result.has_value());
    BOOST_TEST(result.error() == NaNBoxError::OutOfRange);
}

BOOST_AUTO_TEST_CASE(char_types_interoperable) {
    // Different char types with same value should produce same payload
    unsigned char uc = 'A';
    char c = 'A';
    char8_t c8 = u8'A';

    auto encoded_uc = ops::encode(uc);
    auto encoded_c = ops::encode(c);
    auto encoded_c8 = ops::encode(c8);

    BOOST_TEST(ops::payload(encoded_uc.value()) == ops::payload(encoded_c.value()));
    BOOST_TEST(ops::payload(encoded_c.value()) == ops::payload(encoded_c8.value()));

    // Should be decodable as any 8-bit char type
    auto decoded_as_uc = ops::decode<unsigned char>(encoded_c.value());
    BOOST_TEST(decoded_as_uc.has_value());
    BOOST_TEST(decoded_as_uc.value() == 'A');
}

BOOST_AUTO_TEST_SUITE_END()