#include <boost/test/unit_test.hpp>

#include <eta/reader/parser.h>
#include <eta/reader/lexer.h>
#include <string>
#include <vector>
#include <variant>

using namespace eta::reader::parser;
using namespace eta::reader::lexer;

namespace {


// Helper to parse a single datum from source
std::expected<SExprPtr, ReaderError> parse_one(const std::string& src, FileId fid = 0) {
    Lexer lexer(fid, src);
    Parser parser(lexer);
    return parser.parse_datum();
}

// Helper to parse all top-level forms from source
std::expected<std::vector<SExprPtr>, ReaderError> parse_all(const std::string& src, FileId fid = 0) {
    Lexer lexer(fid, src);
    Parser parser(lexer);
    return parser.parse_toplevel();
}

// Helper to parse with strict quasiquote mode
std::expected<SExprPtr, ReaderError> parse_one_strict_qq(const std::string& src, FileId fid = 0) {
    Lexer lexer(fid, src);
    Parser parser(lexer, true);
    return parser.parse_datum();
}

// Verify an SExpr is a specific type and extract it
template<typename T>
const T* expect_type(const SExprPtr& expr) {
    BOOST_REQUIRE(expr != nullptr);
    const T* result = expr->as<T>();
    BOOST_REQUIRE_MESSAGE(result != nullptr, "Expected different SExpr type");
    return result;
}

void expect_error(const std::expected<SExprPtr, ReaderError>& result, ParseErrorKind expected_kind) {
    BOOST_REQUIRE_MESSAGE(!result.has_value(), "Expected parse error but got success");
    BOOST_REQUIRE_MESSAGE(std::holds_alternative<ParseError>(result.error()), "Expected ParseError but got LexError");
    BOOST_CHECK_EQUAL(to_string(std::get<ParseError>(result.error()).kind), to_string(expected_kind));
}

void expect_error_toplevel(const std::expected<std::vector<SExprPtr>, ReaderError>& result, ParseErrorKind expected_kind) {
    BOOST_REQUIRE_MESSAGE(!result.has_value(), "Expected parse error but got success");
    BOOST_REQUIRE_MESSAGE(std::holds_alternative<ParseError>(result.error()), "Expected ParseError but got LexError");
    BOOST_CHECK_EQUAL(to_string(std::get<ParseError>(result.error()).kind), to_string(expected_kind));
}

} // anonymous namespace

BOOST_AUTO_TEST_SUITE(parser_tests)

BOOST_AUTO_TEST_CASE(parse_empty_list_as_nil) {
    const auto result = parse_one("()");
    BOOST_REQUIRE(result.has_value());
    const auto* list = expect_type<List>(*result);
    BOOST_CHECK(list->elems.empty());
    BOOST_CHECK(!list->dotted);
}

BOOST_AUTO_TEST_CASE(parse_boolean_true) {
    const auto result = parse_one("#t");
    BOOST_REQUIRE(result.has_value());
    const auto* b = expect_type<Bool>(*result);
    BOOST_CHECK_EQUAL(b->value, true);
}

BOOST_AUTO_TEST_CASE(parse_boolean_false) {
    const auto result = parse_one("#f");
    BOOST_REQUIRE(result.has_value());
    const auto* b = expect_type<Bool>(*result);
    BOOST_CHECK_EQUAL(b->value, false);
}

BOOST_AUTO_TEST_CASE(parse_boolean_true_long) {
    const auto result = parse_one("#true");
    BOOST_REQUIRE(result.has_value());
    const auto* b = expect_type<Bool>(*result);
    BOOST_CHECK_EQUAL(b->value, true);
}

BOOST_AUTO_TEST_CASE(parse_boolean_false_long) {
    const auto result = parse_one("#false");
    BOOST_REQUIRE(result.has_value());
    const auto* b = expect_type<Bool>(*result);
    BOOST_CHECK_EQUAL(b->value, false);
}

BOOST_AUTO_TEST_CASE(parse_char_simple) {
    const auto result = parse_one("#\\a");
    BOOST_REQUIRE(result.has_value());
    const auto* c = expect_type<Char>(*result);
    BOOST_CHECK(c->value == U'a');
}

BOOST_AUTO_TEST_CASE(parse_char_newline) {
    const auto result = parse_one("#\\newline");
    BOOST_REQUIRE(result.has_value());
    const auto* c = expect_type<Char>(*result);
    BOOST_CHECK(c->value == U'\n');
}

