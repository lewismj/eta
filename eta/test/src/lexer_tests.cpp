#include <boost/test/unit_test.hpp>

#include <eta/reader/lexer.h>
#include <string>
#include <vector>
#include <variant>

using namespace eta::reader::lexer;

namespace {
    struct TokView {
        Token::Kind kind;
        // Optional expectations
        std::optional<bool> b{};
        std::optional<char32_t> ch{};
        std::optional<std::string> str{};
        // For numbers
        std::optional<NumericToken::Kind> num_kind{};
        std::optional<std::uint8_t> radix{};
        std::optional<std::string> num_text{}; // exact body text as lexer stores it
    };

    std::vector<Token> lex_all(std::string src, FileId fid = 1) {
        Lexer lx(fid, src);
        std::vector<Token> out;
        for (;;) {
            auto t = lx.next_token();
            if (!t) {
                // Represent errors as a single EOF_ with message for convenience in callers that expect only success.
                // In error-specific tests we call next_token() directly to assert on LexError.
                break;
            }
            out.push_back(*t);
            if (t->kind == Token::Kind::EOF_) break;
        }
        return out;
    }

    void check_tokens(const std::string& src, std::initializer_list<TokView> expect) {
        auto toks = lex_all(src);
        BOOST_TEST(toks.size() == expect.size());
        auto it = toks.begin();
        for (const auto& ev : expect) {
            BOOST_TEST(it->kind == ev.kind);
            switch (ev.kind) {
                case Token::Kind::Boolean: {
                    BOOST_TEST(std::holds_alternative<bool>(it->value));
                    if (ev.b) BOOST_TEST(std::get<bool>(it->value) == *ev.b);
                    break;
                }
                case Token::Kind::Char: {
                    BOOST_TEST(std::holds_alternative<char32_t>(it->value));
                    if (ev.ch) {
                        auto actual = static_cast<std::uint32_t>(std::get<char32_t>(it->value));
                        auto expected = static_cast<std::uint32_t>(*ev.ch);
                        BOOST_TEST(actual == expected);
                    }
                    break;
                }
                case Token::Kind::String: {
                    BOOST_TEST(std::holds_alternative<std::string>(it->value));
                    if (ev.str) BOOST_TEST(std::get<std::string>(it->value) == *ev.str);
                    break;
                }
                case Token::Kind::Symbol: {
                    BOOST_TEST(std::holds_alternative<std::string>(it->value));
                    if (ev.str) BOOST_TEST(std::get<std::string>(it->value) == *ev.str);
                    break;
                }
                case Token::Kind::Number: {
                    BOOST_TEST(std::holds_alternative<NumericToken>(it->value));
                    const auto& n = std::get<NumericToken>(it->value);
                    if (ev.num_kind) BOOST_TEST(n.kind == *ev.num_kind);
                    if (ev.radix) BOOST_TEST(n.radix == *ev.radix);
                    if (ev.num_text) BOOST_TEST(n.text == *ev.num_text);
                    break;
                }
                default: break;
            }
            ++it;
        }
    }
}

BOOST_AUTO_TEST_SUITE(lexer_tests)

BOOST_AUTO_TEST_CASE(punctuation_and_simple_tokens) {
    check_tokens("( ) [ ] ' ` , ,@ . ...", {
        {Token::Kind::LParen},
        {Token::Kind::RParen},
        {Token::Kind::LBracket},
        {Token::Kind::RBracket},
        {Token::Kind::Quote},
        {Token::Kind::Backtick},
        {Token::Kind::Comma},
        {Token::Kind::CommaAt},
        {Token::Kind::Dot},
        {Token::Kind::Symbol, {}, {}, std::string("...")},
        {Token::Kind::EOF_},
    });
}

BOOST_AUTO_TEST_CASE(skip_whitespace_and_comments) {
    // line comments and whitespace
    check_tokens(" ; comment to end of line\n  'x ", {
        {Token::Kind::Quote},
        {Token::Kind::Symbol, {}, {}, std::string("x")},
        {Token::Kind::EOF_},
    });

    // nested block comments #| ... #| ... |# ... |#
    check_tokens("#| a #| b |# c |# 123", {
        {Token::Kind::Number, {}, {}, {}, NumericToken::Kind::Fixnum, 10, std::string("123")},
        {Token::Kind::EOF_},
    });

    // datum comment skips next datum only
    check_tokens("#; 123 456", {
        {Token::Kind::Number, {}, {}, {}, NumericToken::Kind::Fixnum, 10, std::string("456")},
        {Token::Kind::EOF_},
    });
}

