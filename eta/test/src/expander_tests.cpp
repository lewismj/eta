#include <boost/test/unit_test.hpp>
#include "eta/reader/lexer.h"
#include "eta/reader/parser.h"
#include "eta/reader/expander.h"

using namespace eta::reader;
using namespace eta::reader::expander;
using namespace eta::reader::parser;

/**
 * Helpers
 */

namespace {

/// Parse a single datum from source
SExprPtr parse_one(const std::string& src) {
    lexer::Lexer lex(0, src);
    Parser p(lex);
    auto r = p.parse_datum();
    if (!r) throw std::runtime_error("parse_one failed");
    return std::move(*r);
}

/// Parse all top-level forms
std::vector<SExprPtr> parse_all(const std::string& src) {
    lexer::Lexer lex(0, src);
    Parser p(lex);
    auto r = p.parse_toplevel();
    if (!r) throw std::runtime_error("parse_all failed");
    return std::move(*r);
}

/// Expand a single form
ExpanderResult<SExprPtr> expand_one(const std::string& src, ExpanderConfig cfg = {}) {
    auto datum = parse_one(src);
    Expander ex(cfg);
    return ex.expand_form(datum);
}

/// Expand all forms
ExpanderResult<std::vector<SExprPtr>> expand_all(const std::string& src, ExpanderConfig cfg = {}) {
    auto forms = parse_all(src);
    Expander ex(cfg);
    return ex.expand_many(forms);
}

/// Check that the expanded output is a list whose head is a symbol with the given name
bool head_is(const SExprPtr& p, const std::string& name) {
    if (!p || !p->is<List>()) return false;
    const auto& lst = *p->as<List>();
    if (lst.elems.empty()) return false;
    if (!lst.elems[0] || !lst.elems[0]->is<Symbol>()) return false;
    return lst.elems[0]->as<Symbol>()->name == name;
}

/// Get the list size (number of elements)
std::size_t list_size(const SExprPtr& p) {
    if (!p || !p->is<List>()) return 0;
    return p->as<List>()->elems.size();
}

/// Get list element at index
const SExprPtr& elem(const SExprPtr& p, std::size_t idx) {
    return p->as<List>()->elems.at(idx);
}

/// Check if node is a symbol with given name
bool is_sym(const SExprPtr& p, const std::string& name) {
    return p && p->is<Symbol>() && p->as<Symbol>()->name == name;
}

/// Check if node is a boolean with given value
bool is_bool(const SExprPtr& p, bool val) {
    return p && p->is<Bool>() && p->as<Bool>()->value == val;
}

} ///< anonymous namespace

/**
 * Test suite
 */

BOOST_AUTO_TEST_SUITE(expander_tests)

/// ---------- Atoms pass through ----------

BOOST_AUTO_TEST_CASE(atom_number) {
    auto r = expand_one("42");
    BOOST_REQUIRE(r.has_value());
    BOOST_CHECK(r.value()->is<Number>());
}

BOOST_AUTO_TEST_CASE(atom_string) {
    auto r = expand_one("\"hello\"");
    BOOST_REQUIRE(r.has_value());
    BOOST_CHECK(r.value()->is<String>());
}

BOOST_AUTO_TEST_CASE(atom_symbol) {
    auto r = expand_one("foo");
    BOOST_REQUIRE(r.has_value());
    BOOST_CHECK(is_sym(*r, "foo"));
}

BOOST_AUTO_TEST_CASE(atom_boolean) {
    auto r = expand_one("#t");
    BOOST_REQUIRE(r.has_value());
    BOOST_CHECK(is_bool(*r, true));
}

BOOST_AUTO_TEST_CASE(atom_char) {
    auto r = expand_one("#\\a");
    BOOST_REQUIRE(r.has_value());
    BOOST_CHECK(r.value()->is<Char>());
}

/// ---------- quote ----------

BOOST_AUTO_TEST_CASE(quote_passes_through) {
    auto r = expand_one("(quote (a b c))");
    BOOST_REQUIRE(r.has_value());
    BOOST_CHECK(head_is(*r, "quote"));
    BOOST_CHECK_EQUAL(list_size(*r), 2u);
}

BOOST_AUTO_TEST_CASE(quote_arity_error) {
    auto r = expand_one("(quote a b)");
    BOOST_REQUIRE(!r.has_value());
    BOOST_CHECK(r.error().kind == ExpandError::Kind::ArityError);
}

BOOST_AUTO_TEST_CASE(unquote_outside_quasiquote) {
    auto r = expand_one("(unquote x)");
    BOOST_REQUIRE(!r.has_value());
    BOOST_CHECK(r.error().kind == ExpandError::Kind::InvalidSyntax);
}

BOOST_AUTO_TEST_CASE(unquote_splicing_outside_quasiquote) {
    auto r = expand_one("(unquote-splicing x)");
    BOOST_REQUIRE(!r.has_value());
    BOOST_CHECK(r.error().kind == ExpandError::Kind::InvalidSyntax);
}

/// ---------- if ----------

BOOST_AUTO_TEST_CASE(if_two_branch) {
    auto r = expand_one("(if #t 1 2)");
    BOOST_REQUIRE(r.has_value());
    BOOST_CHECK(head_is(*r, "if"));
    BOOST_CHECK_EQUAL(list_size(*r), 4u); ///< (if test cons alt)
}

BOOST_AUTO_TEST_CASE(if_one_branch_adds_begin) {
    auto r = expand_one("(if #t 1)");
    BOOST_REQUIRE(r.has_value());
    BOOST_CHECK(head_is(*r, "if"));
    BOOST_CHECK_EQUAL(list_size(*r), 4u);
    /// Alt should be (begin)
    BOOST_CHECK(head_is(elem(*r, 3), "begin"));
}

BOOST_AUTO_TEST_CASE(if_too_few_args) {
    auto r = expand_one("(if)");
    BOOST_REQUIRE(!r.has_value());
    BOOST_CHECK(r.error().kind == ExpandError::Kind::InvalidSyntax);
}

