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

BOOST_AUTO_TEST_SUITE_END()