BOOST_AUTO_TEST_CASE(booleans_and_delimiters) {
    check_tokens("#true #false", {
        {Token::Kind::Boolean, true},
        {Token::Kind::Boolean, false},
        {Token::Kind::EOF_},
    });

    // Missing delimiter after boolean should be reported
    Lexer lx(1, "#truex");
    auto t = lx.next_token();
    BOOST_TEST(!t.has_value());
    BOOST_TEST(t.error().kind == LexErrorKind::MissingDelimiter);
}

BOOST_AUTO_TEST_CASE(strings_basic_and_escapes) {

    check_tokens("\"hello\" \"a\\n\\t\\\\\\\"\"", {
        {Token::Kind::String, {}, {}, std::string("hello")},
        {Token::Kind::String, {}, {}, std::string("a\n\t\\\"")},
        {Token::Kind::EOF_},
    });

    // Hex escape to Unicode scalar value
    check_tokens("\"A: \\x41;\"", {
        {Token::Kind::String, {}, {}, std::string("A: A")},
        {Token::Kind::EOF_},
    });

    // Unterminated string (newline cutoff)
    Lexer lx1(1, "\"unterminated\n");
    auto e1 = lx1.next_token();
    BOOST_TEST(!e1.has_value());
    BOOST_TEST(e1.error().kind == LexErrorKind::UnterminatedString);

    // Invalid code point via hex escape (surrogate)
    Lexer lx2(1, "\"bad: \\xD800;\"");
    auto e2 = lx2.next_token();
    BOOST_TEST(!e2.has_value());
    BOOST_TEST(e2.error().kind == LexErrorKind::InvalidCodePoint);
}

BOOST_AUTO_TEST_CASE(characters_named_hex_and_delim) {
    check_tokens("#\\space #\\newline #\\tab #\\return #\\backspace #\\alarm #\\vtab #\\page #\\nul #\\escape #\\delete", {
        {Token::Kind::Char, {}, U' '},
        {Token::Kind::Char, {}, U'\n'},
        {Token::Kind::Char, {}, U'\t'},
        {Token::Kind::Char, {}, U'\r'},
        {Token::Kind::Char, {}, U'\b'},
        {Token::Kind::Char, {}, U'\a'},
        {Token::Kind::Char, {}, U'\v'},
        {Token::Kind::Char, {}, U'\f'},
        {Token::Kind::Char, {}, U'\0'},
        {Token::Kind::Char, {}, U'\x1B'},
        {Token::Kind::Char, {}, U'\x7F'},
        {Token::Kind::EOF_},
    });

    // Hex character
    check_tokens("#\\x41;", {
        {Token::Kind::Char, {}, U'A'},
        {Token::Kind::EOF_},
    });

    // Delimiter char (e.g., ") should be accepted directly
    check_tokens("#\\)", {
        {Token::Kind::Char, {}, U')'},
        {Token::Kind::EOF_},
    });

    // Unknown name -> InvalidChar
    Lexer lxe(1, "#\\unknown_name");
    auto e = lxe.next_token();
    BOOST_TEST(!e.has_value());
    BOOST_TEST(e.error().kind == LexErrorKind::InvalidChar);
}

