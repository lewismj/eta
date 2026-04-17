#pragma once

#include <cctype>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace eta::reader::lexer {

/**
 * @brief Unified numeric parsing utilities
 *
 * This module centralizes all numeric parsing logic to avoid duplication
 * across collect_number_decimal, collect_number_radix, is_valid_decimal,
 * is_signed_integer, etc.
 */
namespace numeric {

/**
 * @brief Result of parsing a numeric literal
 */
struct NumericParseResult {
    enum class Kind : std::uint8_t {
        Invalid,
        Fixnum,      ///< Integer (any radix)
        Flonum,      ///< Floating-point (decimal only)
        SpecialFloat ///< +inf.0, -inf.0, +nan.0, -nan.0
    };

    Kind kind{Kind::Invalid};
    std::string text;           ///< Original text representation
    std::uint8_t radix{10};     ///< 2, 8, 10, or 16
    std::string error_message;  ///< Non-empty if kind == Invalid
};

/**
 * @brief Check if a character is a valid digit for the given radix
 */
inline bool is_valid_digit(char c, std::uint8_t radix) noexcept {
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    switch (radix) {
        case 2:  return c == '0' || c == '1';
        case 8:  return c >= '0' && c <= '7';
        case 10: return c >= '0' && c <= '9';
        case 16: return static_cast<bool>(std::isxdigit(static_cast<unsigned char>(c)));
        default: return false;
    }
}

/**
 * @brief Check if a character is a sign (+/-)
 */
inline bool is_sign(char c) noexcept {
    return c == '+' || c == '-';
}

/**
 * @brief Check if string matches a special IEEE literal (case-insensitive)
 */
inline bool is_special_float(std::string_view s) noexcept {
    if (s.size() < 5) return false;

    /// Normalize: skip optional sign
    std::size_t start = 0;
    if (s[0] == '+' || s[0] == '-') start = 1;

    if (s.size() - start != 5) return false;

    /// Case-insensitive comparison
    std::string lower;
    for (std::size_t i = start; i < s.size(); ++i) {
        lower.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(s[i]))));
    }

    return lower == "inf.0" || lower == "nan.0";
}

/**
 * @brief Unified numeric parser
 *
 * Parses a numeric string with the given radix, handling:
 * - Optional leading sign
 * - Integer (fixnum) in any radix
 * - Floating-point (flonum) in decimal only
 * - Special IEEE values (+inf.0, -inf.0, +nan.0, -nan.0)
 */
class NumericParser {
public:
    explicit NumericParser(std::string_view input, std::uint8_t radix = 10)
        : input_(input), radix_(radix), pos_(0) {}

    NumericParseResult parse() {
        NumericParseResult result;
        result.radix = radix_;

        if (input_.empty()) {
            result.kind = NumericParseResult::Kind::Invalid;
            result.error_message = "empty input";
            return result;
        }

        /// Check for special IEEE literals first (decimal only)
        if (radix_ == 10 && is_special_float(input_)) {
            result.kind = NumericParseResult::Kind::SpecialFloat;
            result.text = std::string(input_);
            return result;
        }

        /// Parse optional sign
        std::string text;
        if (pos_ < input_.size() && is_sign(input_[pos_])) {
            text.push_back(input_[pos_++]);
        }

        if (pos_ >= input_.size()) {
            result.kind = NumericParseResult::Kind::Invalid;
            result.error_message = "missing digits after sign";
            return result;
        }

        /// Collect digits based on radix
        if (radix_ == 10) {
            return parse_decimal(std::move(text));
        } else {
            return parse_integer(std::move(text));
        }
    }

private:
    NumericParseResult parse_decimal(std::string text) {
        NumericParseResult result;
        result.radix = 10;

        bool has_digits = false;
        bool has_dot = false;
        bool has_exponent = false;

        /// Integer part
        while (pos_ < input_.size() && std::isdigit(static_cast<unsigned char>(input_[pos_]))) {
            text.push_back(input_[pos_++]);
            has_digits = true;
        }

        /// Fractional part
        if (pos_ < input_.size() && input_[pos_] == '.') {
            text.push_back(input_[pos_++]);
            has_dot = true;

            while (pos_ < input_.size() && std::isdigit(static_cast<unsigned char>(input_[pos_]))) {
                text.push_back(input_[pos_++]);
                has_digits = true;
            }
        }

        /// Exponent part
        if (pos_ < input_.size() && (input_[pos_] == 'e' || input_[pos_] == 'E')) {
            text.push_back(input_[pos_++]);
            has_exponent = true;

            /// Optional exponent sign
            if (pos_ < input_.size() && is_sign(input_[pos_])) {
                text.push_back(input_[pos_++]);
            }

            /// Exponent digits (required)
            bool has_exp_digits = false;
            while (pos_ < input_.size() && std::isdigit(static_cast<unsigned char>(input_[pos_]))) {
                text.push_back(input_[pos_++]);
                has_exp_digits = true;
            }

            if (!has_exp_digits) {
                result.kind = NumericParseResult::Kind::Invalid;
                result.error_message = "exponent requires digits";
                return result;
            }
        }

        if (!has_digits) {
            result.kind = NumericParseResult::Kind::Invalid;
            result.error_message = "no digits found";
            return result;
        }

        /// Check for trailing invalid characters
        if (pos_ < input_.size()) {
            result.kind = NumericParseResult::Kind::Invalid;
            result.error_message = "invalid character in number";
            return result;
        }

        result.text = std::move(text);
        result.kind = (has_dot || has_exponent)
            ? NumericParseResult::Kind::Flonum
            : NumericParseResult::Kind::Fixnum;
        return result;
    }

    NumericParseResult parse_integer(std::string text) {
        NumericParseResult result;
        result.radix = radix_;

        bool has_digits = false;

        while (pos_ < input_.size() && is_valid_digit(input_[pos_], radix_)) {
            text.push_back(input_[pos_++]);
            has_digits = true;
        }

        if (!has_digits) {
            result.kind = NumericParseResult::Kind::Invalid;
            result.error_message = "no valid digits for radix";
            return result;
        }

        /// Check for trailing invalid characters
        if (pos_ < input_.size()) {
            result.kind = NumericParseResult::Kind::Invalid;
            result.error_message = "invalid digit for radix";
            return result;
        }

        result.text = std::move(text);
        result.kind = NumericParseResult::Kind::Fixnum;
        return result;
    }

    std::string_view input_;
    std::uint8_t radix_;
    std::size_t pos_;
};

/**
 * @brief Convenience function for parsing numeric literals
 */
inline NumericParseResult parse_number(std::string_view input, std::uint8_t radix = 10) {
    return NumericParser(input, radix).parse();
}

/**
 * @brief Validate that a string is a valid integer for the given radix
 */
inline bool is_valid_integer(std::string_view s, std::uint8_t radix) noexcept {
    if (s.empty()) return false;

    std::size_t start = 0;
    if (s[0] == '+' || s[0] == '-') start = 1;
    if (start >= s.size()) return false;

    for (std::size_t i = start; i < s.size(); ++i) {
        if (!is_valid_digit(s[i], radix)) return false;
    }
    return true;
}

/**
 * @brief Validate that a string is a valid decimal number (int or float)
 */
inline bool is_valid_decimal(std::string_view s) noexcept {
    auto result = parse_number(s, 10);
    return result.kind != NumericParseResult::Kind::Invalid;
}

} ///< namespace numeric
} ///< namespace eta::reader::lexer