BOOST_AUTO_TEST_CASE(if_too_many_args) {
    auto r = expand_one("(if 1 2 3 4)");
    BOOST_REQUIRE(!r.has_value());
    BOOST_CHECK(r.error().kind == ExpandError::Kind::InvalidSyntax);
}

/// ---------- begin ----------

BOOST_AUTO_TEST_CASE(begin_empty) {
    auto r = expand_one("(begin)");
    BOOST_REQUIRE(r.has_value());
    BOOST_CHECK(head_is(*r, "begin"));
}

BOOST_AUTO_TEST_CASE(begin_multiple) {
    auto r = expand_one("(begin 1 2 3)");
    BOOST_REQUIRE(r.has_value());
    BOOST_CHECK(head_is(*r, "begin"));
    BOOST_CHECK_EQUAL(list_size(*r), 4u); ///< begin + 3 body
}

/// ---------- define ----------

BOOST_AUTO_TEST_CASE(define_simple) {
    auto r = expand_one("(define x 42)");
    BOOST_REQUIRE(r.has_value());
    BOOST_CHECK(head_is(*r, "define"));
    BOOST_CHECK(is_sym(elem(*r, 1), "x"));
}

BOOST_AUTO_TEST_CASE(define_function) {
    /// (define (f x) x) -> (define f (lambda (x) x))
    auto r = expand_one("(define (f x) x)");
    BOOST_REQUIRE(r.has_value());
    BOOST_CHECK(head_is(*r, "define"));
    BOOST_CHECK(is_sym(elem(*r, 1), "f"));
    /// RHS should be a lambda
    BOOST_CHECK(head_is(elem(*r, 2), "lambda"));
}

BOOST_AUTO_TEST_CASE(define_reserved_keyword) {
    auto r = expand_one("(define if 42)");
    BOOST_REQUIRE(!r.has_value());
    BOOST_CHECK(r.error().kind == ExpandError::Kind::ReservedKeyword);
}

BOOST_AUTO_TEST_CASE(define_too_few_args) {
    auto r = expand_one("(define x)");
    BOOST_REQUIRE(!r.has_value());
    BOOST_CHECK(r.error().kind == ExpandError::Kind::InvalidSyntax);
}

/// ---------- set! ----------

BOOST_AUTO_TEST_CASE(set_bang_valid) {
    auto r = expand_one("(set! x 42)");
    BOOST_REQUIRE(r.has_value());
    BOOST_CHECK(head_is(*r, "set!"));
    BOOST_CHECK(is_sym(elem(*r, 1), "x"));
}

BOOST_AUTO_TEST_CASE(set_bang_invalid_target) {
    auto r = expand_one("(set! 42 x)");
    BOOST_REQUIRE(!r.has_value());
    BOOST_CHECK(r.error().kind == ExpandError::Kind::InvalidSyntax);
}

BOOST_AUTO_TEST_CASE(set_bang_wrong_arity) {
    auto r = expand_one("(set! x)");
    BOOST_REQUIRE(!r.has_value());
    BOOST_CHECK(r.error().kind == ExpandError::Kind::InvalidSyntax);
}

/// ---------- lambda ----------

BOOST_AUTO_TEST_CASE(lambda_basic) {
    auto r = expand_one("(lambda (x) x)");
    BOOST_REQUIRE(r.has_value());
    BOOST_CHECK(head_is(*r, "lambda"));
    BOOST_CHECK_EQUAL(list_size(*r), 3u); ///< lambda formals body
}

BOOST_AUTO_TEST_CASE(lambda_multi_body) {
    auto r = expand_one("(lambda (x) 1 2 x)");
    BOOST_REQUIRE(r.has_value());
    BOOST_CHECK(head_is(*r, "lambda"));
    BOOST_CHECK_EQUAL(list_size(*r), 5u); ///< lambda formals b1 b2 b3
}

BOOST_AUTO_TEST_CASE(lambda_rest_param) {
    auto r = expand_one("(lambda args args)");
    BOOST_REQUIRE(r.has_value());
    BOOST_CHECK(head_is(*r, "lambda"));
}

BOOST_AUTO_TEST_CASE(lambda_dotted_formals) {
    auto r = expand_one("(lambda (x . rest) x)");
    BOOST_REQUIRE(r.has_value());
    BOOST_CHECK(head_is(*r, "lambda"));
}

BOOST_AUTO_TEST_CASE(lambda_no_body) {
    auto r = expand_one("(lambda (x))");
    BOOST_REQUIRE(!r.has_value());
    BOOST_CHECK(r.error().kind == ExpandError::Kind::InvalidSyntax);
}

BOOST_AUTO_TEST_CASE(lambda_reserved_param) {
    auto r = expand_one("(lambda (if) 1)");
    BOOST_REQUIRE(!r.has_value());
    BOOST_CHECK(r.error().kind == ExpandError::Kind::ReservedKeyword);
}

BOOST_AUTO_TEST_CASE(lambda_duplicate_params) {
    auto r = expand_one("(lambda (x x) 1)");
    BOOST_REQUIRE(!r.has_value());
    BOOST_CHECK(r.error().kind == ExpandError::Kind::DuplicateIdentifier);
}

/// ---------- let ----------

BOOST_AUTO_TEST_CASE(let_basic) {
    /// (let ((x 1)) x) -> ((lambda (x) x) 1)
    auto r = expand_one("(let ((x 1)) x)");
    BOOST_REQUIRE(r.has_value());
    /// Result is a function call: ((lambda ...) 1)
    BOOST_CHECK(r.value()->is<List>());
    /// The head should be a lambda
    BOOST_CHECK(head_is(elem(*r, 0), "lambda"));
}

BOOST_AUTO_TEST_CASE(let_multiple_bindings) {
    auto r = expand_one("(let ((x 1) (y 2)) (+ x y))");
    BOOST_REQUIRE(r.has_value());
    /// Should be a lambda call with 2 arguments
    auto& lst = *r.value()->as<List>();
    BOOST_CHECK_EQUAL(lst.elems.size(), 3u); ///< (lambda ...) + 2 args
}