BOOST_AUTO_TEST_CASE(numbers_decimal_exponent_and_specials) {
    check_tokens("0 42 -17 +5 3.14 .5 6. e10 1e-3 -2.5e+8", {
        {Token::Kind::Number, {}, {}, {}, NumericToken::Kind::Fixnum, 10, std::string("0")},
        {Token::Kind::Number, {}, {}, {}, NumericToken::Kind::Fixnum, 10, std::string("42")},
        {Token::Kind::Number, {}, {}, {}, NumericToken::Kind::Fixnum, 10, std::string("-17")},
        {Token::Kind::Number, {}, {}, {}, NumericToken::Kind::Fixnum, 10, std::string("+5")},
        {Token::Kind::Number, {}, {}, {}, NumericToken::Kind::Flonum, 10, std::string("3.14")},
        {Token::Kind::Number, {}, {}, {}, NumericToken::Kind::Flonum, 10, std::string(".5")},
        {Token::Kind::Number, {}, {}, {}, NumericToken::Kind::Flonum, 10, std::string("6.")},
        {Token::Kind::Symbol, {}, {}, std::string("e10")},
        {Token::Kind::Number, {}, {}, {}, NumericToken::Kind::Flonum, 10, std::string("1e-3")},
        {Token::Kind::Number, {}, {}, {}, NumericToken::Kind::Flonum, 10, std::string("-2.5e+8")},
        {Token::Kind::EOF_},
    });
}

BOOST_AUTO_TEST_CASE(number_prefixes_and_radices) {
    check_tokens("#xFF #x-1A #o777 #b1011 #d42", {
        {Token::Kind::Number, {}, {}, {}, NumericToken::Kind::Fixnum, 16, std::string("FF")},
        {Token::Kind::Number, {}, {}, {}, NumericToken::Kind::Fixnum, 16, std::string("-1A")},
        {Token::Kind::Number, {}, {}, {}, NumericToken::Kind::Fixnum, 8, std::string("777")},
        {Token::Kind::Number, {}, {}, {}, NumericToken::Kind::Fixnum, 2, std::string("1011")},
        {Token::Kind::Number, {}, {}, {}, NumericToken::Kind::Fixnum, 10, std::string("42")},
        {Token::Kind::EOF_},
    });

    // Invalid numeric in given radix
    Lexer lx1(1, "#b102");
    auto e1 = lx1.next_token();
    BOOST_TEST(!e1.has_value());
    BOOST_TEST(e1.error().kind == LexErrorKind::InvalidNumeric);

    // Missing digits after prefixes
    Lexer lx2(1, "#x ");
    auto e2 = lx2.next_token();
    BOOST_TEST(!e2.has_value());
    BOOST_TEST(e2.error().kind == LexErrorKind::InvalidNumeric);
}

BOOST_AUTO_TEST_CASE(identifiers_bare_and_bar) {
    check_tokens("abc-def? + - list->vector @foo", {
        {Token::Kind::Symbol, {}, {}, std::string("abc-def?")},
        {Token::Kind::Symbol, {}, {}, std::string("+")},
        {Token::Kind::Symbol, {}, {}, std::string("-")},
        {Token::Kind::Symbol, {}, {}, std::string("list->vector")},
        {Token::Kind::Symbol, {}, {}, std::string("@foo")},
        {Token::Kind::EOF_},
    });

    // Bar identifier with spaces and escapes
    check_tokens("|Hello World| |a\\x3B;| |b\\|c|", {
        {Token::Kind::Symbol, {}, {}, std::string("Hello World")},
        {Token::Kind::Symbol, {}, {}, std::string("a;")},
        {Token::Kind::Symbol, {}, {}, std::string("b|c")},
        {Token::Kind::EOF_},
    });

    // Unterminated bar identifier -> error
    Lexer lxe(1, "|no end");
    auto e = lxe.next_token();
    BOOST_TEST(!e.has_value());
    BOOST_TEST(e.error().kind == LexErrorKind::InvalidToken);
}

BOOST_AUTO_TEST_CASE(directives_ignored_and_shebang) {
    // fold-case and no-fold-case are ignored; identifiers preserve case
    check_tokens("#!fold-case FOO |Bar| baz", {
        {Token::Kind::Symbol, {}, {}, std::string("FOO")},
        {Token::Kind::Symbol, {}, {}, std::string("Bar")},
        {Token::Kind::Symbol, {}, {}, std::string("baz")},
        {Token::Kind::EOF_},
    });

    check_tokens("#!fold-case FOO #!no-fold-case BAR", {
        {Token::Kind::Symbol, {}, {}, std::string("FOO")},
        {Token::Kind::Symbol, {}, {}, std::string("BAR")},
        {Token::Kind::EOF_},
    });

    // Shebang at start of file (not equal to fold-case/no-fold-case) should be skipped entirely
    check_tokens("#!usr/bin/env eta\nabc", {
        {Token::Kind::Symbol, {}, {}, std::string("abc")},
        {Token::Kind::EOF_},
    });
}

