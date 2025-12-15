#pragma once

#include <bit>
#include <cmath>
#include <cstdint>
#include <expected>
#include <limits>
#include <ostream>

/**
 *
 * @brief NaN-box memory layout
 *
 * In IEEE-754, a double is NaN if all 11 exponent bits are 1 (0x7ff << 52).
 * NaN-boxing uses these NaN bit patterns to encode other types (e.g., pointers, tagged values)
 * by setting the exponent to all 1s and using the mantissa as a payload.
 *
 * @mermaid
 * graph LR
 *   A[64-bit value] --> B[Exponent: 0x7FF]
 *   A --> C[Quiet: 1]
 *   A --> D[Marker: 1]
 *   A --> E[Tag: 3 bits]
 *   A --> F[Payload: 47 bits]
 * @endmermaid
 *
 *
 * +∞: [0][11111111111][0000000000000000000000000000000000000000000000000000]
 *      sign  exp=7FF  mantissa all zeros
 *
 * -∞: [1][11111111111][0000000000000000000000000000000000000000000000000000]
 *
 */
namespace eta::runtime::nanbox {

    using LispVal = std::uint64_t;

    enum class Tag : std::uint8_t {
        Nil,
        Char,
        Fixnum,
        String,
        Symbol,
        Nan,
        HeapObject
    };

    constexpr const char* to_string(const Tag tag) {
        using enum Tag;
        switch (tag) {
            case Nil: return "Tag::Nil";
            case Char: return "Tag::Char";
            case Fixnum: return "Tag::Fixnum";
            case String: return "Tag::String";
            case Symbol: return "Tag::Symbol";
            case Nan: return "Tag::Nan";
            case HeapObject: return "Tag::HeapObject";
            default:
                return "Tag::Unknown";
        }
    }

    inline std::ostream& operator<<(std::ostream& os, const Tag tag) {
        return os << to_string(tag);
    }

    enum class NaNBoxError : std::uint8_t {
        InvalidTag,
        OutOfRange
    };

    inline const char* to_string(const NaNBoxError error) {
        using enum NaNBoxError;
        switch (error) {
            case InvalidTag: return "NaNBoxError::InvalidTag";
            case OutOfRange: return "NaNBoxError::OutOfRange";
            default:
                return "NaNBoxError::Unknown";
        }
    }

    inline std::ostream& operator<<(std::ostream& os, const NaNBoxError error) {
        return os << to_string(error);
    }

    namespace constants {
        //! After shifting right by 52, this isolates the exponent field.
        //! [00000000000000000000000000000000000000000000000000000][11111111111] (after >> 52)
        constexpr std::uint64_t EXPONENT_MASK = 0x7ffull;
        constexpr std::uint64_t EXPONENT_SHIFT = 52;

        //! 52 lowest bits set to 1, No exponent or sign bits set.
        //! [0][00000000000][1111111111111111111111111111111111111111111111111111]
        constexpr std::uint64_t MANTISSA_MASK = 0x000fffffffffffffull;

        //! Exponent all ones (NaN or Inf), mantissa zero.
        //! [0][11111111111][0000000000000000000000000000000000000000000000000000]
        constexpr std::uint64_t QNAN_EXP_BITS = 0x7ffull << 52;

        //! Bit 51 is the most significant mantissa bit — setting it indicates a quiet NaN (QNaN).
        //! [0][00000000000][1000000000000000000000000000000000000000000000000000]
        constexpr std::uint64_t QNAN_BIT = 1ull << 51;

        //! Bit 50 is the next bit down from the QNaN bit. Marker bit of NaN-boxing.
        //! [0][00000000000][0100000000000000000000000000000000000000000000000000]
        constexpr std::uint64_t MARKER_BIT = 1ull << 50;
        constexpr std::uint64_t TAG_SHIFT = 47;

        //! Usage (value >> TAG_SHIFT) & TAG_MASK to extract the 3-bit
        //! [0000000000000000000000000000000000000000000000000000000000000][111] (mask for bits at position 47-49)
        constexpr std::uint64_t TAG_MASK = 0x07ull;

        //! Corresponds to all mantissa bits except the top 3 bits (reserved for QNAN, MARKER, TAG).
        //! [0][00000000000][0000111111111111111111111111111111111111111111111111]
        constexpr std::uint64_t PAYLOAD_MASK = 0x00007fffffffffffull;

        //! Exponent = all 1s, mantissa MSB = 1 → quiet NaN bit set.
        //! All other mantissa bits = 0. Quiet NaN pattern.
        //! [0][11111111111][1000000000000000000000000000000000000000000000000000]
        constexpr std::uint64_t QUIET_NAN_BASE = QNAN_EXP_BITS | QNAN_BIT;

        //! Exponent = 0x7FF (all ones)
        //! Mantissa bits 51 and 50 set → 11... at the top of the mantissa.
        //! [0][11111111111][1100000000000000000000000000000000000000000000000000]
        constexpr std::uint64_t BOXED_PATTERN_MASK = QNAN_EXP_BITS | QNAN_BIT | MARKER_BIT;

        //! 47-bit signed integer range for Fixnums.

        //! Minimum value for Fixnum (-2^46),  -70,368,744,177,664
        constexpr int64_t FIXNUM_MIN = -(1LL << 46);

        //! Maximum value for Fixnum (2^46 - 1),  70,368,744,177,663
        constexpr int64_t FIXNUM_MAX = (1LL << 46) - 1;

        //! Maximum valid Unicode code point (U+10FFFF)
        constexpr uint32_t UNICODE_MAX = 0x10FFFFu;

