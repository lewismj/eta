#include <boost/test/unit_test.hpp>

#include "eta/reader/lexer.h"
#include "eta/reader/parser.h"
#include "eta/reader/expander.h"
#include "eta/reader/module_linker.h"

using namespace eta;

static std::vector<reader::parser::SExprPtr> parse_and_expand(std::string_view program) {
    reader::lexer::Lexer lex(0 /*FileId*/, program);
    reader::parser::Parser p(lex);
    auto parsed = p.parse_toplevel();
    BOOST_REQUIRE(parsed.has_value());

    reader::expander::Expander ex;
    auto expanded = ex.expand_many(*parsed);
    BOOST_REQUIRE(expanded.has_value());
    return std::move(*expanded);
}

BOOST_AUTO_TEST_SUITE(module_linker_tests)

BOOST_AUTO_TEST_CASE(linker_plain_import) {
    auto forms = parse_and_expand("(module m1 (define a 1) (define b 2) (export a b))\n(module m2 (import m1) (define x a))");
    reader::ModuleLinker L;
    auto idx = L.index_modules(std::span<const reader::parser::SExprPtr>(forms.data(), forms.size()));
    BOOST_REQUIRE(idx.has_value());
    auto lk = L.link(); BOOST_REQUIRE(lk.has_value());
    auto m2 = L.get("m2"); BOOST_REQUIRE(m2.has_value());
    const auto& M2 = m2->get();
    BOOST_CHECK(M2.visible.contains("a"));
    BOOST_CHECK(M2.visible.contains("b"));
    BOOST_CHECK(M2.visible.contains("x"));
}

BOOST_AUTO_TEST_CASE(linker_only_import) {
    auto forms = parse_and_expand("(module m1 (define a 1) (define b 2) (define c 3) (export a b c))\n(module m2 (import (only m1 a c)) (define y (+ a c)))");
    reader::ModuleLinker L;
    auto idx = L.index_modules(std::span<const reader::parser::SExprPtr>(forms.data(), forms.size()));
    BOOST_REQUIRE(idx.has_value());
    auto lk = L.link(); BOOST_REQUIRE(lk.has_value());
    auto m2 = L.get("m2"); BOOST_REQUIRE(m2.has_value());
    const auto& M2 = m2->get();
    BOOST_CHECK(M2.visible.contains("a"));
    BOOST_CHECK(M2.visible.contains("c"));
    BOOST_CHECK(M2.visible.contains("y"));
    BOOST_CHECK(!M2.visible.contains("b"));
}

BOOST_AUTO_TEST_CASE(linker_except_import) {
    auto forms = parse_and_expand("(module m1 (define a 1) (define b 2) (define c 3) (export a b c))\n(module m2 (import (except m1 b)) (define y (+ a c)))");
    reader::ModuleLinker L;
    auto idx = L.index_modules(std::span<const reader::parser::SExprPtr>(forms.data(), forms.size()));
    BOOST_REQUIRE(idx.has_value());
    auto lk = L.link(); BOOST_REQUIRE(lk.has_value());
    auto m2 = L.get("m2"); BOOST_REQUIRE(m2.has_value());
    const auto& M2 = m2->get();
    BOOST_CHECK(M2.visible.contains("a"));
    BOOST_CHECK(M2.visible.contains("c"));
    BOOST_CHECK(M2.visible.contains("y"));
    BOOST_CHECK(!M2.visible.contains("b"));
}

BOOST_AUTO_TEST_CASE(linker_rename_import) {
    auto forms = parse_and_expand("(module m1 (define a 1) (define c 3) (export a c))\n(module m2 (import (rename m1 (a x) (c z))) (define y (+ x z)))");
    reader::ModuleLinker L;
    auto idx = L.index_modules(std::span<const reader::parser::SExprPtr>(forms.data(), forms.size()));
    BOOST_REQUIRE(idx.has_value());
    auto lk = L.link(); BOOST_REQUIRE(lk.has_value());
    auto m2 = L.get("m2"); BOOST_REQUIRE(m2.has_value());
    const auto& M2 = m2->get();
    BOOST_CHECK(M2.visible.contains("x"));
    BOOST_CHECK(M2.visible.contains("z"));
    BOOST_CHECK(M2.visible.contains("y"));
    BOOST_CHECK(!M2.visible.contains("a"));
    BOOST_CHECK(!M2.visible.contains("c"));
}