BOOST_AUTO_TEST_CASE(let_duplicate_bindings) {
    auto r = expand_one("(let ((x 1) (x 2)) x)");
    BOOST_REQUIRE(!r.has_value());
    BOOST_CHECK(r.error().kind == ExpandError::Kind::InvalidLetBindings);
}

BOOST_AUTO_TEST_CASE(let_reserved_binding_name) {
    auto r = expand_one("(let ((define 1)) define)");
    BOOST_REQUIRE(!r.has_value());
    BOOST_CHECK(r.error().kind == ExpandError::Kind::InvalidLetBindings);
}

BOOST_AUTO_TEST_CASE(let_non_list_binding) {
    auto r = expand_one("(let (x) x)");
    BOOST_REQUIRE(!r.has_value());
    BOOST_CHECK(r.error().kind == ExpandError::Kind::InvalidLetBindings);
}

BOOST_AUTO_TEST_CASE(let_missing_body) {
    auto r = expand_one("(let ((x 1)))");
    BOOST_REQUIRE(!r.has_value());
    BOOST_CHECK(r.error().kind == ExpandError::Kind::InvalidSyntax);
}

BOOST_AUTO_TEST_CASE(let_empty_bindings) {
    auto r = expand_one("(let () 42)");
    BOOST_REQUIRE(r.has_value());
}

/// ---------- named let ----------

BOOST_AUTO_TEST_CASE(named_let_basic) {
    /**
     * (let loop ((i 0)) (loop (+ i 1)))
     * -> (letrec ((loop (lambda (i) (loop (+ i 1))))) (loop 0))
     */
    auto r = expand_one("(let loop ((i 0)) (loop (+ i 1)))");
    BOOST_REQUIRE(r.has_value());
    /// After expansion, the outermost should be a lambda call pattern
    BOOST_CHECK(r.value()->is<List>());
}

BOOST_AUTO_TEST_CASE(named_let_param_shadows_name) {
    auto r = expand_one("(let loop ((loop 0)) loop)");
    BOOST_REQUIRE(!r.has_value());
    BOOST_CHECK(r.error().kind == ExpandError::Kind::InvalidLetBindings);
}

/// ---------- let* ----------

BOOST_AUTO_TEST_CASE(let_star_basic) {
    auto r = expand_one("(let* ((x 1) (y x)) y)");
    BOOST_REQUIRE(r.has_value());
    /// let* nests: outer let for x, inner let for y
}

BOOST_AUTO_TEST_CASE(let_star_empty_bindings) {
    auto r = expand_one("(let* () 42)");
    BOOST_REQUIRE(r.has_value());
}

BOOST_AUTO_TEST_CASE(let_star_missing_body) {
    auto r = expand_one("(let* ((x 1)))");
    BOOST_REQUIRE(!r.has_value());
}

/// ---------- letrec ----------

BOOST_AUTO_TEST_CASE(letrec_basic) {
    auto r = expand_one("(letrec ((x 1)) x)");
    BOOST_REQUIRE(r.has_value());
    /// letrec -> let with placeholders + set! + body
}

BOOST_AUTO_TEST_CASE(letrec_mutual_recursion) {
    auto r = expand_one("(letrec ((even? (lambda (n) (if (= n 0) #t (odd? (- n 1))))) "
                         "         (odd?  (lambda (n) (if (= n 0) #f (even? (- n 1)))))) "
                         " (even? 10))");
    BOOST_REQUIRE(r.has_value());
}

BOOST_AUTO_TEST_CASE(letrec_duplicate_bindings) {
    auto r = expand_one("(letrec ((x 1) (x 2)) x)");
    BOOST_REQUIRE(!r.has_value());
    BOOST_CHECK(r.error().kind == ExpandError::Kind::InvalidLetBindings);
}

BOOST_AUTO_TEST_CASE(letrec_missing_body) {
    auto r = expand_one("(letrec ((x 1)))");
    BOOST_REQUIRE(!r.has_value());
}

/// ---------- letrec* ----------

BOOST_AUTO_TEST_CASE(letrec_star_basic) {
    auto r = expand_one("(letrec* ((x 1) (y x)) y)");
    BOOST_REQUIRE(r.has_value());
}

BOOST_AUTO_TEST_CASE(letrec_star_empty_bindings) {
    auto r = expand_one("(letrec* () 42)");
    BOOST_REQUIRE(r.has_value());
}

/// ---------- cond ----------

BOOST_AUTO_TEST_CASE(cond_basic) {
    auto r = expand_one("(cond (#t 1))");
    BOOST_REQUIRE(r.has_value());
    /// cond -> nested if
    BOOST_CHECK(head_is(*r, "if"));
}

BOOST_AUTO_TEST_CASE(cond_else) {
    auto r = expand_one("(cond (#f 1) (else 2))");
    BOOST_REQUIRE(r.has_value());
    BOOST_CHECK(head_is(*r, "if"));
}

BOOST_AUTO_TEST_CASE(cond_multiple_clauses) {
    auto r = expand_one("(cond ((= x 1) 10) ((= x 2) 20) (else 30))");
    BOOST_REQUIRE(r.has_value());
    BOOST_CHECK(head_is(*r, "if"));
}

BOOST_AUTO_TEST_CASE(cond_arrow) {
    /// (cond (x => proc)) -> (let ((t x)) (if t (proc t) ...))
    auto r = expand_one("(cond (x => display))");
    BOOST_REQUIRE(r.has_value());
}

BOOST_AUTO_TEST_CASE(cond_test_only_clause) {
    /// (cond (x)) -> (if x x ...)
    auto r = expand_one("(cond (x))");
    BOOST_REQUIRE(r.has_value());
    BOOST_CHECK(head_is(*r, "if"));
}

BOOST_AUTO_TEST_CASE(cond_no_clauses) {
    auto r = expand_one("(cond)");
    BOOST_REQUIRE(!r.has_value());
    BOOST_CHECK(r.error().kind == ExpandError::Kind::InvalidSyntax);
}

BOOST_AUTO_TEST_CASE(cond_else_not_last) {
    auto r = expand_one("(cond (else 1) (#t 2))");
    BOOST_REQUIRE(!r.has_value());
    BOOST_CHECK(r.error().kind == ExpandError::Kind::InvalidSyntax);
}