        //! Sign bit for Fixnum (bit 46)
        constexpr uint64_t FIXNUM_SIGN_BIT = 1ull << 46;

        //! Mask to sign-extend a 47-bit Fixnum to full 64 bits.
        constexpr uint64_t FIXNUM_SIGN_EXTEND_MASK = ~PAYLOAD_MASK;
    }

    namespace ops {
        using namespace constants;

        constexpr LispVal box(Tag tag, const LispVal& payload) {
            return BOXED_PATTERN_MASK | static_cast<uint64_t>(tag) << TAG_SHIFT | payload & PAYLOAD_MASK;
        }

        constexpr bool is_boxed(const uint64_t bits) {
            return (bits & BOXED_PATTERN_MASK) == BOXED_PATTERN_MASK;
        }

        constexpr Tag tag(const LispVal value) {
            return static_cast<Tag>(value >> TAG_SHIFT & TAG_MASK);
        }

        constexpr uint64_t payload(const LispVal value) {
            return value & PAYLOAD_MASK;
        }

        template<typename T>
        consteval Tag get_type_tag() {
            if constexpr (std::is_same_v<T, char> ||
                          std::is_same_v<T, signed char> ||
                          std::is_same_v<T, unsigned char> ||
                          std::is_same_v<T, char8_t> ||
                          std::is_same_v<T, char32_t>)
                return Tag::Char;
            else if constexpr (std::is_same_v<T, double>)
                return Tag::Nan;
            else if constexpr (std::is_integral_v<T>)
                return Tag::Fixnum;
            else
                return Tag::Nil;
        }

        template<typename T>
        inline constexpr Tag type_tag_v = get_type_tag<T>();

        template <typename T>
        concept Encodable =
            std::is_integral_v<T> || std::is_same_v<T, bool> ||
            std::is_same_v<T, char> || std::is_same_v<T, signed char> ||
            std::is_same_v<T, unsigned char> || std::is_same_v<T, char8_t> ||
            std::is_same_v<T, char32_t> || std::is_same_v<T, double>;


        template<Encodable T>
        constexpr std::expected<LispVal, NaNBoxError> encode(const T& value) {
            if constexpr (std::is_same_v<T, double>) {
                //! Normalize all NaN variants to canonical boxed NaN
                return std::isnan(value)
                    ? box(Tag::Nan, 0)
                    : std::bit_cast<uint64_t>(value);
            }
            else if constexpr (std::is_same_v<T, char> ||
                std::is_same_v<T, signed char> ||
                std::is_same_v<T, unsigned char> ||
                std::is_same_v<T, char8_t>) {
                const uint32_t codepoint = static_cast<unsigned char>(value);
                return box(Tag::Char, codepoint);
            }
            else if constexpr (std::is_same_v<T, char32_t>) {
                const auto codepoint = static_cast<uint32_t>(value);
                if (codepoint > UNICODE_MAX)
                    return std::unexpected(NaNBoxError::OutOfRange);
                return box(Tag::Char, codepoint);
            }
            else if constexpr (std::is_integral_v<T> && sizeof(T) == 8) {
                if constexpr (std::is_signed_v<T>) {
                    if (value < FIXNUM_MIN || value > FIXNUM_MAX)
                        return std::unexpected(NaNBoxError::OutOfRange);
                    const uint64_t payload = static_cast<uint64_t>(static_cast<int64_t>(value)) & PAYLOAD_MASK;
                    return box(type_tag_v<T>, payload);
                }
                else {
                    if (value > static_cast<uint64_t>(FIXNUM_MAX))
                        return std::unexpected(NaNBoxError::OutOfRange);
                    const uint64_t payload = static_cast<uint64_t>(value) & PAYLOAD_MASK;
                    return box(type_tag_v<T>, payload);
                }
            }
            else {
                return box(type_tag_v<T>, static_cast<uint64_t>(value));
            }
        }

        template<Encodable T>
        constexpr std::expected<T, NaNBoxError> decode(LispVal value) {
            if constexpr (std::is_same_v<T, double>) {
                if (is_boxed(value)) {
                    if (tag(value) == Tag::Nan)
                        return std::numeric_limits<double>::quiet_NaN();
                    return std::unexpected(NaNBoxError::InvalidTag);
                }
                return std::bit_cast<double>(value);
            }
            else {
                if (!is_boxed(value) || tag(value) != type_tag_v<T>)
                    return std::unexpected(NaNBoxError::InvalidTag);

                if constexpr (std::is_same_v<T, char> ||
                    std::is_same_v<T, signed char> ||
                    std::is_same_v<T, unsigned char> ||
                    std::is_same_v<T, char8_t>) {
                    const auto codepoint = static_cast<uint32_t>(payload(value) & 0xFFu);
                    return static_cast<T>(static_cast<unsigned char>(codepoint));
                }
                else if constexpr (std::is_same_v<T, char32_t>) {
                    return static_cast<char32_t>(payload(value));
                }
                else if constexpr (std::is_integral_v<T> && sizeof(T) == 8 && std::is_signed_v<T>) {
                    const uint64_t raw_payload = payload(value);
                    const int64_t signed_value = raw_payload & FIXNUM_SIGN_BIT
                        ? static_cast<int64_t>(raw_payload | FIXNUM_SIGN_EXTEND_MASK)
                        : static_cast<int64_t>(raw_payload);
                    return static_cast<T>(signed_value);
                }
                else {
                    return static_cast<T>(payload(value));
                }
            }
        }
    }

   constexpr LispVal Nil   = ops::box(Tag::Nil, 0);
   constexpr LispVal False = Nil;

}