BOOST_AUTO_TEST_CASE(parse_char_space) {
    const auto result = parse_one("#\\space");
    BOOST_REQUIRE(result.has_value());
    const auto* c = expect_type<Char>(*result);
    BOOST_CHECK(c->value == U' ');
}

BOOST_AUTO_TEST_CASE(parse_char_hex) {
    const auto result = parse_one("#\\x41");
    BOOST_REQUIRE(result.has_value());
    const auto* c = expect_type<Char>(*result);
    BOOST_CHECK(c->value == U'A');
}

BOOST_AUTO_TEST_CASE(parse_string_simple) {
    const auto result = parse_one("\"hello\"");
    BOOST_REQUIRE(result.has_value());
    const auto* s = expect_type<String>(*result);
    BOOST_CHECK_EQUAL(s->value, "hello");
}

BOOST_AUTO_TEST_CASE(parse_string_empty) {
    auto result = parse_one("\"\"");
    BOOST_REQUIRE(result.has_value());
    const auto* s = expect_type<String>(*result);
    BOOST_CHECK_EQUAL(s->value, "");
}

BOOST_AUTO_TEST_CASE(parse_string_with_escapes) {
    auto result = parse_one("\"hello\\nworld\"");
    BOOST_REQUIRE(result.has_value());
    const auto* s = expect_type<String>(*result);
    BOOST_CHECK_EQUAL(s->value, "hello\nworld");
}