BOOST_AUTO_TEST_CASE(vector_and_bytevector_starts) {
    check_tokens("#( #u8( ) ]", {
        {Token::Kind::VectorStart},
        {Token::Kind::ByteVectorStart},
        {Token::Kind::RParen},
        {Token::Kind::RBracket},
        {Token::Kind::EOF_},
    });

    // Invalid #u8 form
    Lexer lx(1, "#u9(");
    auto e = lx.next_token();
    BOOST_TEST(!e.has_value());
    BOOST_TEST(e.error().kind == LexErrorKind::InvalidToken);
}

// ============================================================================
// FileId tracking tests
// ============================================================================

BOOST_AUTO_TEST_CASE(file_id_propagated_in_tokens) {
    // Tokens from different "files" should carry distinct FileIds
    const FileId file_a = 10;
    const FileId file_b = 20;

    auto toks_a = lex_all("(foo)", file_a);
    auto toks_b = lex_all("(bar)", file_b);

    // Each token from file_a should have file_id == 10
    for (const auto& t : toks_a) {
        BOOST_CHECK_EQUAL(t.span.file_id, file_a);
    }
    // Each token from file_b should have file_id == 20
    for (const auto& t : toks_b) {
        BOOST_CHECK_EQUAL(t.span.file_id, file_b);
    }
}

BOOST_AUTO_TEST_CASE(file_id_propagated_in_errors) {
    // Lex errors should carry the correct FileId
    const FileId fid = 99;
    Lexer lx(fid, "\"unterminated");
    auto result = lx.next_token();
    BOOST_REQUIRE(!result.has_value());
    BOOST_CHECK_EQUAL(result.error().span.file_id, fid);
}

BOOST_AUTO_TEST_CASE(file_id_zero_is_valid) {
    // FileId 0 is a valid identifier (used by many tests as default)
    auto toks = lex_all("42", 0);
    BOOST_REQUIRE(!toks.empty());
    BOOST_CHECK_EQUAL(toks[0].span.file_id, 0);
}

// ============================================================================
// Special IEEE float literals
// ============================================================================

BOOST_AUTO_TEST_CASE(special_float_literals) {
    check_tokens("+inf.0 -inf.0 +nan.0 -nan.0", {
        {Token::Kind::Number, {}, {}, {}, NumericToken::Kind::Flonum, 10, std::string("+inf.0")},
        {Token::Kind::Number, {}, {}, {}, NumericToken::Kind::Flonum, 10, std::string("-inf.0")},
        {Token::Kind::Number, {}, {}, {}, NumericToken::Kind::Flonum, 10, std::string("+nan.0")},
        {Token::Kind::Number, {}, {}, {}, NumericToken::Kind::Flonum, 10, std::string("-nan.0")},
        {Token::Kind::EOF_},
    });
}

// ============================================================================
// Exactness prefixes (#e, #i) and combined prefixes
// ============================================================================

BOOST_AUTO_TEST_CASE(exactness_prefix_decimal) {
    // #e and #i should be accepted as exactness prefixes
    check_tokens("#e42 #i3.14", {
        {Token::Kind::Number, {}, {}, {}, NumericToken::Kind::Fixnum, 10, std::string("42")},
        {Token::Kind::Number, {}, {}, {}, NumericToken::Kind::Flonum, 10, std::string("3.14")},
        {Token::Kind::EOF_},
    });
}

BOOST_AUTO_TEST_CASE(combined_radix_and_exactness_prefixes) {
    // Combined: radix + exactness in either order
    check_tokens("#e#xFF #x#eFF", {
        {Token::Kind::Number, {}, {}, {}, NumericToken::Kind::Fixnum, 16, std::string("FF")},
        {Token::Kind::Number, {}, {}, {}, NumericToken::Kind::Fixnum, 16, std::string("FF")},
        {Token::Kind::EOF_},
    });

    check_tokens("#i#b1010 #b#i1010", {
        {Token::Kind::Number, {}, {}, {}, NumericToken::Kind::Fixnum, 2, std::string("1010")},
        {Token::Kind::Number, {}, {}, {}, NumericToken::Kind::Fixnum, 2, std::string("1010")},
        {Token::Kind::EOF_},
    });
}