BOOST_AUTO_TEST_CASE(linker_conflicting_import_vs_local_define) {
    auto forms = parse_and_expand("(module m1 (define a 1) (export a))\n(module m2 (define a 2) (import m1))");
    reader::ModuleLinker L;
    auto idx = L.index_modules(std::span<const reader::parser::SExprPtr>(forms.data(), forms.size()));
    BOOST_REQUIRE(idx.has_value());
    auto lk = L.link();
    BOOST_REQUIRE(!lk.has_value());
    BOOST_CHECK(lk.error().kind == eta::reader::LinkError::Kind::ConflictingImport);
}

BOOST_AUTO_TEST_CASE(linker_export_of_missing_name) {
    auto forms = parse_and_expand("(module m1 (export x))");
    reader::ModuleLinker L;
    auto idx = L.index_modules(std::span<const reader::parser::SExprPtr>(forms.data(), forms.size()));
    BOOST_REQUIRE(idx.has_value());
    auto lk = L.link();
    BOOST_REQUIRE(!lk.has_value());
    BOOST_CHECK(lk.error().kind == eta::reader::LinkError::Kind::ExportOfUnknownName);
}

BOOST_AUTO_TEST_CASE(linker_reexport_imported_name) {
    auto forms = parse_and_expand(
        "(module m1 (define a 1) (define b 2) (export a b))\n"
        "(module m2 (import m1) (export a b))");
    reader::ModuleLinker L;
    auto idx = L.index_modules(std::span<const reader::parser::SExprPtr>(forms.data(), forms.size()));
    BOOST_REQUIRE(idx.has_value());
    auto lk = L.link();
    BOOST_REQUIRE(lk.has_value());
    auto m2 = L.get("m2"); BOOST_REQUIRE(m2.has_value());
    const auto& M2 = m2->get();
    BOOST_CHECK(M2.exports.contains("a"));
    BOOST_CHECK(M2.exports.contains("b"));
    BOOST_CHECK(M2.visible.contains("a"));
    BOOST_CHECK(M2.visible.contains("b"));
}

BOOST_AUTO_TEST_CASE(linker_circular_imports) {
    auto forms = parse_and_expand("(module A (export a) (import B) (define a 1))\n(module B (export b) (import A) (define b a))");
    reader::ModuleLinker L;
    auto idx = L.index_modules(std::span<const reader::parser::SExprPtr>(forms.data(), forms.size()));
    BOOST_REQUIRE(idx.has_value());
    auto lk = L.link();
    BOOST_REQUIRE(!lk.has_value());
    BOOST_CHECK(lk.error().kind == reader::LinkError::Kind::CircularDependency);
}

BOOST_AUTO_TEST_CASE(linker_multi_clause_imports) {
    auto forms = parse_and_expand("(module m1 (define a 1) (define b 2) (export a b))\n(module m2 (import (only m1 a) (rename m1 (b x))) (define y (+ a x)))");
    reader::ModuleLinker L;
    auto idx = L.index_modules(std::span<const reader::parser::SExprPtr>(forms.data(), forms.size()));
    BOOST_REQUIRE(idx.has_value());
    auto lk = L.link();
    BOOST_REQUIRE(lk.has_value());
    auto m2 = L.get("m2"); BOOST_REQUIRE(m2.has_value());
    const auto& M2 = m2->get();
    BOOST_CHECK(M2.visible.contains("a"));
    BOOST_CHECK(M2.visible.contains("x"));
    BOOST_CHECK(M2.visible.contains("y"));
    BOOST_CHECK(!M2.visible.contains("b"));
}

BOOST_AUTO_TEST_SUITE_END()