BOOST_AUTO_TEST_CASE(cond_empty_clause) {
    auto r = expand_one("(cond ())");
    BOOST_REQUIRE(!r.has_value());
    BOOST_CHECK(r.error().kind == ExpandError::Kind::InvalidSyntax);
}

/// ---------- case ----------

BOOST_AUTO_TEST_CASE(case_basic) {
    auto r = expand_one("(case x ((1) 10) ((2) 20))");
    BOOST_REQUIRE(r.has_value());
    /// case -> let + nested if with eqv?
}

BOOST_AUTO_TEST_CASE(case_with_else) {
    auto r = expand_one("(case x ((1 2) 10) (else 20))");
    BOOST_REQUIRE(r.has_value());
}

BOOST_AUTO_TEST_CASE(case_no_key) {
    auto r = expand_one("(case)");
    BOOST_REQUIRE(!r.has_value());
    BOOST_CHECK(r.error().kind == ExpandError::Kind::InvalidSyntax);
}

BOOST_AUTO_TEST_CASE(case_else_not_last) {
    auto r = expand_one("(case x (else 1) ((1) 2))");
    BOOST_REQUIRE(!r.has_value());
    BOOST_CHECK(r.error().kind == ExpandError::Kind::InvalidSyntax);
}

/// ---------- and ----------

BOOST_AUTO_TEST_CASE(and_empty) {
    auto r = expand_one("(and)");
    BOOST_REQUIRE(r.has_value());
    /// (and) -> #t
    BOOST_CHECK(is_bool(*r, true));
}

BOOST_AUTO_TEST_CASE(and_single) {
    auto r = expand_one("(and 42)");
    BOOST_REQUIRE(r.has_value());
    /// (and e) -> e
    BOOST_CHECK(r.value()->is<Number>());
}

BOOST_AUTO_TEST_CASE(and_multiple) {
    auto r = expand_one("(and 1 2 3)");
    BOOST_REQUIRE(r.has_value());
    /// (and 1 2 3) -> (if 1 (if 2 3 #f) #f)
    BOOST_CHECK(head_is(*r, "if"));
}

/// ---------- or ----------

BOOST_AUTO_TEST_CASE(or_empty) {
    auto r = expand_one("(or)");
    BOOST_REQUIRE(r.has_value());
    /// (or) -> #f
    BOOST_CHECK(is_bool(*r, false));
}

BOOST_AUTO_TEST_CASE(or_single) {
    auto r = expand_one("(or 42)");
    BOOST_REQUIRE(r.has_value());
    /// (or e) -> e
    BOOST_CHECK(r.value()->is<Number>());
}

BOOST_AUTO_TEST_CASE(or_multiple) {
    auto r = expand_one("(or 1 2)");
    BOOST_REQUIRE(r.has_value());
    /**
     * (or 1 2) -> (let ((t 1)) (if t t 2))
     * After expansion, outermost is a lambda call
     */
    BOOST_CHECK(r.value()->is<List>());
}

/// ---------- when ----------

BOOST_AUTO_TEST_CASE(when_basic) {
    auto r = expand_one("(when #t 1 2)");
    BOOST_REQUIRE(r.has_value());
    /// -> (if #t (begin 1 2) (begin))
    BOOST_CHECK(head_is(*r, "if"));
    BOOST_CHECK(head_is(elem(*r, 2), "begin")); ///< consequent
    BOOST_CHECK(head_is(elem(*r, 3), "begin")); ///< alternate
}

BOOST_AUTO_TEST_CASE(when_no_test) {
    auto r = expand_one("(when)");
    BOOST_REQUIRE(!r.has_value());
    BOOST_CHECK(r.error().kind == ExpandError::Kind::InvalidSyntax);
}

/// ---------- unless ----------

BOOST_AUTO_TEST_CASE(unless_basic) {
    auto r = expand_one("(unless #f 1 2)");
    BOOST_REQUIRE(r.has_value());
    /// -> (if #f (begin) (begin 1 2))
    BOOST_CHECK(head_is(*r, "if"));
    BOOST_CHECK(head_is(elem(*r, 2), "begin")); ///< empty consequent
    BOOST_CHECK(head_is(elem(*r, 3), "begin")); ///< alternate with body
}

BOOST_AUTO_TEST_CASE(unless_no_test) {
    auto r = expand_one("(unless)");
    BOOST_REQUIRE(!r.has_value());
    BOOST_CHECK(r.error().kind == ExpandError::Kind::InvalidSyntax);
}

/// ---------- do ----------

BOOST_AUTO_TEST_CASE(do_basic) {
    /// (do ((i 0 (+ i 1))) ((= i 5) i))
    auto r = expand_one("(do ((i 0 (+ i 1))) ((= i 5) i))");
    BOOST_REQUIRE(r.has_value());
    /// Result is a letrec expansion (lambda call pattern)
}

BOOST_AUTO_TEST_CASE(do_with_body) {
    auto r = expand_one("(do ((i 0 (+ i 1))) ((= i 3)) (display i))");
    BOOST_REQUIRE(r.has_value());
}

BOOST_AUTO_TEST_CASE(do_implicit_step) {
    /// When step is omitted, variable is its own step
    auto r = expand_one("(do ((i 0)) ((= i 0) i))");
    BOOST_REQUIRE(r.has_value());
}

BOOST_AUTO_TEST_CASE(do_no_var_clauses) {
    auto r = expand_one("(do () (#t 42))");
    BOOST_REQUIRE(r.has_value());
}

BOOST_AUTO_TEST_CASE(do_missing_test) {
    auto r = expand_one("(do ((i 0)))");
    BOOST_REQUIRE(!r.has_value());
    BOOST_CHECK(r.error().kind == ExpandError::Kind::InvalidSyntax);
}

BOOST_AUTO_TEST_CASE(do_duplicate_vars) {
    auto r = expand_one("(do ((i 0 (+ i 1)) (i 1 (+ i 1))) (#t 0))");
    BOOST_REQUIRE(!r.has_value());
    BOOST_CHECK(r.error().kind == ExpandError::Kind::DuplicateIdentifier);
}