// ============================================================================
// Edge cases in number parsing
// ============================================================================

BOOST_AUTO_TEST_CASE(number_edge_cases) {
    // Leading-dot decimal
    check_tokens(".0 .123", {
        {Token::Kind::Number, {}, {}, {}, NumericToken::Kind::Flonum, 10, std::string(".0")},
        {Token::Kind::Number, {}, {}, {}, NumericToken::Kind::Flonum, 10, std::string(".123")},
        {Token::Kind::EOF_},
    });

    // Trailing-dot decimal
    check_tokens("0. 100.", {
        {Token::Kind::Number, {}, {}, {}, NumericToken::Kind::Flonum, 10, std::string("0.")},
        {Token::Kind::Number, {}, {}, {}, NumericToken::Kind::Flonum, 10, std::string("100.")},
        {Token::Kind::EOF_},
    });

    // Signed zero
    check_tokens("+0 -0", {
        {Token::Kind::Number, {}, {}, {}, NumericToken::Kind::Fixnum, 10, std::string("+0")},
        {Token::Kind::Number, {}, {}, {}, NumericToken::Kind::Fixnum, 10, std::string("-0")},
        {Token::Kind::EOF_},
    });

    // Hex with upper and lower case
    check_tokens("#xABcd #xabCD", {
        {Token::Kind::Number, {}, {}, {}, NumericToken::Kind::Fixnum, 16, std::string("ABcd")},
        {Token::Kind::Number, {}, {}, {}, NumericToken::Kind::Fixnum, 16, std::string("abCD")},
        {Token::Kind::EOF_},
    });
}

BOOST_AUTO_TEST_CASE(number_signed_hex_and_octal) {
    check_tokens("#x+A #o-77", {
        {Token::Kind::Number, {}, {}, {}, NumericToken::Kind::Fixnum, 16, std::string("+A")},
        {Token::Kind::Number, {}, {}, {}, NumericToken::Kind::Fixnum, 8, std::string("-77")},
        {Token::Kind::EOF_},
    });
}

// ============================================================================
// Empty source and whitespace-only inputs
// ============================================================================

BOOST_AUTO_TEST_CASE(empty_and_whitespace_sources) {
    // Empty source should produce only EOF
    check_tokens("", {
        {Token::Kind::EOF_},
    });

    // Whitespace-only source
    check_tokens("   \t\n  \r\n  ", {
        {Token::Kind::EOF_},
    });

    // Comment-only source
    check_tokens("; just a comment\n; and another", {
        {Token::Kind::EOF_},
    });
}

// ============================================================================
// Span tracking (offsets and line/column)
// ============================================================================

BOOST_AUTO_TEST_CASE(span_offsets_are_correct) {
    auto toks = lex_all("(abc 42)");
    // Expected tokens: LParen, Symbol("abc"), Number(42), RParen, EOF
    BOOST_REQUIRE_EQUAL(toks.size(), 5u);

    // LParen at offset 0
    BOOST_CHECK_EQUAL(toks[0].span.start.offset, 0u);
    BOOST_CHECK_EQUAL(toks[0].span.start.line, 1u);
    BOOST_CHECK_EQUAL(toks[0].span.start.column, 1u);

    // Symbol "abc" starts at offset 1
    BOOST_CHECK_EQUAL(toks[1].span.start.offset, 1u);
    BOOST_CHECK_EQUAL(toks[1].span.start.line, 1u);
    BOOST_CHECK_EQUAL(toks[1].span.start.column, 2u);

    // Number 42 starts at offset 5
    BOOST_CHECK_EQUAL(toks[2].span.start.offset, 5u);
    BOOST_CHECK_EQUAL(toks[2].span.start.line, 1u);
    BOOST_CHECK_EQUAL(toks[2].span.start.column, 6u);
}