BOOST_AUTO_TEST_CASE(parse_string_with_unicode) {
    auto result = parse_one("\"hello \\x3BB;\"");
    BOOST_REQUIRE(result.has_value());
    const auto* s = expect_type<String>(*result);
    // λ is U+03BB
    BOOST_CHECK(s->value.find("\xCE\xBB") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(parse_symbol_simple) {
    const auto result = parse_one("foo");
    BOOST_REQUIRE(result.has_value());
    const auto* sym = expect_type<Symbol>(*result);
    BOOST_CHECK_EQUAL(sym->name, "foo");
}

BOOST_AUTO_TEST_CASE(parse_symbol_with_special_chars) {
    const auto result = parse_one("foo-bar");
    BOOST_REQUIRE(result.has_value());
    const auto* sym = expect_type<Symbol>(*result);
    BOOST_CHECK_EQUAL(sym->name, "foo-bar");
}

BOOST_AUTO_TEST_CASE(parse_symbol_plus) {
    const auto result = parse_one("+");
    BOOST_REQUIRE(result.has_value());
    const auto* sym = expect_type<Symbol>(*result);
    BOOST_CHECK_EQUAL(sym->name, "+");
}

BOOST_AUTO_TEST_CASE(parse_symbol_minus) {
    const auto result = parse_one("-");
    BOOST_REQUIRE(result.has_value());
    const auto* sym = expect_type<Symbol>(*result);
    BOOST_CHECK_EQUAL(sym->name, "-");
}

BOOST_AUTO_TEST_CASE(parse_symbol_ellipsis) {
    const auto result = parse_one("...");
    BOOST_REQUIRE(result.has_value());
    const auto* sym = expect_type<Symbol>(*result);
    BOOST_CHECK_EQUAL(sym->name, "...");
}

BOOST_AUTO_TEST_CASE(parse_symbol_bar_delimited) {
    const auto result = parse_one("|hello world|");
    BOOST_REQUIRE(result.has_value());
    const auto* sym = expect_type<Symbol>(*result);
    BOOST_CHECK_EQUAL(sym->name, "hello world");
}

BOOST_AUTO_TEST_CASE(parse_number_integer) {
    const auto result = parse_one("42");
    BOOST_REQUIRE(result.has_value());
    const auto* n = expect_type<Number>(*result);
    const auto* val = std::get_if<int64_t>(&n->value);
    BOOST_REQUIRE(val != nullptr);
    BOOST_CHECK_EQUAL(*val, 42);
}

BOOST_AUTO_TEST_CASE(parse_number_negative) {
    const auto result = parse_one("-42");
    BOOST_REQUIRE(result.has_value());
    const auto* n = expect_type<Number>(*result);
    const auto* val = std::get_if<int64_t>(&n->value);
    BOOST_REQUIRE(val != nullptr);
    BOOST_CHECK_EQUAL(*val, -42);
}

BOOST_AUTO_TEST_CASE(parse_number_float) {
    const auto result = parse_one("3.14");
    BOOST_REQUIRE(result.has_value());
    const auto* n = expect_type<Number>(*result);
    const auto* val = std::get_if<double>(&n->value);
    BOOST_REQUIRE(val != nullptr);
    BOOST_CHECK_CLOSE(*val, 3.14, 0.0001);
}

BOOST_AUTO_TEST_CASE(parse_number_hex) {
    const auto result = parse_one("#xFF");
    BOOST_REQUIRE(result.has_value());
    const auto* n = expect_type<Number>(*result);
    const auto* val = std::get_if<int64_t>(&n->value);
    BOOST_REQUIRE(val != nullptr);
    BOOST_CHECK_EQUAL(*val, 255);
}

BOOST_AUTO_TEST_CASE(parse_number_binary) {
    const auto result = parse_one("#b1010");
    BOOST_REQUIRE(result.has_value());
    const auto* n = expect_type<Number>(*result);
    const auto* val = std::get_if<int64_t>(&n->value);
    BOOST_REQUIRE(val != nullptr);
    BOOST_CHECK_EQUAL(*val, 10);
}

BOOST_AUTO_TEST_CASE(parse_number_octal) {
    const auto result = parse_one("#o777");
    BOOST_REQUIRE(result.has_value());
    const auto* n = expect_type<Number>(*result);
    const auto* val = std::get_if<int64_t>(&n->value);
    BOOST_REQUIRE(val != nullptr);
    BOOST_CHECK_EQUAL(*val, 511);
}

BOOST_AUTO_TEST_CASE(parse_number_scientific) {
    const auto result = parse_one("1e10");
    BOOST_REQUIRE(result.has_value());
    const auto* n = expect_type<Number>(*result);
    const auto* val = std::get_if<double>(&n->value);
    BOOST_REQUIRE(val != nullptr);
}

BOOST_AUTO_TEST_CASE(parse_list_simple) {
    const auto result = parse_one("(a b c)");
    BOOST_REQUIRE(result.has_value());
    const auto* list = expect_type<List>(*result);
    BOOST_CHECK_EQUAL(list->elems.size(), 3);
    BOOST_CHECK(!list->dotted);
}

BOOST_AUTO_TEST_CASE(parse_list_nested) {
    const auto result = parse_one("(a (b c) d)");
    BOOST_REQUIRE(result.has_value());
    const auto* list = expect_type<List>(*result);
    BOOST_CHECK_EQUAL(list->elems.size(), 3);
    const auto* inner = list->elems[1]->as<List>();
    BOOST_REQUIRE(inner != nullptr);
    BOOST_CHECK_EQUAL(inner->elems.size(), 2);
}

BOOST_AUTO_TEST_CASE(parse_list_dotted) {
    const auto result = parse_one("(a b . c)");
    BOOST_REQUIRE(result.has_value());
    const auto* list = expect_type<List>(*result);
    BOOST_CHECK_EQUAL(list->elems.size(), 2);
    BOOST_CHECK(list->dotted);
    BOOST_REQUIRE(list->tail != nullptr);
    const auto* tail_sym = list->tail->as<Symbol>();
    BOOST_REQUIRE(tail_sym != nullptr);
    BOOST_CHECK_EQUAL(tail_sym->name, "c");
}

BOOST_AUTO_TEST_CASE(parse_list_cons_cell) {
    const auto result = parse_one("(a . b)");
    BOOST_REQUIRE(result.has_value());
    const auto* list = expect_type<List>(*result);
    BOOST_CHECK_EQUAL(list->elems.size(), 1);
    BOOST_CHECK(list->dotted);
}

BOOST_AUTO_TEST_CASE(parse_list_with_brackets) {
    const auto result = parse_one("[a b c]");
    BOOST_REQUIRE(result.has_value());
    const auto* list = expect_type<List>(*result);
    BOOST_CHECK_EQUAL(list->elems.size(), 3);
}

BOOST_AUTO_TEST_CASE(parse_list_mixed_brackets) {
    // Square brackets should work like parens
    const auto result = parse_one("(a [b c] d)");
    BOOST_REQUIRE(result.has_value());
    const auto* list = expect_type<List>(*result);
    BOOST_CHECK_EQUAL(list->elems.size(), 3);
}

BOOST_AUTO_TEST_CASE(parse_vector_simple) {
    const auto result = parse_one("#(1 2 3)");
    BOOST_REQUIRE(result.has_value());
    const auto* vec = expect_type<Vector>(*result);
    BOOST_CHECK_EQUAL(vec->elems.size(), 3);
}

BOOST_AUTO_TEST_CASE(parse_vector_empty) {
    const auto result = parse_one("#()");
    BOOST_REQUIRE(result.has_value());
    const auto* vec = expect_type<Vector>(*result);
    BOOST_CHECK(vec->elems.empty());
}

BOOST_AUTO_TEST_CASE(parse_vector_nested) {
    const auto result = parse_one("#(a #(b c) d)");
    BOOST_REQUIRE(result.has_value());
    const auto* vec = expect_type<Vector>(*result);
    BOOST_CHECK_EQUAL(vec->elems.size(), 3);
    const auto* inner = vec->elems[1]->as<Vector>();
    BOOST_REQUIRE(inner != nullptr);
    BOOST_CHECK_EQUAL(inner->elems.size(), 2);
}

BOOST_AUTO_TEST_CASE(parse_bytevector_simple) {
    const auto result = parse_one("#u8(1 2 255)");
    BOOST_REQUIRE(result.has_value());
    const auto* bv = expect_type<ByteVector>(*result);
    BOOST_CHECK_EQUAL(bv->bytes.size(), 3);
    BOOST_CHECK_EQUAL(bv->bytes[0], 1);
    BOOST_CHECK_EQUAL(bv->bytes[1], 2);
    BOOST_CHECK_EQUAL(bv->bytes[2], 255);
}

BOOST_AUTO_TEST_CASE(parse_bytevector_empty) {
    const auto result = parse_one("#u8()");
    BOOST_REQUIRE(result.has_value());
    const auto* bv = expect_type<ByteVector>(*result);
    BOOST_CHECK(bv->bytes.empty());
}

BOOST_AUTO_TEST_CASE(parse_bytevector_hex_values) {
    const auto result = parse_one("#u8(#xFF #x00 #x7F)");
    BOOST_REQUIRE(result.has_value());
    const auto* bv = expect_type<ByteVector>(*result);
    BOOST_CHECK_EQUAL(bv->bytes.size(), 3);
    BOOST_CHECK_EQUAL(bv->bytes[0], 255);
    BOOST_CHECK_EQUAL(bv->bytes[1], 0);
    BOOST_CHECK_EQUAL(bv->bytes[2], 127);
}

BOOST_AUTO_TEST_CASE(parse_quote) {
    const auto result = parse_one("'foo");
    BOOST_REQUIRE(result.has_value());
    const auto* rf = expect_type<ReaderForm>(*result);
    BOOST_CHECK(rf->kind == QuoteKind::Quote);
    const auto* sym = rf->expr->as<Symbol>();
    BOOST_REQUIRE(sym != nullptr);
    BOOST_CHECK_EQUAL(sym->name, "foo");
}

BOOST_AUTO_TEST_CASE(parse_quote_list) {
    const auto result = parse_one("'(a b c)");
    BOOST_REQUIRE(result.has_value());
    const auto* rf = expect_type<ReaderForm>(*result);
    BOOST_CHECK(rf->kind == QuoteKind::Quote);
    const auto* list = rf->expr->as<List>();
    BOOST_REQUIRE(list != nullptr);
    BOOST_CHECK_EQUAL(list->elems.size(), 3);
}

BOOST_AUTO_TEST_CASE(parse_quasiquote) {
    const auto result = parse_one("`foo");
    BOOST_REQUIRE(result.has_value());
    const auto* rf = expect_type<ReaderForm>(*result);
    BOOST_CHECK(rf->kind == QuoteKind::Quasiquote);
}

BOOST_AUTO_TEST_CASE(parse_unquote) {
    const auto result = parse_one("`,x");
    BOOST_REQUIRE(result.has_value());
    const auto* rf = expect_type<ReaderForm>(*result);
    BOOST_CHECK(rf->kind == QuoteKind::Quasiquote);
    const auto* inner = rf->expr->as<ReaderForm>();
    BOOST_REQUIRE(inner != nullptr);
    BOOST_CHECK(inner->kind == QuoteKind::Unquote);
}

BOOST_AUTO_TEST_CASE(parse_unquote_splicing) {
    const auto result = parse_one("`(a ,@b c)");
    BOOST_REQUIRE(result.has_value());
    const auto* rf = expect_type<ReaderForm>(*result);
    BOOST_CHECK(rf->kind == QuoteKind::Quasiquote);
    const auto* list = rf->expr->as<List>();
    BOOST_REQUIRE(list != nullptr);
    const auto* splice = list->elems[1]->as<ReaderForm>();
    BOOST_REQUIRE(splice != nullptr);
    BOOST_CHECK(splice->kind == QuoteKind::UnquoteSplicing);
}

BOOST_AUTO_TEST_CASE(parse_nested_quasiquote) {
    const auto result = parse_one("``,,x");
    BOOST_REQUIRE(result.has_value());
    const auto* rf = expect_type<ReaderForm>(*result);
    BOOST_CHECK(rf->kind == QuoteKind::Quasiquote);
}

BOOST_AUTO_TEST_CASE(parse_toplevel_multiple) {
    const auto result = parse_all("a b c");
    BOOST_REQUIRE(result.has_value());
    BOOST_CHECK_EQUAL(result->size(), 3);
}

BOOST_AUTO_TEST_CASE(parse_toplevel_empty) {
    const auto result = parse_all("");
    BOOST_REQUIRE(result.has_value());
    BOOST_CHECK(result->empty());
}

BOOST_AUTO_TEST_CASE(parse_toplevel_with_comments) {
    const auto result = parse_all("; comment\na\n; another\nb");
    BOOST_REQUIRE(result.has_value());
    BOOST_CHECK_EQUAL(result->size(), 2);
}

BOOST_AUTO_TEST_CASE(parse_toplevel_mixed_forms) {
    const auto result = parse_all("42 \"hello\" (a b) #(1 2)");
    BOOST_REQUIRE(result.has_value());
    BOOST_CHECK_EQUAL(result->size(), 4);
}

BOOST_AUTO_TEST_CASE(error_unclosed_list) {
    const auto result = parse_one("(a b c");
    expect_error(result, ParseErrorKind::UnclosedList);
}

BOOST_AUTO_TEST_CASE(error_unclosed_nested_list) {
    const auto result = parse_one("(a (b c)");
    expect_error(result, ParseErrorKind::UnclosedList);
}

BOOST_AUTO_TEST_CASE(error_unexpected_closing_paren) {
    const auto result = parse_one(")");
    expect_error(result, ParseErrorKind::UnexpectedClosingDelimiter);
}

BOOST_AUTO_TEST_CASE(error_unexpected_closing_bracket) {
    const auto result = parse_one("]");
    expect_error(result, ParseErrorKind::UnexpectedClosingDelimiter);
}

BOOST_AUTO_TEST_CASE(error_mismatched_delimiters) {
    const auto result = parse_one("(a b]");
    expect_error(result, ParseErrorKind::UnclosedList);
}

BOOST_AUTO_TEST_CASE(error_dot_at_list_start) {
    const auto result = parse_one("(. a)");
    expect_error(result, ParseErrorKind::DotAtListStart);
}

BOOST_AUTO_TEST_CASE(error_multiple_dots) {
    const auto result = parse_one("(a . b . c)");
    expect_error(result, ParseErrorKind::MultipleDotsInDottedList);
}

BOOST_AUTO_TEST_CASE(error_dot_without_tail) {
    const auto result = parse_one("(a b .)");
    // Should be unclosed or misplaced dot
    BOOST_CHECK(!result.has_value());
}

BOOST_AUTO_TEST_CASE(error_dot_with_multiple_tail) {
    const auto result = parse_one("(a . b c)");
    // After dot, only one element allowed before closing
    expect_error(result, ParseErrorKind::MisplacedDot);
}

BOOST_AUTO_TEST_CASE(error_unclosed_vector) {
    const auto result = parse_one("#(1 2 3");
    expect_error(result, ParseErrorKind::UnclosedVector);
}

BOOST_AUTO_TEST_CASE(error_dot_in_vector) {
    const auto result = parse_one("#(a . b)");
    expect_error(result, ParseErrorKind::DotInVector);
}

BOOST_AUTO_TEST_CASE(error_bytevector_non_integer) {
    const auto result = parse_one("#u8(1 foo 3)");
    expect_error(result, ParseErrorKind::ByteVectorNonInteger);
}

BOOST_AUTO_TEST_CASE(error_bytevector_out_of_range) {
    const auto result = parse_one("#u8(256)");
    expect_error(result, ParseErrorKind::InvalidByteLiteral);
}

BOOST_AUTO_TEST_CASE(error_bytevector_negative) {
    const auto result = parse_one("#u8(-1)");
    expect_error(result, ParseErrorKind::InvalidByteLiteral);
}

BOOST_AUTO_TEST_CASE(error_bytevector_float) {
    const auto result = parse_one("#u8(3.14)");
    expect_error(result, ParseErrorKind::InvalidByteLiteral);
}

BOOST_AUTO_TEST_CASE(error_unquote_outside_quasiquote_strict) {
    const auto result = parse_one_strict_qq(",x");
    expect_error(result, ParseErrorKind::UnquoteOutsideQuasiquote);
}

BOOST_AUTO_TEST_CASE(error_unquote_splicing_outside_quasiquote_strict) {
    const auto result = parse_one_strict_qq(",@x");
    expect_error(result, ParseErrorKind::UnquoteOutsideQuasiquote);
}

// In non-strict mode, unquote outside quasiquote should still work
BOOST_AUTO_TEST_CASE(unquote_outside_quasiquote_non_strict) {
    const auto result = parse_one(",x");
    BOOST_REQUIRE(result.has_value());
    const auto* rf = expect_type<ReaderForm>(*result);
    BOOST_CHECK(rf->kind == QuoteKind::Unquote);
}

BOOST_AUTO_TEST_CASE(error_unexpected_eof_quote) {
    const auto result = parse_one("'");
    expect_error(result, ParseErrorKind::UnexpectedEOF);
}

BOOST_AUTO_TEST_CASE(error_unexpected_eof_quasiquote) {
    const auto result = parse_one("`");
    expect_error(result, ParseErrorKind::UnexpectedEOF);
}

BOOST_AUTO_TEST_CASE(error_unexpected_eof_unquote) {
    const auto result = parse_one(",");
    expect_error(result, ParseErrorKind::UnexpectedEOF);
}

BOOST_AUTO_TEST_CASE(span_single_token) {
    const auto result = parse_one("foo");
    BOOST_REQUIRE(result.has_value());
    const auto span = (*result)->span();
    BOOST_CHECK_EQUAL(span.start.offset, 0);
    BOOST_CHECK_EQUAL(span.end.offset, 3);
}

BOOST_AUTO_TEST_CASE(span_list) {
    const auto result = parse_one("(a b)");
    BOOST_REQUIRE(result.has_value());
    const auto span = (*result)->span();
    BOOST_CHECK_EQUAL(span.start.offset, 0);
    BOOST_CHECK_EQUAL(span.end.offset, 5);
}

BOOST_AUTO_TEST_CASE(span_preserves_file_id) {
    const FileId test_file_id = 42;
    const auto result = parse_one("foo", test_file_id);
    BOOST_REQUIRE(result.has_value());
    BOOST_CHECK_EQUAL((*result)->span().file_id, test_file_id);
}

BOOST_AUTO_TEST_CASE(parse_deeply_nested) {
    const auto result = parse_one("((((((a))))))");
    BOOST_REQUIRE(result.has_value());
    const List* current = expect_type<List>(*result);
    for (int i = 0; i < 5; ++i) {
        BOOST_REQUIRE_EQUAL(current->elems.size(), 1);
        current = current->elems[0]->as<List>();
        BOOST_REQUIRE(current != nullptr);
    }
    BOOST_REQUIRE_EQUAL(current->elems.size(), 1);
    const auto* sym = current->elems[0]->as<Symbol>();
    BOOST_REQUIRE(sym != nullptr);
    BOOST_CHECK_EQUAL(sym->name, "a");
}

BOOST_AUTO_TEST_CASE(parse_whitespace_only) {
    const auto result = parse_all("   \n\t  ");
    BOOST_REQUIRE(result.has_value());
    BOOST_CHECK(result->empty());
}

BOOST_AUTO_TEST_CASE(parse_comment_only) {
    const auto result = parse_all("; just a comment");
    BOOST_REQUIRE(result.has_value());
    BOOST_CHECK(result->empty());
}

BOOST_AUTO_TEST_CASE(parse_datum_returns_null_on_empty) {
    // parse_datum on empty input should return error or empty
    Lexer lexer(0, "");
    Parser parser(lexer);
    const auto result = parser.parse_datum();
    // Empty input means EOF, should return error
    BOOST_CHECK(!result.has_value());
}

BOOST_AUTO_TEST_CASE(parse_multiple_datums_sequentially) {
    Lexer lexer(0, "a b c");
    Parser parser(lexer);

    auto r1 = parser.parse_datum();
    BOOST_REQUIRE(r1.has_value());
    BOOST_CHECK_EQUAL((*r1)->as<Symbol>()->name, "a");

    auto r2 = parser.parse_datum();
    BOOST_REQUIRE(r2.has_value());
    BOOST_CHECK_EQUAL((*r2)->as<Symbol>()->name, "b");

    auto r3 = parser.parse_datum();
    BOOST_REQUIRE(r3.has_value());
    BOOST_CHECK_EQUAL((*r3)->as<Symbol>()->name, "c");
}

BOOST_AUTO_TEST_CASE(parse_list_with_all_types) {
    const auto result = parse_one("(42 3.14 \"str\" #t #\\a sym)");
    BOOST_REQUIRE(result.has_value());
    const auto* list = expect_type<List>(*result);
    BOOST_CHECK_EQUAL(list->elems.size(), 6);

    BOOST_CHECK(list->elems[0]->is<Number>());
    BOOST_CHECK(list->elems[1]->is<Number>());
    BOOST_CHECK(list->elems[2]->is<String>());
    BOOST_CHECK(list->elems[3]->is<Bool>());
    BOOST_CHECK(list->elems[4]->is<Char>());
    BOOST_CHECK(list->elems[5]->is<Symbol>());
}

BOOST_AUTO_TEST_CASE(parse_complex_s_expression) {
    const auto result = parse_one("(define (factorial n) (if (= n 0) 1 (* n (factorial (- n 1)))))");
    BOOST_REQUIRE(result.has_value());
    const auto* list = expect_type<List>(*result);
    BOOST_CHECK_EQUAL(list->elems.size(), 3);

    // First element should be 'define' symbol
    const auto* define_sym = list->elems[0]->as<Symbol>();
    BOOST_REQUIRE(define_sym != nullptr);
    BOOST_CHECK_EQUAL(define_sym->name, "define");
}

BOOST_AUTO_TEST_CASE(sexpr_is_method) {
    const auto result = parse_one("42");
    BOOST_REQUIRE(result.has_value());
    BOOST_CHECK((*result)->is<Number>());
    BOOST_CHECK(!(*result)->is<Symbol>());
    BOOST_CHECK(!(*result)->is<List>());
}

BOOST_AUTO_TEST_CASE(sexpr_as_method_const) {
    const auto result = parse_one("foo");
    BOOST_REQUIRE(result.has_value());
    const SExpr& expr = **result;
    const Symbol* sym = expr.as<Symbol>();
    BOOST_REQUIRE(sym != nullptr);
    BOOST_CHECK_EQUAL(sym->name, "foo");
}

BOOST_AUTO_TEST_CASE(sexpr_as_method_mutable) {
    const auto result = parse_one("foo");
    BOOST_REQUIRE(result.has_value());
    Symbol* sym = (*result)->as<Symbol>();
    BOOST_REQUIRE(sym != nullptr);
    sym->name = "bar"; // Should be mutable
    BOOST_CHECK_EQUAL((*result)->as<Symbol>()->name, "bar");
}

BOOST_AUTO_TEST_CASE(parse_error_kind_to_string) {
    BOOST_CHECK_EQUAL(std::string(to_string(ParseErrorKind::FromLexer)), "ParseErrorKind::FromLexer");
    BOOST_CHECK_EQUAL(std::string(to_string(ParseErrorKind::UnexpectedEOF)), "ParseErrorKind::UnexpectedEOF");
    BOOST_CHECK_EQUAL(std::string(to_string(ParseErrorKind::UnclosedList)), "ParseErrorKind::UnclosedList");
    BOOST_CHECK_EQUAL(std::string(to_string(ParseErrorKind::DotInVector)), "ParseErrorKind::DotInVector");
}

BOOST_AUTO_TEST_SUITE_END()
