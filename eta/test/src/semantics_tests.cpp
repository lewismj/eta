#include <boost/test/unit_test.hpp>

#include "eta/reader/lexer.h"
#include "eta/reader/parser.h"
#include "eta/reader/expander.h"
#include "eta/reader/module_linker.h"
#include "eta/semantics/semantic_analyzer.h"
#include "eta/semantics/core_ir.h"

using namespace eta;
using namespace eta::semantics;

static SemResult<std::vector<ModuleSemantics>> analyze_src(std::string_view program) {
    reader::lexer::Lexer lex(0, program);
    reader::parser::Parser p(lex);
    auto parsed = p.parse_toplevel();
    if (!parsed) return std::unexpected(SemanticError{SemanticError::Kind::InvalidFormShape, {}, "Parse error"});

    reader::expander::Expander ex;
    auto expanded = ex.expand_many(*parsed);
    if (!expanded) return std::unexpected(SemanticError{SemanticError::Kind::InvalidFormShape, {}, "Expansion error"});

    reader::ModuleLinker L;
    auto idx = L.index_modules(*expanded);
    if (!idx) return std::unexpected(SemanticError{SemanticError::Kind::InvalidFormShape, {}, "Linker index error"});
    
    auto lk = L.link();
    if (!lk) return std::unexpected(SemanticError{SemanticError::Kind::InvalidFormShape, {}, "Link error"});

    SemanticAnalyzer sa;
    return sa.analyze_all(*expanded, L);
}

BOOST_AUTO_TEST_SUITE(semantics_tests)

BOOST_AUTO_TEST_CASE(test_basic_global_define) {
    std::string src = "(module m1 (export x) (define x 1))";
    auto res = analyze_src(src);
    BOOST_REQUIRE(res.has_value());
    BOOST_REQUIRE_EQUAL(res->size(), 1);
    
    const auto& mod = (*res)[0];
    BOOST_CHECK_EQUAL(mod.name, "m1");
    BOOST_REQUIRE_EQUAL(mod.bindings.size(), 1);
    BOOST_CHECK_EQUAL(mod.bindings[0].name, "x");
    BOOST_CHECK(mod.bindings[0].kind == BindingInfo::Kind::Global);
    
    // Check export
    BOOST_REQUIRE(mod.exports.contains("x"));
    BOOST_CHECK_EQUAL(mod.exports.at("x").id, 0);
    
    // Check toplevel inits: (set! x 1)
    BOOST_REQUIRE_EQUAL(mod.toplevel_inits.size(), 1);
    auto* set_node = std::get_if<core::Set>(mod.toplevel_inits[0]);
    BOOST_REQUIRE(set_node != nullptr);
    auto* g = std::get_if<core::Address::Global>(&set_node->target.where);
    BOOST_REQUIRE(g != nullptr);
    BOOST_CHECK_EQUAL(g->id, 0);
}

BOOST_AUTO_TEST_CASE(test_local_let_shadowing) {
    std::string src = "(module m1 (define x 1) (define (f x) (let ((x 2)) x)))";
    auto res = analyze_src(src);
    if (!res.has_value()) {
        BOOST_TEST_MESSAGE("Semantic error: " << res.error().message);
    }
    BOOST_REQUIRE(res.has_value());
    const auto& mod = (*res)[0];
    
    // bindings: x (global), f (global), x (param for f), x (param for let-lambda)
    // The expander transforms (let ((x 2)) x) into ((lambda (x) x) 2)
    BOOST_REQUIRE_GE(mod.bindings.size(), 4);
    
    // Find (define (f x) ...) which is (set! f (lambda (x) ...))
    BOOST_REQUIRE_EQUAL(mod.toplevel_inits.size(), 2);
    auto* set_f = std::get_if<core::Set>(mod.toplevel_inits[1]);
    BOOST_REQUIRE(set_f != nullptr);
    
    auto* lam = std::get_if<core::Lambda>(set_f->value);
    BOOST_REQUIRE(lam != nullptr);
    
    // After expansion, (let ((x 2)) x) becomes ((lambda (x) x) 2)
    // So lam->body is a Call node
    auto* call_node = std::get_if<core::Call>(lam->body);
    BOOST_REQUIRE(call_node != nullptr);

    // The callee of the call is a lambda
    auto* inner_lam = std::get_if<core::Lambda>(call_node->callee);
    BOOST_REQUIRE(inner_lam != nullptr);

    // The body of the inner lambda is the variable x referring to its param
    auto* var_node = std::get_if<core::Var>(inner_lam->body);
    BOOST_REQUIRE(var_node != nullptr);
    
    auto* local_addr = std::get_if<core::Address::Local>(&var_node->addr.where);
    BOOST_REQUIRE(local_addr != nullptr);
    
    // The binding ID for this local x (should be the inner lambda's param)
    core::BindingId bid{local_addr->slot};
    BOOST_REQUIRE_LT(bid.id, mod.bindings.size());
    BOOST_CHECK_EQUAL(mod.bindings[bid.id].name, "x");
    BOOST_CHECK(mod.bindings[bid.id].kind == BindingInfo::Kind::Param);
}