BOOST_AUTO_TEST_CASE(span_multiline_tracking) {
    auto toks = lex_all("a\nb\nc");
    // Symbols a, b, c on lines 1, 2, 3
    BOOST_REQUIRE_EQUAL(toks.size(), 4u); // a, b, c, EOF

    BOOST_CHECK_EQUAL(toks[0].span.start.line, 1u);
    BOOST_CHECK_EQUAL(toks[0].span.start.column, 1u);

    BOOST_CHECK_EQUAL(toks[1].span.start.line, 2u);
    BOOST_CHECK_EQUAL(toks[1].span.start.column, 1u);

    BOOST_CHECK_EQUAL(toks[2].span.start.line, 3u);
    BOOST_CHECK_EQUAL(toks[2].span.start.column, 1u);
}

// ============================================================================
// Datum comment edge cases
// ============================================================================

BOOST_AUTO_TEST_CASE(datum_comment_skips_nested_form) {
    // Datum comment should skip an entire list
    check_tokens("#;(a b c) 42", {
        {Token::Kind::Number, {}, {}, {}, NumericToken::Kind::Fixnum, 10, std::string("42")},
        {Token::Kind::EOF_},
    });

    // Datum comment skips a string
    check_tokens("#;\"hello\" world", {
        {Token::Kind::Symbol, {}, {}, std::string("world")},
        {Token::Kind::EOF_},
    });

    // Datum comment skips a vector
    check_tokens("#;#(1 2 3) done", {
        {Token::Kind::Symbol, {}, {}, std::string("done")},
        {Token::Kind::EOF_},
    });
}

BOOST_AUTO_TEST_CASE(datum_comment_at_eof_is_error) {
    // #; at EOF with no datum to skip
    Lexer lx(1, "#; ");
    auto result = lx.next_token();
    BOOST_TEST(!result.has_value());
    BOOST_TEST(result.error().kind == LexErrorKind::UnexpectedEOF);
}

BOOST_AUTO_TEST_CASE(datum_comment_on_rparen_is_error) {
    // #; followed by ) is not a valid datum
    Lexer lx(1, "#;)");
    auto result = lx.next_token();
    BOOST_TEST(!result.has_value());
    BOOST_TEST(result.error().kind == LexErrorKind::InvalidDatum);
}

BOOST_AUTO_TEST_CASE(datum_comment_on_rbracket_is_error) {
    // #; followed by ] is not a valid datum
    Lexer lx(1, "#;]");
    auto result = lx.next_token();
    BOOST_TEST(!result.has_value());
    BOOST_TEST(result.error().kind == LexErrorKind::InvalidDatum);
}

BOOST_AUTO_TEST_CASE(datum_comment_on_dot_is_error) {
    // #; followed by . is not a valid datum
    Lexer lx(1, "#;.");
    auto result = lx.next_token();
    BOOST_TEST(!result.has_value());
    BOOST_TEST(result.error().kind == LexErrorKind::InvalidDatum);
}

// ============================================================================
// Block comment edge cases
// ============================================================================

BOOST_AUTO_TEST_CASE(unterminated_block_comment_error) {
    Lexer lx(1, "#| unterminated block comment");
    auto result = lx.next_token();
    BOOST_TEST(!result.has_value());
    BOOST_TEST(result.error().kind == LexErrorKind::UnterminatedComment);
}

BOOST_AUTO_TEST_CASE(deeply_nested_block_comment) {
    // Multiple levels of nesting
    check_tokens("#| a #| b #| c |# d |# e |# 99", {
        {Token::Kind::Number, {}, {}, {}, NumericToken::Kind::Fixnum, 10, std::string("99")},
        {Token::Kind::EOF_},
    });
}

// ============================================================================
// String edge cases
// ============================================================================

BOOST_AUTO_TEST_CASE(empty_string) {
    check_tokens("\"\"", {
        {Token::Kind::String, {}, {}, std::string("")},
        {Token::Kind::EOF_},
    });
}

