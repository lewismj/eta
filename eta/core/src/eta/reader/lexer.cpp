#include "lexer.h"

#include <eta/arch.h>

#include <cctype>
#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

#include "numeric.h"
#include "unicode.h"

namespace {

    constexpr std::uint32_t kUnicodeMaxCodePoint   = 0x10'FFFF;
    constexpr std::uint32_t kHighSurrogateStart    = 0xD800;
    constexpr std::uint32_t kLowSurrogateEnd       = 0xDFFF;
    constexpr int           kShebangAllowedMaxCol  = 3;

    }

    namespace eta::reader::lexer {

    /// Constructor
    Lexer::Lexer(FileId file_id, std::string_view source)
        : file_id_(file_id)
        , source_(source)
        , pos_(0)
        , current_line_(1)
        , current_col_(1)
        , at_bof_(true) {}

    /// Error helper
    LexError Lexer::make_error(LexErrorKind kind, Position start, std::string msg, std::string_view detail) const {
        if (!detail.empty()) {
            msg += ": ";
            msg.append(detail.begin(), detail.end());
        }
        return LexError{kind, make_span(start, current_position()), std::move(msg)};
    }

    /// Main tokenization entry
    std::expected<Token, LexError> Lexer::next_token() {
        if (auto skip_result = skip_whitespace_and_comments(); !skip_result)
            return std::unexpected(skip_result.error());

        Position start = current_position();

        if (is_eof()) {
            at_bof_ = false;
            return Token{Token::Kind::EOF_, make_span(start, current_position())};
        }

        char c = peek();
        Token tok{Token::Kind::EOF_};

        switch (c) {
            case '(': advance(); tok = Token{Token::Kind::LParen, make_span(start, current_position())}; break;
            case ')': advance(); tok = Token{Token::Kind::RParen, make_span(start, current_position())}; break;
            case '[': advance(); tok = Token{Token::Kind::LBracket, make_span(start, current_position())}; break;
            case ']': advance(); tok = Token{Token::Kind::RBracket, make_span(start, current_position())}; break;
            case '\'': advance(); tok = Token{Token::Kind::Quote, make_span(start, current_position())}; break;
            case '`': advance(); tok = Token{Token::Kind::Backtick, make_span(start, current_position())}; break;
            case ',': {
                advance();
                if (!is_eof() && peek() == '@') {
                    advance();
                    tok = Token{Token::Kind::CommaAt, make_span(start, current_position())};
                } else {
                    tok = Token{Token::Kind::Comma, make_span(start, current_position())};
                }
                break;
            }
            case '"': {
                auto result = consume_string();
                if (!result) return std::unexpected(result.error());
                tok = Token{Token::Kind::String, std::move(*result), make_span(start, current_position())};
                break;
            }
            case '#': {
                auto result = consume_sharp();
                if (!result) return std::unexpected(result.error());
                tok = std::move(*result);
                tok.span = make_span(start, current_position());
                break;
            }
            case '|': {
                auto result = consume_bar_identifier();
                if (!result) return std::unexpected(result.error());
                tok = Token{Token::Kind::Symbol, std::move(*result), make_span(start, current_position())};
                break;
            }
            case '.': {
                if (pos_ + 2 < source_.size() && source_[pos_ + 1] == '.' && source_[pos_ + 2] == '.') {
                    advance(); advance(); advance();
                    tok = Token{Token::Kind::Symbol, std::string("..."), make_span(start, current_position())};
                } else if (pos_ + 1 < source_.size() && std::isdigit(static_cast<unsigned char>(source_[pos_ + 1]))) {
                    auto result = consume_bare_identifier_or_number();
                    if (!result) return std::unexpected(result.error());
                    tok = std::move(*result);
                    tok.span = make_span(start, current_position());
                } else {
                    advance();
                    tok = Token{Token::Kind::Dot, make_span(start, current_position())};
                }
                break;
            }
            default: {
                auto result = consume_bare_identifier_or_number();
                if (!result) return std::unexpected(result.error());
                tok = std::move(*result);
                tok.span = make_span(start, current_position());
                break;
            }
        }

        at_bof_ = false;
        return tok;
    }

    std::expected<void, LexError> Lexer::skip_whitespace_and_comments() {
        while (!is_eof()) {
            const char c = peek();

            if (std::isspace(static_cast<unsigned char>(c))) {
                advance();
                continue;
            }

            if (c == ';') {
                /// Line comment
                while (!is_eof() && peek() != '\n') {
                    advance();
                }
                continue;
            }

            if (c == '#') {
                const char next = peek(1);
                if (next == '|') {
                    /// Block comment
                    const Position start = current_position();
                    advance(); advance();
                    if (!skip_block_comment()) {
                        return std::unexpected(make_error(LexErrorKind::UnterminatedComment, start));
                    }
                    continue;
                }
                if (next == ';') {
                    /// Datum comment - skip one complete datum
                    Position dc_start = current_position();
                    advance(); advance();
                    if (auto r = skip_datum(dc_start); !r) return std::unexpected(r.error());
                    continue;
                }
                if (next == '!') {
                    /// Directive or shebang
                    advance(); advance();
                    consume_directive_or_shebang();
                    continue;
                }
            }

            break;
        }
        return {};
    }

    bool Lexer::skip_block_comment() {
        int depth = 1;
        while (!is_eof() && depth > 0) {
            char c = peek();
            if (c == '#' && peek(1) == '|') {
                advance(); advance();
                depth++;
            } else if (c == '|' && peek(1) == '#') {
                advance(); advance();
                depth--;
            } else {
                advance();
            }
        }
        return depth == 0;
    }

    std::expected<void, LexError> Lexer::skip_datum(Position start) {
        /// Get the first token of the datum
        auto tok = next_token();
        if (!tok || tok->kind == Token::Kind::EOF_) {
            return std::unexpected(make_error(LexErrorKind::UnexpectedEOF, start, "unterminated datum comment"));
        }

        switch (tok->kind) {
            /// Opening delimiters: skip until the matching close
            case Token::Kind::LParen:
            case Token::Kind::VectorStart:
            case Token::Kind::ByteVectorStart: {
                for (;;) {
                    auto inner = next_token();
                    if (!inner) return std::unexpected(inner.error());
                    if (inner->kind == Token::Kind::EOF_)
                        return std::unexpected(make_error(LexErrorKind::UnexpectedEOF, start, "unterminated datum comment"));
                    if (inner->kind == Token::Kind::RParen) break;
                    /**
                     * For nested open delimiters, we need to recursively handle them.
                     * Rewind isn't easy, so we handle nesting with a depth counter.
                     */
                    if (inner->kind == Token::Kind::LParen ||
                        inner->kind == Token::Kind::VectorStart ||
                        inner->kind == Token::Kind::ByteVectorStart ||
                        inner->kind == Token::Kind::LBracket) {
                        int depth = 1;
                        while (depth > 0) {
                            auto nested = next_token();
                            if (!nested) return std::unexpected(nested.error());
                            if (nested->kind == Token::Kind::EOF_)
                                return std::unexpected(make_error(LexErrorKind::UnexpectedEOF, start, "unterminated datum comment"));
                            if (nested->kind == Token::Kind::LParen ||
                                nested->kind == Token::Kind::VectorStart ||
                                nested->kind == Token::Kind::ByteVectorStart ||
                                nested->kind == Token::Kind::LBracket) depth++;
                            else if (nested->kind == Token::Kind::RParen ||
                                     nested->kind == Token::Kind::RBracket) depth--;
                        }
                    }
                }
                break;
            }
            case Token::Kind::LBracket: {
                for (;;) {
                    auto inner = next_token();
                    if (!inner) return std::unexpected(inner.error());
                    if (inner->kind == Token::Kind::EOF_)
                        return std::unexpected(make_error(LexErrorKind::UnexpectedEOF, start, "unterminated datum comment"));
                    if (inner->kind == Token::Kind::RBracket) break;
                    if (inner->kind == Token::Kind::LParen ||
                        inner->kind == Token::Kind::VectorStart ||
                        inner->kind == Token::Kind::ByteVectorStart ||
                        inner->kind == Token::Kind::LBracket) {
                        int depth = 1;
                        while (depth > 0) {
                            auto nested = next_token();
                            if (!nested) return std::unexpected(nested.error());
                            if (nested->kind == Token::Kind::EOF_)
                                return std::unexpected(make_error(LexErrorKind::UnexpectedEOF, start, "unterminated datum comment"));
                            if (nested->kind == Token::Kind::LParen ||
                                nested->kind == Token::Kind::VectorStart ||
                                nested->kind == Token::Kind::ByteVectorStart ||
                                nested->kind == Token::Kind::LBracket) depth++;
                            else if (nested->kind == Token::Kind::RParen ||
                                     nested->kind == Token::Kind::RBracket) depth--;
                        }
                    }
                }
                break;
            }
            /// Prefix tokens: skip the prefix + one more datum
            case Token::Kind::Quote:
            case Token::Kind::Backtick:
            case Token::Kind::Comma:
            case Token::Kind::CommaAt:
                return skip_datum(start);

            /// Atoms: already consumed by next_token()
            case Token::Kind::Boolean:
            case Token::Kind::Char:
            case Token::Kind::String:
            case Token::Kind::Symbol:
            case Token::Kind::Number:
                break;

            /// Closing delimiters and dot cannot start a datum
            case Token::Kind::RParen:
            case Token::Kind::RBracket:
            case Token::Kind::Dot:
                return std::unexpected(make_error(LexErrorKind::InvalidDatum, start,
                    "expected a datum after #; but found a delimiter"));

            /// EOF_ is already handled before the switch
            case Token::Kind::EOF_:
                return std::unexpected(make_error(LexErrorKind::UnexpectedEOF, start,
                    "unterminated datum comment"));
        }
        return {};
    }

    void Lexer::consume_directive_or_shebang() {
        const bool was_at_line_start = at_bof_ && current_line_ == 1 && current_col_ <= static_cast<std::uint32_t>(kShebangAllowedMaxCol);

        std::string word;
        /// Read directive name only; stop at whitespace or delimiter
        while (!is_eof() && !is_hard_delimiter(peek()) && !std::isspace(static_cast<unsigned char>(peek()))) {
            word.push_back(advance());
        }

        const std::string lower = to_lower(word);

        if (was_at_line_start && lower != "fold-case" && lower != "no-fold-case") {
            /// Shebang - skip to end of line
            while (!is_eof() && peek() != '\n') {
                advance();
            }
            return;
        }
        /// For fold-case and no-fold-case directives: ignore (no-ops) to comply with modern Scheme defaults.
    }

    /// Decode a single UTF-8 sequence from input without advancing on failure
    std::expected<char32_t, LexError> Lexer::decode_utf8(Position start) {
        if (is_eof()) return std::unexpected(make_error(LexErrorKind::UnexpectedEOF, start));
        unsigned char b0 = static_cast<unsigned char>(peek());
        if (b0 < 0x80) {
            return static_cast<char32_t>(b0);
        }
        auto unexpected_utf8 = [&]() -> std::expected<char32_t, LexError> {
            return std::unexpected(make_error(LexErrorKind::InvalidUtf8, start, "invalid UTF-8 sequence"));
        };
        std::uint32_t code = 0;
        int need = 0;
        if ((b0 & 0xE0) == 0xC0) { code = b0 & 0x1F; need = 1; if (code < 0x2) return unexpected_utf8(); }
        else if ((b0 & 0xF0) == 0xE0) { code = b0 & 0x0F; need = 2; }
        else if ((b0 & 0xF8) == 0xF0) { code = b0 & 0x07; need = 3; if (code > 0x4) return unexpected_utf8(); }
        else { return unexpected_utf8(); }
        /// Validate continuation bytes
        for (int i = 1; i <= need; ++i) {
            if (pos_ + i >= source_.size()) return std::unexpected(make_error(LexErrorKind::InvalidUtf8, start, "truncated UTF-8 sequence"));
            unsigned char bx = static_cast<unsigned char>(source_[pos_ + i]);
            if ((bx & 0xC0) != 0x80) return unexpected_utf8();
            code = (code << 6) | (bx & 0x3F);
        }
        /// Check scalar range and surrogates
        if (code > kUnicodeMaxCodePoint || (code >= kHighSurrogateStart && code <= kLowSurrogateEnd)) {
            return std::unexpected(make_error(LexErrorKind::InvalidCodePoint, start, "Unicode scalar value out of range"));
        }
        return static_cast<char32_t>(code);
    }

    std::expected<std::string, LexError> Lexer::consume_string() {
        const Position start = current_position();
        advance();

        std::string result;
        while (!is_eof()) {
            const char c = peek();
            if (c == '"') {
                advance();
                return result;
            }
            if (c == '\\') {
                advance();
                if (is_eof()) {
                    return std::unexpected(make_error(LexErrorKind::UnterminatedString, start));
                }
                switch (advance()) {
                    case 'a': result.push_back('\x07'); break;
                    case 'b': result.push_back('\x08'); break;
                    case 't': result.push_back('\t'); break;
                    case 'n': result.push_back('\n'); break;
                    case 'r': result.push_back('\r'); break;
                    case '"': result.push_back('"'); break;
                    case '\\': result.push_back('\\'); break;
                    case 'x': case 'X': {
                        auto ch = consume_hex_until_semicolon();
                        if (!ch) return std::unexpected(ch.error());
                        unicode::append_utf8(result, static_cast<std::uint32_t>(*ch));
                        break;
                    }
                    default:
                        return std::unexpected(make_error(LexErrorKind::InvalidToken, start, "Invalid escape sequence"));
                }
            } else if (c == '\n') {
                return std::unexpected(make_error(LexErrorKind::UnterminatedString, start));
            } else {
                if (static_cast<unsigned char>(c) < 0x80) {
                    result.push_back(advance());
                } else {
                    /// Validate UTF-8 and append
                    Position p = current_position();
                    auto cp = decode_utf8(p);
                    if (!cp) return std::unexpected(cp.error());
                    /// advance by the sequence length
                    unsigned char b0 = static_cast<unsigned char>(peek());
                    int adv = (b0 < 0x80) ? 1 : ((b0 & 0xE0) == 0xC0 ? 2 : ( (b0 & 0xF0) == 0xE0 ? 3 : 4));
                    for (int i = 0; i < adv; ++i) advance();
                    unicode::append_utf8(result, static_cast<std::uint32_t>(*cp));
                }
            }
        }

        return std::unexpected(make_error(LexErrorKind::UnterminatedString, start));
    }

    std::expected<char32_t, LexError> Lexer::consume_char() {
        const Position start = current_position();
        advance(); ///< '\\'

        if (is_eof()) {
            return std::unexpected(make_error(LexErrorKind::InvalidChar, start, "unexpected EOF after #\\"));
        }

        if (peek_lower() == 'x') {
            advance();
            return consume_hex_until_semicolon();
        }

        /// Delimiter character (except whitespace which could be "space")
        if (is_hard_delimiter(peek())) {
            return static_cast<char32_t>(advance());
        }

        /// Named character
        std::string name;
        while (!is_eof() && !is_hard_delimiter(peek()) && !std::isspace(static_cast<unsigned char>(peek()))) {
            name.push_back(advance());
        }

        if (name.empty()) {
            return std::unexpected(make_error(LexErrorKind::InvalidChar, start, "empty character name"));
        }

        const std::string lower = to_lower(name);
        if (lower == "space") return U' ';
        if (lower == "newline") return U'\n';
        if (lower == "tab") return U'\t';
        if (lower == "return") return U'\r';
        if (lower == "linefeed") return U'\n';
        if (lower == "backspace") return U'\x08';
        if (lower == "alarm") return U'\x07';
        if (lower == "vtab") return U'\x0B';
        if (lower == "page") return U'\x0C';
        if (lower == "nul" || lower == "null") return U'\0';
        if (lower == "escape") return U'\x1B';
        if (lower == "delete" || lower == "del") return U'\x7F';

        if (name.size() == 1) return static_cast<char32_t>(name[0]);

        return std::unexpected(make_error(LexErrorKind::InvalidChar, start, "unknown character name: " + name));
    }

    std::expected<char32_t, LexError> Lexer::consume_hex_until_semicolon() {
        const Position start = current_position();
        std::string hex;

        while (!is_eof() && peek() != ';') {
            char c = peek();
            if (std::isxdigit(static_cast<unsigned char>(c))) {
                hex.push_back(advance());
            } else {
                return std::unexpected(make_error(LexErrorKind::InvalidToken, start, "invalid hex digit"));
            }
        }

        if (!is_eof() && peek() == ';') {
            advance();
        }

        if (hex.empty()) {
            return std::unexpected(make_error(LexErrorKind::InvalidToken, start, "empty hex escape"));
        }

        std::uint32_t code = 0;
        for (const char c : hex) {
            const unsigned v = static_cast<unsigned>(hex_value(c));
            if (code > (kUnicodeMaxCodePoint - v) / 16) {
                return std::unexpected(make_error(LexErrorKind::InvalidCodePoint, start, "Unicode escape exceeds maximum code point"));
            }
            code = code * 16 + v;
        }

        if (code >= kHighSurrogateStart && code <= kLowSurrogateEnd) {
            return std::unexpected(make_error(LexErrorKind::InvalidCodePoint, start, "surrogate not allowed"));
        }

        return static_cast<char32_t>(code);
    }

    std::expected<Token, LexError> Lexer::consume_sharp() {
        const Position start = current_position();
        advance(); ///< '#'

        if (is_eof()) {
            return std::unexpected(make_error(LexErrorKind::InvalidToken, start));
        }

        char c = peek_lower();

        switch (c) {
            case 't': case 'f': {
                auto result = consume_boolean();
                if (!result) return std::unexpected(result.error());
                return Token{Token::Kind::Boolean, *result, make_span(start, current_position())};
            }
            case '\\': {
                auto result = consume_char();
                if (!result) return std::unexpected(result.error());
                if (auto ok = require_token_boundary(start, "character"); !ok) return std::unexpected(ok.error());
                return Token{Token::Kind::Char, *result, make_span(start, current_position())};
            }
            case '(': {
                advance();
                return Token{Token::Kind::VectorStart, make_span(start, current_position())};
            }
            case 'u': {
                if (peek(1) == '8' && peek(2) == '(') {
                    advance(); advance(); advance();
                    return Token{Token::Kind::ByteVectorStart, make_span(start, current_position())};
                }
                return std::unexpected(make_error(LexErrorKind::InvalidToken, start, "expected #u8( for bytevector"));
            }
            case 'b': case 'o': case 'd': case 'x':
            case 'e': case 'i':
                return consume_number_with_prefixes(start);
            default:
                return std::unexpected(make_error(LexErrorKind::InvalidToken, start));
        }
    }

    bool Lexer::consume_exact_tail_icase(std::string_view tail) noexcept {
        Position p = current_position();
        for (char want : tail) {
            if (is_eof() || std::tolower(static_cast<unsigned char>(peek())) != want) {
                /// rewind
                pos_ = p.offset;
                current_line_ = p.line;
                current_col_ = p.column;
                return false;
            }
            advance();
        }
        return true;
    }

    std::expected<bool, LexError> Lexer::consume_boolean() {
        const Position start = current_position();
        const char c = peek_lower();
        advance();

        if (c == 't') {
            /// Accept both short form "#t" and long form "#true"
            (void)consume_exact_tail_icase("rue"); ///< optional; rewinds internally on mismatch
        } else if (c == 'f') {
            /// Accept both short form "#f" and long form "#false"
            (void)consume_exact_tail_icase("alse"); ///< optional; rewinds internally on mismatch
        } else {
            return std::unexpected(make_error(LexErrorKind::InvalidBoolean, start));
        }

        /// Enforce delimiter after boolean (reject things like #tr or #f0)
        if (auto ok = require_token_boundary(start, "boolean"); !ok) return std::unexpected(ok.error());

        return c == 't';
    }

    bool Lexer::iequals_prefix(std::string_view prefix) const noexcept {
        if (pos_ + prefix.size() > source_.size()) return false;
        for (std::size_t i = 0; i < prefix.size(); ++i) {
            if (peek_lower(i) != prefix[i]) return false;
        }
        return true;
    }

    std::optional<std::string> Lexer::try_collect_inf_nan() {
        const std::size_t saved = pos_;
        std::string out;

        /// Optional sign
        if (!is_eof() && (peek() == '+' || peek() == '-')) {
            out.push_back(advance());
        }

        /// Check for inf.0 or nan.0 using unified numeric helper
        if (iequals_prefix("inf.0")) {
            for (int i = 0; i < 5; ++i) out.push_back(advance());
            if (numeric::is_special_float(out)) {
                return out;
            }
        }
        if (iequals_prefix("nan.0")) {
            for (int i = 0; i < 5; ++i) out.push_back(advance());
            if (numeric::is_special_float(out)) {
                return out;
            }
        }

        /// No match; restore
        pos_ = saved;
        return std::nullopt;
    }

    std::expected<Token, LexError> Lexer::consume_number_with_prefixes(Position start) {
        std::uint8_t radix = 10;

        auto consume_prefix = [&](char c) -> bool {
            switch (c) {
                case 'b': if (radix == 10) { radix = 2; return true; } break;
                case 'o': if (radix == 10) { radix = 8; return true; } break;
                case 'd': if (radix == 10) { radix = 10; return true; } break;
                case 'x': if (radix == 10) { radix = 16; return true; } break;
                /// Ignore exactness prefixes - we only have fixnum/flonum
                case 'e': case 'i': return true;
                default: break;
            }
            return false;
        };

        /// Consume first prefix letter (already peeked by consume_sharp)
        if (!is_eof()) {
            if (char c = peek_lower(); consume_prefix(c)) {
                advance();
            }
        }

        /// Handle optional second prefix: #e#x, #x#e, etc.
        if (!is_eof() && peek() == '#') {
            advance(); ///< skip the second '#'
            if (!is_eof()) {
                if (char c = peek_lower(); consume_prefix(c)) {
                    advance();
                } else {
                    return std::unexpected(make_error(LexErrorKind::InvalidNumeric, start, "invalid number prefix"));
                }
            } else {
                return std::unexpected(make_error(LexErrorKind::InvalidNumeric, start, "unexpected EOF after #"));
            }
        }

        /// Collect number body based on radix
        const std::string body = (radix == 10) ? collect_number_decimal() : collect_number_radix(radix);

        /// After collecting `body` in consume_number_with_prefixes(...)
        if (body.empty()) {
            return std::unexpected(make_error(LexErrorKind::InvalidNumeric, start, "missing digits"));
        }

        /**
         * NEW: for non-decimal prefixes, reject immediately if the next char is
         * neither a token boundary nor a valid digit for the selected radix.
         */
        if (radix != 10 && !is_eof()) {
            if (char next = peek(); !is_token_boundary(next) && !is_valid_digit_for_radix(next, radix)) {
                return std::unexpected(make_error(LexErrorKind::InvalidNumeric, start, "invalid digit for radix"));
            }
        }

        auto classified = classify_number(body, radix, start);
        if (!classified) return classified;
        if (auto ok = require_token_boundary(start, "number"); !ok) return std::unexpected(ok.error());

        return classified;
    }

    std::string Lexer::collect_number_decimal() {
        /// Special-case IEEE literals first
        if (auto special = try_collect_inf_nan()) {
            return *special;
        }

        std::string out;
        bool seen_dot = false;
        bool seen_exp = false;

        /// Optional leading sign
        if (!is_eof() && (peek() == '+' || peek() == '-')) {
            out.push_back(advance());
        }

        while (!is_eof()) {
            const char c = peek();
            if (is_hard_delimiter(c)) break;

            if (std::isdigit(static_cast<unsigned char>(c))) {
                out.push_back(advance());
                continue;
            }
            if (c == '.' && !seen_dot && !seen_exp) {
                seen_dot = true;
                out.push_back(advance());
                continue;
            }
            if ((c == 'e' || c == 'E') && !seen_exp) {
                seen_exp = true;
                out.push_back(advance());
                /// Optional sign immediately after exponent
                if (!is_eof() && (peek() == '+' || peek() == '-')) {
                    out.push_back(advance());
                }
                continue;
            }
            break;
        }

        return out;
    }

    std::string Lexer::collect_number_radix(std::uint8_t radix) {
        std::string out;

        /// Optional sign
        if (!is_eof() && (peek() == '+' || peek() == '-')) {
            out.push_back(advance());
        }

        /// Collect valid digits for this radix
        while (!is_eof()) {
            char c = peek();
            if (is_hard_delimiter(c)) break;
            if (!is_valid_digit_for_radix(c, radix)) break;
            out.push_back(advance());
        }

        return out;
    }

    std::expected<Token, LexError> Lexer::classify_number(const std::string& body, std::uint8_t radix, Position start) {
        /// Use unified numeric parser from numeric.h
        auto result = numeric::parse_number(body, radix);

        switch (result.kind) {
            case numeric::NumericParseResult::Kind::SpecialFloat:
            case numeric::NumericParseResult::Kind::Flonum: {
                NumericToken num{NumericToken::Kind::Flonum, body, 10};
                return Token{Token::Kind::Number, std::move(num), make_span(start, current_position())};
            }
            case numeric::NumericParseResult::Kind::Fixnum: {
                NumericToken num{NumericToken::Kind::Fixnum, body, radix};
                return Token{Token::Kind::Number, std::move(num), make_span(start, current_position())};
            }
            case numeric::NumericParseResult::Kind::Invalid:
            default:
                return std::unexpected(make_error(LexErrorKind::InvalidNumeric, start, result.error_message.empty() ? "unrecognized numeric format" : result.error_message));
        }
    }

    std::expected<std::string, LexError> Lexer::consume_bar_identifier() {
        const Position start = current_position();
        advance(); ///< '|'

        std::string result;
        while (!is_eof()) {
            char c = peek();
            if (c == '|') {
                advance();
                return result;
            }
            if (c == '\\') {
                advance();
                if (is_eof()) {
                    return std::unexpected(make_error(LexErrorKind::InvalidToken, start));
                }
                char escape = advance();
                if (escape == 'x' || escape == 'X') {
                    auto ch = consume_hex_until_semicolon();
                    if (!ch) return std::unexpected(ch.error());
                    unicode::append_utf8(result, static_cast<std::uint32_t>(*ch));
                } else {
                    result.push_back(escape);
                }
            } else {
                result.push_back(advance());
            }
        }

        return std::unexpected(make_error(LexErrorKind::InvalidToken, start, "unterminated bar identifier"));
    }

    std::expected<Token, LexError> Lexer::consume_bare_identifier_or_number() {
        const Position start = current_position();

        /// Check if it's a number
        char c0 = peek();
        bool could_be_number = false;

        if (c0 == '+' || c0 == '-') {
            if (is_eof()) {
                advance();
                return Token{Token::Kind::Symbol, std::string(1, c0), make_span(start, current_position())};
            }
            char next = peek(1);
            could_be_number = std::isdigit(static_cast<unsigned char>(next)) || next == '.';

            /// Check for +inf.0, -inf.0, +nan.0, -nan.0
            if (!could_be_number) {
                if (auto special = try_collect_inf_nan()) {
                    return classify_number(*special, 10, start);
                }
            }
        } else if (c0 == '.') {
            could_be_number = !is_eof() && std::isdigit(static_cast<unsigned char>(peek(1)));
        } else if (std::isdigit(static_cast<unsigned char>(c0))) {
            could_be_number = true;
        }

        if (could_be_number) {
            const std::string body = collect_number_decimal();
            auto classified = classify_number(body, 10, start);
            if (!classified) return classified;
            if (auto ok = require_token_boundary(start, "number"); !ok) return std::unexpected(ok.error());
            return classified;
        }

        /// Otherwise it's an identifier
        if (!is_identifier_initial(c0) && c0 != '+' && c0 != '-') {
            return std::unexpected(make_error(LexErrorKind::InvalidToken, start));
        }

        std::string name;
        name.push_back(advance());

        /// Special case: bare '+' or '-'
        if ((name == "+" || name == "-") && (is_eof() || is_hard_delimiter(peek()) || std::isspace(static_cast<unsigned char>(peek())))) {
            return Token{Token::Kind::Symbol, name, make_span(start, current_position())};
        }

        while (!is_eof() && is_identifier_subsequent(peek())) {
            name.push_back(advance());
        }

        /// Case-folding deprecated: keep identifiers as-is.

        if (auto ok = require_token_boundary(start, "identifier"); !ok) return std::unexpected(ok.error());
        return Token{Token::Kind::Symbol, std::move(name), make_span(start, current_position())};
    }

    int Lexer::hex_value(char c) noexcept {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return 0;
    }

    std::string Lexer::to_lower(const std::string& s) noexcept {
        std::string result;
        result.reserve(s.size());
        for (char c : s) {
            result.push_back(std::tolower(static_cast<unsigned char>(c)));
        }
        return result;
    }

    bool Lexer::is_valid_decimal(const std::string& s) noexcept {
        /// Delegate to unified numeric helper from numeric.h
        return numeric::is_valid_decimal(s);
    }

    bool Lexer::is_signed_integer(const std::string& s, std::uint8_t radix) noexcept {
        /// Delegate to unified numeric helper from numeric.h
        return numeric::is_valid_integer(s, radix);
    }

    bool Lexer::is_valid_digit_for_radix(char c, const std::uint8_t radix) noexcept {
        /// Delegate to unified numeric helper from numeric.h
        return numeric::is_valid_digit(c, radix);
    }

    bool Lexer::is_identifier_subsequent(char c) noexcept {
        return is_identifier_initial(c) || std::isdigit(static_cast<unsigned char>(c)) ||
               c == '+' || c == '-' || c == '.' || c == '@';
    }

    bool Lexer::is_identifier_initial(char c) noexcept {
        return static_cast<bool>(std::isalpha(static_cast<unsigned char>(c))) ||
               c == '!' || c == '$' || c == '%' || c == '&' || c == '*' ||
               c == '/' || c == ':' || c == '<' || c == '=' || c == '>' ||
               c == '?' || c == '^' || c == '_' || c == '~' || c == '@';
    }

    bool Lexer::is_token_boundary(char c) noexcept {
        return is_eof_char(c) || is_hard_delimiter(c) || std::isspace(static_cast<unsigned char>(c));
    }

    bool Lexer::is_hard_delimiter(char c) noexcept {
        return c == '(' || c == ')' || c == '[' || c == ']' ||
               c == '"' || c == ';' || c == ',' || c == '`' ||
               c == '\''; // note: '#' starts a sharp token but is not a delimiter for preceding tokens
    }

    bool Lexer::is_eof_char(char c) noexcept { return c == '\0'; }

    std::expected<void, LexError> Lexer::require_token_boundary(Position start, std::string_view what) const {
        if (source_.empty() || is_eof()) return {};
        const char c = (pos_ < source_.size() ? source_[pos_] : '\0');
        if (is_token_boundary(c)) return {};
        return std::unexpected(LexError{LexErrorKind::MissingDelimiter, make_span(start, current_position()), std::string(what) + " must be followed by a delimiter"});
    }

}