BOOST_AUTO_TEST_CASE(test_undefined_symbol) {
    std::string src = "(module m1 (define x y))";
    auto res = analyze_src(src);
    BOOST_REQUIRE(!res.has_value());
    BOOST_CHECK(res.error().kind == SemanticError::Kind::UndefinedName);
}

BOOST_AUTO_TEST_CASE(test_immutable_lambda_param) {
    // Lambda params are mutable to support letrec expansion (which uses set! on params).
    // This test verifies that set! on params is allowed.
    std::string src = "(module m1 (define (f x) (set! x 1)))";
    auto res = analyze_src(src);
    // Params are now mutable, so this should succeed
    BOOST_CHECK(res.has_value());
}

BOOST_AUTO_TEST_CASE(test_tail_calls) {
    std::string src = "(module m1 (define (f x) (if x (f 1) (f 2))))";
    auto res = analyze_src(src);
    BOOST_REQUIRE(res.has_value());
    const auto& mod = (*res)[0];
    
    auto* set_f = std::get_if<core::Set>(mod.toplevel_inits[0]);
    auto* lam = std::get_if<core::Lambda>(set_f->value);
    auto* if_node = std::get_if<core::If>(lam->body);
    BOOST_REQUIRE(if_node != nullptr);
    
    auto* call_conseq = std::get_if<core::Call>(if_node->conseq);
    auto* call_alt = std::get_if<core::Call>(if_node->alt);
    
    BOOST_REQUIRE(call_conseq != nullptr);
    BOOST_REQUIRE(call_alt != nullptr);
    
    BOOST_CHECK(call_conseq->tail == true);
    BOOST_CHECK(call_alt->tail == true);
}

BOOST_AUTO_TEST_CASE(test_upvals) {
    std::string src = "(module m1 (define (f x) (lambda (y) x)))";
    auto res = analyze_src(src);
    BOOST_REQUIRE(res.has_value());
    const auto& mod = (*res)[0];
    
    auto* set_f = std::get_if<core::Set>(mod.toplevel_inits[0]);
    auto* lam_f = std::get_if<core::Lambda>(set_f->value);
    auto* lam_inner = std::get_if<core::Lambda>(lam_f->body);
    BOOST_REQUIRE(lam_inner != nullptr);
    
    // Inner lambda should have 'x' as an upval
    BOOST_REQUIRE_EQUAL(lam_inner->upvals.size(), 1);
    auto upval_bid = lam_inner->upvals[0];
    BOOST_CHECK_EQUAL(mod.bindings[upval_bid.id].name, "x");
    
    auto* var_x = std::get_if<core::Var>(lam_inner->body);
    BOOST_REQUIRE(var_x != nullptr);
    auto* up_addr = std::get_if<core::Address::Upval>(&var_x->addr.where);
    BOOST_REQUIRE(up_addr != nullptr);
    BOOST_CHECK_EQUAL(up_addr->slot, 0);
}

BOOST_AUTO_TEST_CASE(test_letrec) {
    // After expansion, letrec becomes:
    // ((lambda (a b)
    //    (begin (set! a ...) (set! b ...) body))
    //  Nil Nil)
    // Using simpler mutually-referential lambdas that don't require external functions
    std::string src = "(module m1 (define (f x) (letrec ((a (lambda () b)) (b (lambda () a))) (a))))";
    auto res = analyze_src(src);
    if (!res.has_value()) {
        BOOST_TEST_MESSAGE("Semantic error: " << res.error().message);
    }
    BOOST_REQUIRE(res.has_value());
    const auto& mod = (*res)[0];
    
    auto* set_f = std::get_if<core::Set>(mod.toplevel_inits[0]);
    BOOST_REQUIRE(set_f != nullptr);
    auto* lam_f = std::get_if<core::Lambda>(set_f->value);
    BOOST_REQUIRE(lam_f != nullptr);

    // After expansion, letrec becomes a lambda call: ((lambda (a b) ...) Nil Nil)
    auto* call_node = std::get_if<core::Call>(lam_f->body);
    BOOST_REQUIRE(call_node != nullptr);

    auto* letrec_lam = std::get_if<core::Lambda>(call_node->callee);
    BOOST_REQUIRE(letrec_lam != nullptr);

    // The lambda should have 2 params (a and b)
    BOOST_CHECK_EQUAL(letrec_lam->params.size(), 2);

    // The body is a begin with set! initializers followed by the actual body
    auto* body_begin = std::get_if<core::Begin>(letrec_lam->body);
    BOOST_REQUIRE(body_begin != nullptr);
    BOOST_REQUIRE_GE(body_begin->exprs.size(), 3); // set! a, set! b, (a)
}

