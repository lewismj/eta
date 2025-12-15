
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstring>

#include <eta/runtime/nanbox.h>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t sz) {
    using namespace eta::runtime::nanbox;

    if (sz >= sizeof(uint64_t)) {
        uint64_t raw_bits;
        std::memcpy(&raw_bits, data, sizeof(raw_bits));

        if (ops::is_boxed(raw_bits)) {
            Tag t = ops::tag(raw_bits);
            const uint64_t p = ops::payload(raw_bits);

            assert((p & constants::PAYLOAD_MASK) == p);
            assert(static_cast<uint8_t>(t) <= 7);
        }
    }

    //! Encode/decode int64_t (signed)
    if (sz >= sizeof(int64_t)) {
        int64_t i64;
        std::memcpy(&i64, data, sizeof(i64));

        if (const auto encoded = ops::encode(i64); encoded.has_value()) {
            //! Verify the encoded value is actually boxed as expected
            assert(ops::is_boxed(encoded.value()));
            assert(ops::tag(encoded.value()) == Tag::Fixnum);

            const auto decoded = ops::decode<int64_t>(encoded.value());
            assert(decoded.has_value());
            assert(decoded.value() == i64);

            //! Test wrong-type decode
            const auto wrong_decode = ops::decode<char32_t>(encoded.value());
            assert(!wrong_decode.has_value());
        } else {
            //! Must be out of range
            assert(i64 < constants::FIXNUM_MIN || i64 > constants::FIXNUM_MAX);
        }
    }

    //! Encode/decode uint64_t (unsigned)
    if (sz >= sizeof(uint64_t) + sizeof(int64_t)) {
        uint64_t u64;
        std::memcpy(&u64, data + sizeof(int64_t), sizeof(u64));

        if (const auto encoded = ops::encode(u64); encoded.has_value()) {
            assert(ops::is_boxed(encoded.value()));
            assert(ops::tag(encoded.value()) == Tag::Fixnum);

            const auto decoded = ops::decode<uint64_t>(encoded.value());
            assert(decoded.has_value());
            assert(decoded.value() == u64);
        } else {
            //! Must be out of range
            assert(u64 > static_cast<uint64_t>(constants::FIXNUM_MAX));
        }
    }

    //! Encode/decode smaller integer types
    if (sz >= sizeof(int32_t)) {
        int32_t i32;
        std::memcpy(&i32, data, sizeof(i32));

        const auto encoded = ops::encode(i32);
        assert(encoded.has_value()); // int32 always fits

        const auto decoded = ops::decode<int32_t>(encoded.value());
        assert(decoded.has_value());
        assert(decoded.value() == i32);
    }

    if (sz >= sizeof(int16_t)) {
        int16_t i16;
        std::memcpy(&i16, data, sizeof(i16));

        const auto encoded = ops::encode(i16);
        assert(encoded.has_value());

        const auto decoded = ops::decode<int16_t>(encoded.value());
        assert(decoded.has_value());
        assert(decoded.value() == i16);
    }

    if (sz >= sizeof(int8_t)) {
        int8_t i8;
        std::memcpy(&i8, data, sizeof(i8));

        const auto encoded = ops::encode(i8);
        assert(encoded.has_value());

        const auto decoded = ops::decode<int8_t>(encoded.value());
        assert(decoded.has_value());
        assert(decoded.value() == i8);
    }

    //! Encode/decode bool
    if (sz >= sizeof(bool)) {
        uint8_t byte_value;
        std::memcpy(&byte_value, data, sizeof(byte_value));
        
        // Normalize to valid bool values (0 or 1)
        bool b = byte_value != 0;

        const auto encoded = ops::encode(b);
        assert(encoded.has_value());

        const auto decoded = ops::decode<bool>(encoded.value());
        assert(decoded.has_value());
        assert(decoded.value() == b);
    }

    //! Encode/decode double
    if (sz >= sizeof(double)) {
        double d;
        std::memcpy(&d, data, sizeof(d));

        const auto encoded = ops::encode(d);
        assert(encoded.has_value()); // double encoding always succeeds

        const auto decoded = ops::decode<double>(encoded.value());
        assert(decoded.has_value());

        //! Ensure NaN is handled properly.
        if (std::isnan(d)) {
            assert(std::isnan(decoded.value()));
        } else if (std::isinf(d)) {
            //! Test infinity preservation
            assert(std::isinf(decoded.value()));
            assert((d > 0) == (decoded.value() > 0)); // sign matches
        } else {
            assert(decoded.value() == d);
        }

        //! Test wrong-type decode on unboxed double
        if (!ops::is_boxed(encoded.value())) {
            const auto wrong = ops::decode<int64_t>(encoded.value());
            assert(!wrong.has_value());
        }
    }

    //! Encode/decode char types
    //! Encode/decode char types
    if (sz >= sizeof(char)) {
        char c;
        std::memcpy(&c, data, sizeof(c));

        const auto encoded = ops::encode(c);
        assert(encoded.has_value());

        const auto decoded = ops::decode<char>(encoded.value());
        assert(decoded.has_value());
        assert(static_cast<unsigned char>(decoded.value()) == static_cast<unsigned char>(c));
    }

    if (sz >= sizeof(char8_t)) {
        char8_t c8;
        std::memcpy(&c8, data, sizeof(c8));

        const auto encoded = ops::encode(c8);
        assert(encoded.has_value());

        const auto decoded = ops::decode<char8_t>(encoded.value());
        assert(decoded.has_value());
        assert(decoded.value() == c8);
    }

    //! Encode/decode char32_t (Unicode)
    if (sz >= sizeof(char32_t)) {
        char32_t c32;
        std::memcpy(&c32, data, sizeof(c32));

        if (const auto encoded = ops::encode(c32); encoded.has_value()) {
            assert(ops::is_boxed(encoded.value()));
            assert(ops::tag(encoded.value()) == Tag::Char);

            const auto decoded = ops::decode<char32_t>(encoded.value());
            assert(decoded.has_value());
            assert(decoded.value() == c32);

            //! Test surrogate pair detection (0xD800-0xDFFF are invalid)
            if (c32 >= 0xD800 && c32 <= 0xDFFF) {
                // These are technically valid in the payload but invalid Unicode
                // Current implementation doesn't check for surrogates
            }
        } else {
            //! Must be out of range
            assert(c32 > constants::UNICODE_MAX);
        }
    }

    //! Test cross-type decode failures
    if (sz >= sizeof(uint64_t) * 2) {
        uint64_t raw1, raw2;
        std::memcpy(&raw1, data, sizeof(raw1));
        std::memcpy(&raw2, data + sizeof(raw1), sizeof(raw2));

        // Try to decode raw bits as wrong types
        if (ops::is_boxed(raw1)) {
            Tag t = ops::tag(raw1);

            // Attempt decode with wrong tag
            if (t == Tag::Fixnum) {
                const auto wrong = ops::decode<char32_t>(raw1);
                assert(!wrong.has_value());
            } else if (t == Tag::Char) {
                const auto wrong = ops::decode<int64_t>(raw1);
                assert(!wrong.has_value());
            }
        }
    }

    return 0;
}