BOOST_AUTO_TEST_CASE(string_all_escapes) {
    // Test all standard escape characters
    check_tokens("\"\\a\\b\\t\\n\\r\\\\\\\"\"", {
        {Token::Kind::String, {}, {}, std::string("\a\b\t\n\r\\\"")},
        {Token::Kind::EOF_},
    });
}

BOOST_AUTO_TEST_CASE(string_invalid_escape) {
    Lexer lx(1, "\"\\z\"");
    auto result = lx.next_token();
    BOOST_TEST(!result.has_value());
    BOOST_TEST(result.error().kind == LexErrorKind::InvalidToken);
}

BOOST_AUTO_TEST_CASE(string_eof_after_backslash) {
    Lexer lx(1, "\"abc\\");
    auto result = lx.next_token();
    BOOST_TEST(!result.has_value());
    BOOST_TEST(result.error().kind == LexErrorKind::UnterminatedString);
}

BOOST_AUTO_TEST_CASE(string_hex_escape_multi_char) {
    // Multiple hex escapes in one string
    check_tokens("\"\\x48;\\x65;\\x6C;\\x6C;\\x6F;\"", {
        {Token::Kind::String, {}, {}, std::string("Hello")},
        {Token::Kind::EOF_},
    });
}

// ============================================================================
// Character edge cases
// ============================================================================

BOOST_AUTO_TEST_CASE(char_single_ascii) {
    check_tokens("#\\a #\\Z #\\0 #\\~", {
        {Token::Kind::Char, {}, U'a'},
        {Token::Kind::Char, {}, U'Z'},
        {Token::Kind::Char, {}, U'0'},
        {Token::Kind::Char, {}, U'~'},
        {Token::Kind::EOF_},
    });
}

BOOST_AUTO_TEST_CASE(char_hex_unicode) {
    // Unicode lambda character U+03BB
    check_tokens("#\\x3BB;", {
        {Token::Kind::Char, {}, static_cast<char32_t>(0x3BB)},
        {Token::Kind::EOF_},
    });
}

BOOST_AUTO_TEST_CASE(char_hex_surrogate_is_error) {
    // Surrogate code points are invalid
    Lexer lx(1, "#\\xD800;");
    auto result = lx.next_token();
    BOOST_TEST(!result.has_value());
    BOOST_TEST(result.error().kind == LexErrorKind::InvalidCodePoint);
}

// ============================================================================
// Identifier edge cases
// ============================================================================

BOOST_AUTO_TEST_CASE(identifier_special_initial_chars) {
    check_tokens("!predicate $var %internal &rest *glob /path :keyword <less =eq >more ?query ^power _private ~approx", {
        {Token::Kind::Symbol, {}, {}, std::string("!predicate")},
        {Token::Kind::Symbol, {}, {}, std::string("$var")},
        {Token::Kind::Symbol, {}, {}, std::string("%internal")},
        {Token::Kind::Symbol, {}, {}, std::string("&rest")},
        {Token::Kind::Symbol, {}, {}, std::string("*glob")},
        {Token::Kind::Symbol, {}, {}, std::string("/path")},
        {Token::Kind::Symbol, {}, {}, std::string(":keyword")},
        {Token::Kind::Symbol, {}, {}, std::string("<less")},
        {Token::Kind::Symbol, {}, {}, std::string("=eq")},
        {Token::Kind::Symbol, {}, {}, std::string(">more")},
        {Token::Kind::Symbol, {}, {}, std::string("?query")},
        {Token::Kind::Symbol, {}, {}, std::string("^power")},
        {Token::Kind::Symbol, {}, {}, std::string("_private")},
        {Token::Kind::Symbol, {}, {}, std::string("~approx")},
        {Token::Kind::EOF_},
    });
}

BOOST_AUTO_TEST_CASE(identifier_with_digits_and_dots) {
    check_tokens("x1 foo2bar a.b", {
        {Token::Kind::Symbol, {}, {}, std::string("x1")},
        {Token::Kind::Symbol, {}, {}, std::string("foo2bar")},
        {Token::Kind::Symbol, {}, {}, std::string("a.b")},
        {Token::Kind::EOF_},
    });
}

BOOST_AUTO_TEST_CASE(bar_identifier_empty) {
    check_tokens("||", {
        {Token::Kind::Symbol, {}, {}, std::string("")},
        {Token::Kind::EOF_},
    });
}

