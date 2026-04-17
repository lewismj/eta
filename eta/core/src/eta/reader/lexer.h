#pragma once

#include <cassert>
#include <cctype>
#include <cstdint>
#include <expected>
#include <optional>
#include <ostream>
#include <string>
#include <string_view>
#include <variant>


namespace eta::reader::lexer {

    using FileId = std::uint32_t;

    struct Position {
        std::uint64_t offset{0};
        std::uint32_t line{1};
        std::uint32_t column{1};

        constexpr Position() = default;
        constexpr Position(const std::uint64_t off, const std::uint32_t ln, const std::uint32_t col)
            : offset(off), line(ln), column(col) {}
    };

    struct Span {
        FileId file_id{0};
        Position start;
        Position end;

        constexpr Span() = default;
        constexpr Span(FileId fid, Position s, Position e)
            : file_id(fid), start(s), end(e) {}

        [[nodiscard]] constexpr bool is_valid() const noexcept {
            return start.offset <= end.offset;
        }
    };

    struct NumericToken {
        enum class Kind : std::uint8_t {
            Fixnum,   ///< Integer in any radix
            Flonum,   ///< Floating-point decimal
        };

        Kind kind;
        std::string text;        ///< Original text representation
        std::uint8_t radix{10};  ///< 2, 8, 10, or 16 (only for Fixnum)

        explicit NumericToken(Kind k) : kind(k) {}
        NumericToken(Kind k, std::string txt, std::uint8_t rad = 10)
            : kind(k), text(std::move(txt)), radix(rad) {}
    };

    constexpr const char* to_string(const NumericToken::Kind kind) noexcept {
        using enum NumericToken::Kind;
        switch (kind) {
            case Fixnum: return "NumericToken::Kind::Fixnum";
            case Flonum: return "NumericToken::Kind::Flonum";
        }
        return "NumericToken::Kind::Unknown";
    }

    inline std::ostream& operator<<(std::ostream& os, const NumericToken::Kind kind) {
        return os << to_string(kind);
    }

    struct Token {
        enum class Kind : std::uint8_t {
            LParen,
            RParen,
            LBracket,
            RBracket,
            Quote,
            Backtick,
            Comma,
            CommaAt,
            Boolean,
            Char,
            String,
            Symbol,
            Number,
            Dot,
            VectorStart,    ///< #(
            ByteVectorStart, ///< #u8(
            EOF_,
        };

        Kind kind;
        std::variant<bool, char32_t, std::string, NumericToken> value;
        Span span;

        explicit Token(Kind k, Span s = {}) : kind(k), span(s) {}

        template<typename T>
        Token(Kind k, T&& v, Span s = {})
            : kind(k), value(std::forward<T>(v)), span(s) {}
    };

    constexpr const char* to_string(const Token::Kind kind) noexcept {
        using enum Token::Kind;
        switch (kind) {
            case LParen:         return "Token::Kind::LParen";
            case RParen:         return "Token::Kind::RParen";
            case LBracket:       return "Token::Kind::LBracket";
            case RBracket:       return "Token::Kind::RBracket";
            case Quote:          return "Token::Kind::Quote";
            case Backtick:       return "Token::Kind::Backtick";
            case Comma:          return "Token::Kind::Comma";
            case CommaAt:        return "Token::Kind::CommaAt";
            case Boolean:        return "Token::Kind::Boolean";
            case Char:           return "Token::Kind::Char";
            case String:         return "Token::Kind::String";
            case Symbol:         return "Token::Kind::Symbol";
            case Number:         return "Token::Kind::Number";
            case Dot:            return "Token::Kind::Dot";
            case VectorStart:    return "Token::Kind::VectorStart";
            case ByteVectorStart:return "Token::Kind::ByteVectorStart";
            case EOF_:           return "Token::Kind::EOF_";
        }
        return "Token::Kind::Unknown";
    }

    inline std::ostream& operator<<(std::ostream& os, const Token::Kind kind) {
        return os << to_string(kind);
    }

    enum class LexErrorKind : std::uint8_t {
        UnexpectedEOF,
        UnterminatedString,
        UnterminatedComment,
        InvalidChar,
        InvalidToken,
        InvalidNumeric,
        InvalidUtf8,
        MissingDelimiter,
        InvalidCodePoint,
        InvalidBoolean,
        InvalidDatum
    };

    struct LexError {
        LexErrorKind kind;
        Span span;
        std::string message;

        LexError(LexErrorKind k, Span s, std::string msg = {})
            : kind(k), span(s), message(std::move(msg)) {}
    };