BOOST_AUTO_TEST_CASE(do_reserved_var) {
    auto r = expand_one("(do ((define 0 1)) (#t 0))");
    BOOST_REQUIRE(!r.has_value());
    BOOST_CHECK(r.error().kind == ExpandError::Kind::ReservedKeyword);
}

BOOST_AUTO_TEST_CASE(do_empty_test_clause) {
    auto r = expand_one("(do ((i 0 (+ i 1))) ())");
    BOOST_REQUIRE(!r.has_value());
    BOOST_CHECK(r.error().kind == ExpandError::Kind::InvalidSyntax);
}

/// ---------- quasiquote ----------

BOOST_AUTO_TEST_CASE(quasiquote_no_unquote) {
    auto r = expand_one("`(a b c)");
    BOOST_REQUIRE(r.has_value());
    /// All atoms quoted: becomes cons chain
}

BOOST_AUTO_TEST_CASE(quasiquote_with_unquote) {
    auto r = expand_one("`(a ,x c)");
    BOOST_REQUIRE(r.has_value());
    /// Should contain a reference to x somewhere
}

BOOST_AUTO_TEST_CASE(quasiquote_splicing) {
    auto r = expand_one("`(a ,@xs b)");
    BOOST_REQUIRE(r.has_value());
    /// Should contain an append call
}

BOOST_AUTO_TEST_CASE(quasiquote_nested) {
    auto r = expand_one("``(a ,,x)");
    BOOST_REQUIRE(r.has_value());
}

BOOST_AUTO_TEST_CASE(quasiquote_atom) {
    auto r = expand_one("`42");
    BOOST_REQUIRE(r.has_value());
    /// `42 -> (quote 42)
    BOOST_CHECK(head_is(*r, "quote"));
}

/// ---------- def (convenience sugar) ----------

BOOST_AUTO_TEST_CASE(def_simple) {
    auto r = expand_one("(def x 42)");
    BOOST_REQUIRE(r.has_value());
    BOOST_CHECK(head_is(*r, "define"));
    BOOST_CHECK(is_sym(elem(*r, 1), "x"));
}

BOOST_AUTO_TEST_CASE(def_function) {
    auto r = expand_one("(def (f x) x)");
    BOOST_REQUIRE(r.has_value());
    BOOST_CHECK(head_is(*r, "define"));
    BOOST_CHECK(head_is(elem(*r, 2), "lambda"));
}

BOOST_AUTO_TEST_CASE(def_reserved) {
    auto r = expand_one("(def lambda 1)");
    BOOST_REQUIRE(!r.has_value());
    BOOST_CHECK(r.error().kind == ExpandError::Kind::ReservedKeyword);
}

/// ---------- defun ----------

BOOST_AUTO_TEST_CASE(defun_basic) {
    auto r = expand_one("(defun f (x y) (+ x y))");
    BOOST_REQUIRE(r.has_value());
    BOOST_CHECK(head_is(*r, "define"));
    BOOST_CHECK(is_sym(elem(*r, 1), "f"));
    BOOST_CHECK(head_is(elem(*r, 2), "lambda"));
}

BOOST_AUTO_TEST_CASE(defun_rest_args) {
    auto r = expand_one("(defun f args args)");
    BOOST_REQUIRE(r.has_value());
    BOOST_CHECK(head_is(*r, "define"));
}

BOOST_AUTO_TEST_CASE(defun_reserved_name) {
    auto r = expand_one("(defun quote (x) x)");
    BOOST_REQUIRE(!r.has_value());
    BOOST_CHECK(r.error().kind == ExpandError::Kind::ReservedKeyword);
}

BOOST_AUTO_TEST_CASE(defun_too_few_args) {
    auto r = expand_one("(defun f)");
    BOOST_REQUIRE(!r.has_value());
    BOOST_CHECK(r.error().kind == ExpandError::Kind::InvalidSyntax);
}

/// ---------- progn and step (aliases for begin) ----------

BOOST_AUTO_TEST_CASE(progn_is_begin) {
    auto r = expand_one("(progn 1 2 3)");
    BOOST_REQUIRE(r.has_value());
    /// handle_begin preserves the original keyword
    BOOST_CHECK(head_is(*r, "progn"));
    BOOST_CHECK_EQUAL(list_size(*r), 4u); ///< progn + 3 body
}


/// ---------- module directives ----------

BOOST_AUTO_TEST_CASE(module_basic) {
    auto r = expand_one("(module m (define x 1))");
    BOOST_REQUIRE(r.has_value());
    BOOST_CHECK(head_is(*r, "module"));
    BOOST_CHECK(is_sym(elem(*r, 1), "m"));
}

BOOST_AUTO_TEST_CASE(export_basic) {
    auto r = expand_one("(export x y z)");
    BOOST_REQUIRE(r.has_value());
    BOOST_CHECK(head_is(*r, "export"));
}

BOOST_AUTO_TEST_CASE(export_non_symbol) {
    auto r = expand_one("(export 42)");
    BOOST_REQUIRE(!r.has_value());
    BOOST_CHECK(r.error().kind == ExpandError::Kind::InvalidSyntax);
}

BOOST_AUTO_TEST_CASE(import_basic) {
    auto r = expand_one("(import foo)");
    BOOST_REQUIRE(r.has_value());
    BOOST_CHECK(head_is(*r, "import"));
}

/// ---------- Module form (parsed as ModuleForm) ----------

BOOST_AUTO_TEST_CASE(module_form_with_exports) {
    auto forms = parse_all("(module m (export x) (define x 42))");
    BOOST_REQUIRE(!forms.empty());
    BOOST_CHECK(forms[0]->is<ModuleForm>());
    Expander ex;
    auto r = ex.expand_form(forms[0]);
    BOOST_REQUIRE(r.has_value());
    BOOST_CHECK(head_is(*r, "module"));
}

/// ---------- Application ----------