BOOST_AUTO_TEST_CASE(bar_identifier_with_newlines_and_specials) {
    // Bar identifiers can contain any characters including newlines
    check_tokens("|foo bar|", {
        {Token::Kind::Symbol, {}, {}, std::string("foo bar")},
        {Token::Kind::EOF_},
    });
}

// ============================================================================
// Comma and CommaAt disambiguation
// ============================================================================

BOOST_AUTO_TEST_CASE(comma_at_vs_comma_followed_by_at) {
    // ,@ should be a single CommaAt token
    check_tokens(",@x", {
        {Token::Kind::CommaAt},
        {Token::Kind::Symbol, {}, {}, std::string("x")},
        {Token::Kind::EOF_},
    });

    // , followed by something not @ should be just Comma
    check_tokens(",x", {
        {Token::Kind::Comma},
        {Token::Kind::Symbol, {}, {}, std::string("x")},
        {Token::Kind::EOF_},
    });
}

// ============================================================================
// Token adjacency and delimiter behavior
// ============================================================================

BOOST_AUTO_TEST_CASE(tokens_adjacent_to_delimiters) {
    // Tokens immediately next to parens and brackets
    check_tokens("(foo)(bar)[baz]", {
        {Token::Kind::LParen},
        {Token::Kind::Symbol, {}, {}, std::string("foo")},
        {Token::Kind::RParen},
        {Token::Kind::LParen},
        {Token::Kind::Symbol, {}, {}, std::string("bar")},
        {Token::Kind::RParen},
        {Token::Kind::LBracket},
        {Token::Kind::Symbol, {}, {}, std::string("baz")},
        {Token::Kind::RBracket},
        {Token::Kind::EOF_},
    });
}

BOOST_AUTO_TEST_CASE(number_before_paren) {
    // Number immediately before closing paren
    check_tokens("(42)", {
        {Token::Kind::LParen},
        {Token::Kind::Number, {}, {}, {}, NumericToken::Kind::Fixnum, 10, std::string("42")},
        {Token::Kind::RParen},
        {Token::Kind::EOF_},
    });
}

// ============================================================================
// Multiple tokens in sequence
// ============================================================================

BOOST_AUTO_TEST_CASE(complex_expression) {
    check_tokens("(define (fact n) (if (= n 0) 1 (* n (fact (- n 1)))))", {
        {Token::Kind::LParen},
        {Token::Kind::Symbol, {}, {}, std::string("define")},
        {Token::Kind::LParen},
        {Token::Kind::Symbol, {}, {}, std::string("fact")},
        {Token::Kind::Symbol, {}, {}, std::string("n")},
        {Token::Kind::RParen},
        {Token::Kind::LParen},
        {Token::Kind::Symbol, {}, {}, std::string("if")},
        {Token::Kind::LParen},
        {Token::Kind::Symbol, {}, {}, std::string("=")},
        {Token::Kind::Symbol, {}, {}, std::string("n")},
        {Token::Kind::Number, {}, {}, {}, NumericToken::Kind::Fixnum, 10, std::string("0")},
        {Token::Kind::RParen},
        {Token::Kind::Number, {}, {}, {}, NumericToken::Kind::Fixnum, 10, std::string("1")},
        {Token::Kind::LParen},
        {Token::Kind::Symbol, {}, {}, std::string("*")},
        {Token::Kind::Symbol, {}, {}, std::string("n")},
        {Token::Kind::LParen},
        {Token::Kind::Symbol, {}, {}, std::string("fact")},
        {Token::Kind::LParen},
        {Token::Kind::Symbol, {}, {}, std::string("-")},
        {Token::Kind::Symbol, {}, {}, std::string("n")},
        {Token::Kind::Number, {}, {}, {}, NumericToken::Kind::Fixnum, 10, std::string("1")},
        {Token::Kind::RParen},
        {Token::Kind::RParen},
        {Token::Kind::RParen},
        {Token::Kind::RParen},
        {Token::Kind::RParen},
        {Token::Kind::EOF_},
    });
}

BOOST_AUTO_TEST_SUITE_END()