BOOST_AUTO_TEST_CASE(test_module_imports) {
    std::string src = "(module m1 (export a) (define a 42))\n(module m2 (import m1) (define b a))";
    auto res = analyze_src(src);
    BOOST_REQUIRE(res.has_value());
    BOOST_REQUIRE_EQUAL(res->size(), 2);
    
    const auto& m2 = (*res)[1];
    BOOST_CHECK_EQUAL(m2.name, "m2");
    
    // 'a' in 'm2' should be an import
    bool found_a = false;
    for (const auto& b : m2.bindings) {
        if (b.name == "a" && b.kind == BindingInfo::Kind::Import) {
            found_a = true;
            break;
        }
    }
    BOOST_CHECK(found_a);
}

BOOST_AUTO_TEST_CASE(test_if_arity_error) {
    std::string src = "(module m1 (define x (if 1 2)))"; // requires 3 args
    auto res = analyze_src(src);
    BOOST_REQUIRE(!res.has_value());
    BOOST_CHECK(res.error().kind == SemanticError::Kind::InvalidFormShape);
}

BOOST_AUTO_TEST_CASE(test_begin_tail) {
    std::string src = "(module m1 (define (f x) (begin (f 0) (f 1) (f 2))))";
    auto res = analyze_src(src);
    BOOST_REQUIRE(res.has_value());
    const auto& mod = (*res)[0];
    
    auto* set_f = std::get_if<core::Set>(mod.toplevel_inits[0]);
    auto* lam = std::get_if<core::Lambda>(set_f->value);
    auto* begin_node = std::get_if<core::Begin>(lam->body);
    BOOST_REQUIRE(begin_node != nullptr);
    BOOST_REQUIRE_EQUAL(begin_node->exprs.size(), 3);
    
    auto* call0 = std::get_if<core::Call>(begin_node->exprs[0]);
    auto* call1 = std::get_if<core::Call>(begin_node->exprs[1]);
    auto* call2 = std::get_if<core::Call>(begin_node->exprs[2]);
    
    BOOST_REQUIRE(call0 != nullptr);
    BOOST_REQUIRE(call1 != nullptr);
    BOOST_REQUIRE(call2 != nullptr);
    
    BOOST_CHECK_EQUAL(call0->tail, false);
    BOOST_CHECK_EQUAL(call1->tail, false);
    BOOST_CHECK_EQUAL(call2->tail, true);
}

BOOST_AUTO_TEST_CASE(test_lambda_arity) {
    auto res1 = analyze_src("(module m1 (define (f x y) 1))");
    BOOST_REQUIRE(res1.has_value());
    auto* lam1 = std::get_if<core::Lambda>(std::get_if<core::Set>((*res1)[0].toplevel_inits[0])->value);
    BOOST_CHECK_EQUAL(lam1->arity.required, 2);
    BOOST_CHECK_EQUAL(lam1->arity.has_rest, false);

    auto res2 = analyze_src("(module m1 (define (f x . y) 1))");
    BOOST_REQUIRE(res2.has_value());
    auto* lam2 = std::get_if<core::Lambda>(std::get_if<core::Set>((*res2)[0].toplevel_inits[0])->value);
    BOOST_CHECK_EQUAL(lam2->arity.required, 1);
    BOOST_CHECK_EQUAL(lam2->arity.has_rest, true);

    auto res3 = analyze_src("(module m1 (define (f . x) 1))");
    BOOST_REQUIRE(res3.has_value());
    auto* lam3 = std::get_if<core::Lambda>(std::get_if<core::Set>((*res3)[0].toplevel_inits[0])->value);
    BOOST_CHECK_EQUAL(lam3->arity.required, 0);
    BOOST_CHECK_EQUAL(lam3->arity.has_rest, true);
}

BOOST_AUTO_TEST_SUITE_END()