BOOST_AUTO_TEST_CASE(application_basic) {
    auto r = expand_one("(f x y)");
    BOOST_REQUIRE(r.has_value());
    BOOST_CHECK(r.value()->is<List>());
    BOOST_CHECK(is_sym(elem(*r, 0), "f"));
}

BOOST_AUTO_TEST_CASE(application_empty_list) {
    auto r = expand_one("()");
    BOOST_REQUIRE(r.has_value());
    BOOST_CHECK(r.value()->is<List>());
    BOOST_CHECK_EQUAL(list_size(*r), 0u);
}

/// ---------- Depth limit ----------

BOOST_AUTO_TEST_CASE(expansion_depth_exceeded) {
    /// Use a very low depth limit and a deeply nesting form
    ExpanderConfig cfg;
    cfg.depth_limit = 5;
    /// Deeply nested begin forms
    auto r = expand_one("(begin (begin (begin (begin (begin (begin 1))))))", cfg);
    BOOST_REQUIRE(!r.has_value());
    BOOST_CHECK(r.error().kind == ExpandError::Kind::ExpansionDepthExceeded);
}

/// ---------- Internal defines -> letrec ----------

BOOST_AUTO_TEST_CASE(internal_defines_to_letrec) {
    /**
     * (lambda () (define a 1) (define b 2) (+ a b))
     * -> (lambda () (letrec ((a 1) (b 2)) (+ a b)))
     */
    auto r = expand_one("(lambda () (define a 1) (define b 2) (+ a b))");
    BOOST_REQUIRE(r.has_value());
    BOOST_CHECK(head_is(*r, "lambda"));
    /// Body should be a single letrec expansion (which itself gets expanded)
}

BOOST_AUTO_TEST_CASE(internal_defines_disabled) {
    ExpanderConfig cfg;
    cfg.enable_internal_defines_to_letrec = false;
    /**
     * Without the rewrite, internal defines remain as-is
     * But they get passed to handle_define individually
     */
    auto r = expand_one("(lambda () (define a 1) a)", cfg);
    BOOST_REQUIRE(r.has_value());
    BOOST_CHECK(head_is(*r, "lambda"));
}

/// ---------- expand_many ----------

BOOST_AUTO_TEST_CASE(expand_many_basic) {
    auto r = expand_all("42 (if #t 1 2) (begin 3)");
    BOOST_REQUIRE(r.has_value());
    BOOST_CHECK_EQUAL(r.value().size(), 3u);
    BOOST_CHECK(r.value()[0]->is<Number>());
    BOOST_CHECK(head_is(r.value()[1], "if"));
    BOOST_CHECK(head_is(r.value()[2], "begin"));
}

BOOST_AUTO_TEST_CASE(expand_many_propagates_error) {
    auto r = expand_all("42 (if) 3");
    BOOST_REQUIRE(!r.has_value());
    BOOST_CHECK(r.error().kind == ExpandError::Kind::InvalidSyntax);
}

/// ---------- dotted application rejected ----------

BOOST_AUTO_TEST_CASE(dotted_application_error) {
    /// We parse a dotted list outside of a special form
    auto datum = parse_one("(f x . y)");
    BOOST_REQUIRE(datum->is<List>());
    BOOST_CHECK(datum->as<List>()->dotted);
    Expander ex;
    auto r = ex.expand_form(datum);
    BOOST_REQUIRE(!r.has_value());
    BOOST_CHECK(r.error().kind == ExpandError::Kind::InvalidSyntax);
}

/// ---------- Validator: reserved keyword as identifier ----------

BOOST_AUTO_TEST_CASE(let_with_reserved_keyword_param) {
    auto r = expand_one("(let ((lambda 1)) lambda)");
    BOOST_REQUIRE(!r.has_value());
    BOOST_CHECK(r.error().kind == ExpandError::Kind::InvalidLetBindings);
}

/// ---------- Combined: let desugars into lambda application ----------

BOOST_AUTO_TEST_CASE(let_desugars_to_lambda_call) {
    auto r = expand_one("(let ((a 1) (b 2)) (+ a b))");
    BOOST_REQUIRE(r.has_value());
    /// Top level should be a function application
    auto& lst = *r.value()->as<List>();
    /// First element should be a lambda
    BOOST_CHECK(head_is(lst.elems[0], "lambda"));
    /// Args follow
    BOOST_CHECK_EQUAL(lst.elems.size(), 3u); ///< lambda + 2 init args
}

/// ---------- letrec desugars to let + set! ----------

BOOST_AUTO_TEST_CASE(letrec_desugars_to_let_set_body) {
    auto r = expand_one("(letrec ((x 1)) x)");
    BOOST_REQUIRE(r.has_value());
    /// Top-level is a lambda call (from the let)
    auto& lst = *r.value()->as<List>();
    BOOST_CHECK(head_is(lst.elems[0], "lambda"));
    /// Lambda body should contain begin with set! and then x
    const auto& lam = *lst.elems[0]->as<List>();
    /// Body is the 3rd element (index 2)
    BOOST_CHECK(head_is(lam.elems[2], "begin"));
    /// Inside begin: first should be set!
    const auto& body = *lam.elems[2]->as<List>();
    BOOST_CHECK(head_is(body.elems[1], "set!"));
}

/**
 * define-record-type
 */

BOOST_AUTO_TEST_CASE(record_type_basic_expand) {
    auto r = expand_one(
        "(define-record-type point"
        "  (make-point x y)"
        "  point?"
        "  (x point-x)"
        "  (y point-y))");
    BOOST_REQUIRE(r.has_value());
    BOOST_CHECK(head_is(*r, "begin"));
    /// begin + 4 defines = 5 elements
    auto& lst = *r.value()->as<List>();
    /// Each child after "begin" should be a define
    std::size_t define_count = 0;
    for (std::size_t i = 1; i < lst.elems.size(); ++i) {
        if (head_is(lst.elems[i], "define")) ++define_count;
    }
    BOOST_CHECK_EQUAL(define_count, 4u); ///< ctor + pred + 2 accessors
}