    constexpr const char* to_string(const LexErrorKind kind) noexcept {
        using enum LexErrorKind;
        switch (kind) {
            case UnexpectedEOF:       return "LexErrorKind::UnexpectedEOF";
            case UnterminatedString:  return "LexErrorKind::UnterminatedString";
            case UnterminatedComment: return "LexErrorKind::UnterminatedComment";
            case InvalidChar:         return "LexErrorKind::InvalidChar";
            case InvalidToken:        return "LexErrorKind::InvalidToken";
            case InvalidNumeric:      return "LexErrorKind::InvalidNumeric";
            case InvalidUtf8:         return "LexErrorKind::InvalidUtf8";
            case MissingDelimiter:    return "LexErrorKind::MissingDelimiter";
            case InvalidCodePoint:    return "LexErrorKind::InvalidCodePoint";
            case InvalidBoolean:      return "LexErrorKind::InvalidBoolean";
            case InvalidDatum:        return "LexErrorKind::InvalidDatum";
        }
        return "LexErrorKind::Unknown";
    }

    inline std::ostream& operator<<(std::ostream& os, const LexErrorKind kind) {
        return os << to_string(kind);
    }

    class Lexer {
    public:
        explicit Lexer(FileId file_id, std::string_view source);

        std::expected<Token, LexError> next_token();

        [[nodiscard]] FileId file_id() const noexcept { return file_id_; }

    private:
        FileId file_id_;
        std::string_view source_;
        std::size_t pos_;
        std::uint32_t current_line_;
        std::uint32_t current_col_;
        bool at_bof_;

        [[nodiscard]] bool is_eof() const noexcept {
            return pos_ >= source_.size();
        }

        [[nodiscard]] char peek(std::size_t offset = 0) const noexcept {
            return (pos_ + offset < source_.size()) ? source_[pos_ + offset] : '\0';
        }

        [[nodiscard]] char peek_lower(std::size_t offset = 0) const noexcept {
            return std::tolower(static_cast<unsigned char>(peek(offset)));
        }

        char advance() noexcept {
            if (is_eof()) return '\0';
            const char c = source_[pos_++];
            if (c == '\n') {
                current_line_++;
                current_col_ = 1;
            } else {
                current_col_++;
            }
            return c;
        }

        [[nodiscard]] Position current_position() const noexcept {
            return Position{pos_, current_line_, current_col_};
        }

        [[nodiscard]] Span make_span(Position start, Position end) const noexcept {
            return Span{file_id_, start, end};
        }

        LexError make_error(LexErrorKind kind, Position start, std::string msg = {}, std::string_view detail = {}) const;

        [[nodiscard]] static bool is_eof_char(char c) noexcept;

        /// Hard delimiters that always end a token
        [[nodiscard]] static bool is_hard_delimiter(char c) noexcept;

        [[nodiscard]] static bool is_token_boundary(char c) noexcept;

        std::expected<void, LexError> require_token_boundary(Position start, std::string_view what) const;

        std::expected<void, LexError> skip_whitespace_and_comments();

        bool skip_block_comment();

        std::expected<void, LexError> skip_datum(Position start);

        void consume_directive_or_shebang();

        /// Decode a single UTF-8 sequence from input without advancing on failure
        std::expected<char32_t, LexError> decode_utf8(Position start);

        std::expected<std::string, LexError> consume_string();

        std::expected<char32_t, LexError> consume_char();


        std::expected<char32_t, LexError> consume_hex_until_semicolon();

        std::expected<Token, LexError> consume_sharp();

        bool consume_exact_tail_icase(std::string_view tail) noexcept;

        std::expected<bool, LexError> consume_boolean();


        bool iequals_prefix(std::string_view prefix) const noexcept;

        std::optional<std::string> try_collect_inf_nan();

        std::expected<Token, LexError> consume_number_with_prefixes(Position start);

        std::string collect_number_decimal();

        std::string collect_number_radix(std::uint8_t radix);

        std::expected<Token, LexError> classify_number(const std::string& body, std::uint8_t radix, Position start);

        std::expected<std::string, LexError> consume_bar_identifier();

        std::expected<Token, LexError> consume_bare_identifier_or_number();

        [[nodiscard]] static bool is_identifier_initial(char c) noexcept;

        [[nodiscard]] static bool is_identifier_subsequent(char c) noexcept;

        [[nodiscard]] static bool is_valid_digit_for_radix(char c, const std::uint8_t radix) noexcept;

        [[nodiscard]] static bool is_signed_integer(const std::string& s, std::uint8_t radix) noexcept;

        [[nodiscard]] static bool is_valid_decimal(const std::string& s) noexcept;

        [[nodiscard]] static std::string to_lower(const std::string& s) noexcept;

        [[nodiscard]] static int hex_value(char c) noexcept;
    };

}