BOOST_AUTO_TEST_CASE(record_type_with_mutator) {
    auto r = expand_one(
        "(define-record-type pair"
        "  (make-pair a b)"
        "  pair?"
        "  (a pair-a)"
        "  (b pair-b set-pair-b!))");
    BOOST_REQUIRE(r.has_value());
    BOOST_CHECK(head_is(*r, "begin"));
    auto& lst = *r.value()->as<List>();
    std::size_t define_count = 0;
    for (std::size_t i = 1; i < lst.elems.size(); ++i) {
        if (head_is(lst.elems[i], "define")) ++define_count;
    }
    BOOST_CHECK_EQUAL(define_count, 5u); ///< ctor + pred + 2 accessors + 1 mutator
}

BOOST_AUTO_TEST_CASE(record_type_readonly_no_mutator) {
    /// All read-only: no mutator defines should appear
    auto r = expand_one(
        "(define-record-type color"
        "  (make-color r g b)"
        "  color?"
        "  (r color-r)"
        "  (g color-g)"
        "  (b color-b))");
    BOOST_REQUIRE(r.has_value());
    auto& lst = *r.value()->as<List>();
    std::size_t define_count = 0;
    for (std::size_t i = 1; i < lst.elems.size(); ++i) {
        if (head_is(lst.elems[i], "define")) ++define_count;
    }
    BOOST_CHECK_EQUAL(define_count, 5u); ///< ctor + pred + 3 accessors, 0 mutators
}

BOOST_AUTO_TEST_CASE(record_type_error_too_few_subforms) {
    /// Missing predicate
    auto r = expand_one(
        "(define-record-type point"
        "  (make-point x y))");
    BOOST_REQUIRE(!r.has_value());
    BOOST_CHECK(r.error().kind == ExpandError::Kind::ArityError);
}

BOOST_AUTO_TEST_CASE(record_type_error_duplicate_ctor_fields) {
    auto r = expand_one(
        "(define-record-type point"
        "  (make-point x x)"
        "  point?"
        "  (x point-x))");
    BOOST_REQUIRE(!r.has_value());
    BOOST_CHECK(r.error().kind == ExpandError::Kind::DuplicateIdentifier);
}

BOOST_AUTO_TEST_CASE(record_type_error_unknown_field_in_spec) {
    auto r = expand_one(
        "(define-record-type point"
        "  (make-point x y)"
        "  point?"
        "  (z point-z))");  ///< 'z' not in constructor
    BOOST_REQUIRE(!r.has_value());
    BOOST_CHECK(r.error().kind == ExpandError::Kind::InvalidSyntax);
}

BOOST_AUTO_TEST_CASE(record_type_error_reserved_type_name) {
    auto r = expand_one(
        "(define-record-type lambda"
        "  (make-lambda x)"
        "  lambda?"
        "  (x lambda-x))");
    BOOST_REQUIRE(!r.has_value());
    BOOST_CHECK(r.error().kind == ExpandError::Kind::ReservedKeyword);
}

BOOST_AUTO_TEST_CASE(record_type_error_reserved_constructor) {
    auto r = expand_one(
        "(define-record-type point"
        "  (define x)"
        "  point?"
        "  (x point-x))");
    BOOST_REQUIRE(!r.has_value());
    BOOST_CHECK(r.error().kind == ExpandError::Kind::ReservedKeyword);
}

BOOST_AUTO_TEST_CASE(record_type_error_reserved_field_name) {
    auto r = expand_one(
        "(define-record-type point"
        "  (make-point if)"
        "  point?"
        "  (if point-if))");
    BOOST_REQUIRE(!r.has_value());
    BOOST_CHECK(r.error().kind == ExpandError::Kind::ReservedKeyword);
}

BOOST_AUTO_TEST_CASE(record_type_error_non_symbol_type_name) {
    auto r = expand_one(
        "(define-record-type 42"
        "  (make-point x)"
        "  point?"
        "  (x point-x))");
    BOOST_REQUIRE(!r.has_value());
    BOOST_CHECK(r.error().kind == ExpandError::Kind::InvalidSyntax);
}

BOOST_AUTO_TEST_CASE(record_type_error_non_symbol_predicate) {
    auto r = expand_one(
        "(define-record-type point"
        "  (make-point x)"
        "  42"
        "  (x point-x))");
    BOOST_REQUIRE(!r.has_value());
    BOOST_CHECK(r.error().kind == ExpandError::Kind::InvalidSyntax);
}

BOOST_AUTO_TEST_CASE(record_type_no_fields) {
    auto r = expand_one(
        "(define-record-type unit"
        "  (make-unit)"
        "  unit?)");
    BOOST_REQUIRE(r.has_value());
    BOOST_CHECK(head_is(*r, "begin"));
    auto& lst = *r.value()->as<List>();
    std::size_t define_count = 0;
    for (std::size_t i = 1; i < lst.elems.size(); ++i) {
        if (head_is(lst.elems[i], "define")) ++define_count;
    }
    BOOST_CHECK_EQUAL(define_count, 2u); ///< ctor + pred
}

BOOST_AUTO_TEST_CASE(record_type_error_duplicate_field_spec) {
    auto r = expand_one(
        "(define-record-type point"
        "  (make-point x)"
        "  point?"
        "  (x point-x)"
        "  (x point-x2))");
    BOOST_REQUIRE(!r.has_value());
    BOOST_CHECK(r.error().kind == ExpandError::Kind::DuplicateIdentifier);
}

/**
 * define-syntax / syntax-rules
 */

BOOST_AUTO_TEST_CASE(syntax_rules_basic_expansion) {
    /// Define a simple macro that rewrites (my-if t c a) -> (if t c a)
    auto r = expand_all(
        "(define-syntax my-if (syntax-rules () ((_ t c a) (if t c a))))"
        "(my-if #t 1 2)");
    BOOST_REQUIRE(r.has_value());
    BOOST_REQUIRE_EQUAL(r->size(), 2u);
    /// First form is the empty (begin) from define-syntax
    BOOST_CHECK(head_is((*r)[0], "begin"));
    /// Second form should be expanded to (if #t 1 2)
    BOOST_CHECK(head_is((*r)[1], "if"));
    BOOST_CHECK_EQUAL(list_size((*r)[1]), 4u);
}

BOOST_AUTO_TEST_CASE(syntax_rules_multiple_clauses) {
    /// Macro with two clauses: unary and binary
    auto r = expand_all(
        "(define-syntax my-add (syntax-rules ()"
        "  ((_ x) x)"
        "  ((_ x y) (+ x y))))"
        "(my-add 5)"
        "(my-add 3 4)");
    BOOST_REQUIRE(r.has_value());
    BOOST_REQUIRE_EQUAL(r->size(), 3u);
    /// (my-add 5) -> 5
    BOOST_CHECK((*r)[1]->is<parser::Number>());
    /// (my-add 3 4) -> (+ 3 4)
    BOOST_CHECK(head_is((*r)[2], "+"));
}

BOOST_AUTO_TEST_CASE(syntax_rules_ellipsis) {
    /// Macro with ellipsis: (my-list x ...) -> (list x ...)
    auto r = expand_all(
        "(define-syntax my-list (syntax-rules () ((_ x ...) (list x ...))))"
        "(my-list 1 2 3)");
    BOOST_REQUIRE(r.has_value());
    BOOST_REQUIRE_EQUAL(r->size(), 2u);
    BOOST_CHECK(head_is((*r)[1], "list"));
    BOOST_CHECK_EQUAL(list_size((*r)[1]), 4u); ///< list + 3 args
}

BOOST_AUTO_TEST_CASE(syntax_rules_ellipsis_empty) {
    /// Ellipsis matching zero elements
    auto r = expand_all(
        "(define-syntax my-list (syntax-rules () ((_ x ...) (list x ...))))"
        "(my-list)");
    BOOST_REQUIRE(r.has_value());
    BOOST_REQUIRE_EQUAL(r->size(), 2u);
    BOOST_CHECK(head_is((*r)[1], "list"));
    BOOST_CHECK_EQUAL(list_size((*r)[1]), 1u); ///< just "list", no args
}

BOOST_AUTO_TEST_CASE(syntax_rules_literal_keyword) {
    /// Macro with literal keyword matching
    auto r = expand_all(
        "(define-syntax my-cond (syntax-rules (else)"
        "  ((_ (else e)) e)"
        "  ((_ (t e)) (if t e))))"
        "(my-cond (else 42))"
        "(my-cond (#t 99))");
    BOOST_REQUIRE(r.has_value());
    BOOST_REQUIRE_EQUAL(r->size(), 3u);
    /// (my-cond (else 42)) -> 42
    BOOST_CHECK((*r)[1]->is<parser::Number>());
    /// (my-cond (#t 99)) -> (if #t 99)
    BOOST_CHECK(head_is((*r)[2], "if"));
}

BOOST_AUTO_TEST_CASE(syntax_rules_nested_output) {
    /// Macro output contains derived forms that get re-expanded
    auto r = expand_all(
        "(define-syntax my-let1 (syntax-rules ()"
        "  ((_ var val body) (let ((var val)) body))))"
        "(my-let1 x 10 x)");
    BOOST_REQUIRE(r.has_value());
    BOOST_REQUIRE_EQUAL(r->size(), 2u);
    const auto& expanded = (*r)[1];
    BOOST_CHECK(expanded->is<List>());
}

BOOST_AUTO_TEST_CASE(syntax_rules_no_matching_clause) {
    auto r = expand_all(
        "(define-syntax my-mac (syntax-rules () ((_ x y) (+ x y))))"
        "(my-mac 1)");
    BOOST_REQUIRE(!r.has_value());
    BOOST_CHECK(r.error().kind == ExpandError::Kind::InvalidSyntax);
}

BOOST_AUTO_TEST_CASE(syntax_rules_malformed_define_syntax) {
    /// Missing transformer
    auto r = expand_one("(define-syntax foo)");
    BOOST_REQUIRE(!r.has_value());
    BOOST_CHECK(r.error().kind == ExpandError::Kind::InvalidSyntax);
}

BOOST_AUTO_TEST_CASE(syntax_rules_malformed_not_syntax_rules) {
    /// Transformer is not syntax-rules
    auto r = expand_one("(define-syntax foo (lambda (x) x))");
    BOOST_REQUIRE(!r.has_value());
    BOOST_CHECK(r.error().kind == ExpandError::Kind::InvalidSyntax);
}

BOOST_AUTO_TEST_CASE(syntax_rules_wildcard_pattern) {
    /// _ matches anything but doesn't bind
    auto r = expand_all(
        "(define-syntax ignore-first (syntax-rules () ((_ _ x) x)))"
        "(ignore-first 999 42)");
    BOOST_REQUIRE(r.has_value());
    BOOST_REQUIRE_EQUAL(r->size(), 2u);
    BOOST_CHECK((*r)[1]->is<parser::Number>());
}

BOOST_AUTO_TEST_CASE(syntax_rules_swap_macro) {
    /// Classic swap! macro using begin+let+set!
    auto r = expand_all(
        "(define-syntax swap! (syntax-rules ()"
        "  ((_ a b) (let ((tmp a)) (set! a b) (set! b tmp)))))"
        "(swap! x y)");
    BOOST_REQUIRE(r.has_value());
    BOOST_REQUIRE_EQUAL(r->size(), 2u);
    /// The output should be expanded (let desugars to lambda application)
    BOOST_CHECK((*r)[1]->is<List>());
}

BOOST_AUTO_TEST_CASE(syntax_rules_chained_macros) {
    /// One macro's output invokes another macro
    auto r = expand_all(
        "(define-syntax double (syntax-rules () ((_ x) (+ x x))))"
        "(define-syntax quadruple (syntax-rules () ((_ x) (double (double x)))))"
        "(quadruple 3)");
    BOOST_REQUIRE(r.has_value());
    BOOST_REQUIRE_EQUAL(r->size(), 3u);
    /// Should fully expand: (+ (+ 3 3) (+ 3 3))
    BOOST_CHECK(head_is((*r)[2], "+"));
}

BOOST_AUTO_TEST_SUITE_END()



