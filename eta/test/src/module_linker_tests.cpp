#include <boost/test/unit_test.hpp>

#include "eta/reader/lexer.h"
#include "eta/reader/parser.h"
#include "eta/reader/expander.h"
#include "eta/reader/module_linker.h"

using namespace eta;

/// Safely extract the error message from a LinkResult<void>.
/// Returns "" if the result holds a value, avoiding UB from accessing
/// the inactive union member of std::expected.
static std::string link_error_msg(const reader::LinkResult<void>& r) {
    return r.has_value() ? std::string{} : r.error().message;
}

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

// ─── Prefix import tests ─────────────────────────────────────────────

BOOST_AUTO_TEST_CASE(linker_prefix_import_basic) {
    // (prefix m1 m1:) should import all exports with "m1:" prefix
    auto forms = parse_and_expand(
        "(module m1 (define a 1) (define b 2) (export a b))\n"
        "(module m2 (import (prefix m1 m1:)) (define y m1:a))");
    reader::ModuleLinker L;
    auto idx = L.index_modules(std::span<const reader::parser::SExprPtr>(forms.data(), forms.size()));
    BOOST_REQUIRE(idx.has_value());
    auto lk = L.link();
    BOOST_REQUIRE_MESSAGE(lk.has_value(), link_error_msg(lk));
    auto m2 = L.get("m2"); BOOST_REQUIRE(m2.has_value());
    const auto& M2 = m2->get();
    BOOST_CHECK(M2.visible.contains("m1:a"));
    BOOST_CHECK(M2.visible.contains("m1:b"));
    BOOST_CHECK(M2.visible.contains("y"));
    // Original unprefixed names should NOT be visible
    BOOST_CHECK(!M2.visible.contains("a"));
    BOOST_CHECK(!M2.visible.contains("b"));
}

BOOST_AUTO_TEST_CASE(linker_prefix_import_provenance) {
    // Verify import_origins correctly maps prefixed names back to remote names
    auto forms = parse_and_expand(
        "(module m1 (define foo 42) (export foo))\n"
        "(module m2 (import (prefix m1 m1.)))");
    reader::ModuleLinker L;
    auto idx = L.index_modules(std::span<const reader::parser::SExprPtr>(forms.data(), forms.size()));
    BOOST_REQUIRE(idx.has_value());
    auto lk = L.link();
    BOOST_REQUIRE_MESSAGE(lk.has_value(), link_error_msg(lk));
    auto m2 = L.get("m2"); BOOST_REQUIRE(m2.has_value());
    const auto& M2 = m2->get();
    BOOST_CHECK(M2.visible.contains("m1.foo"));
    BOOST_REQUIRE(M2.import_origins.contains("m1.foo"));
    BOOST_CHECK_EQUAL(M2.import_origins.at("m1.foo").from_module, "m1");
    BOOST_CHECK_EQUAL(M2.import_origins.at("m1.foo").remote_name, "foo");
}

BOOST_AUTO_TEST_CASE(linker_prefix_no_conflict_with_local) {
    // Prefix avoids name conflict that would otherwise occur
    auto forms = parse_and_expand(
        "(module m1 (define a 1) (export a))\n"
        "(module m2 (define a 2) (import (prefix m1 m1:)))");
    reader::ModuleLinker L;
    auto idx = L.index_modules(std::span<const reader::parser::SExprPtr>(forms.data(), forms.size()));
    BOOST_REQUIRE(idx.has_value());
    auto lk = L.link();
    BOOST_REQUIRE_MESSAGE(lk.has_value(), link_error_msg(lk));
    auto m2 = L.get("m2"); BOOST_REQUIRE(m2.has_value());
    const auto& M2 = m2->get();
    BOOST_CHECK(M2.visible.contains("a"));     // local
    BOOST_CHECK(M2.visible.contains("m1:a"));  // prefixed import
}

BOOST_AUTO_TEST_CASE(linker_prefix_conflict_with_local) {
    // If the prefixed name still collides with a local define, error
    auto forms = parse_and_expand(
        "(module m1 (define a 1) (export a))\n"
        "(module m2 (define m1:a 2) (import (prefix m1 m1:)))");
    reader::ModuleLinker L;
    auto idx = L.index_modules(std::span<const reader::parser::SExprPtr>(forms.data(), forms.size()));
    BOOST_REQUIRE(idx.has_value());
    auto lk = L.link();
    BOOST_REQUIRE(!lk.has_value());
    BOOST_CHECK(lk.error().kind == reader::LinkError::Kind::ConflictingImport);
}

BOOST_AUTO_TEST_CASE(linker_prefix_multi_module) {
    // Two modules imported with different prefixes
    auto forms = parse_and_expand(
        "(module m1 (define x 1) (export x))\n"
        "(module m2 (define x 2) (export x))\n"
        "(module m3 (import (prefix m1 a:) (prefix m2 b:)))");
    reader::ModuleLinker L;
    auto idx = L.index_modules(std::span<const reader::parser::SExprPtr>(forms.data(), forms.size()));
    BOOST_REQUIRE(idx.has_value());
    auto lk = L.link();
    BOOST_REQUIRE_MESSAGE(lk.has_value(), link_error_msg(lk));
    auto m3 = L.get("m3"); BOOST_REQUIRE(m3.has_value());
    const auto& M3 = m3->get();
    BOOST_CHECK(M3.visible.contains("a:x"));
    BOOST_CHECK(M3.visible.contains("b:x"));
    BOOST_CHECK(!M3.visible.contains("x"));
}

BOOST_AUTO_TEST_CASE(linker_prefix_reexport) {
    // A prefix-imported name can be re-exported
    auto forms = parse_and_expand(
        "(module m1 (define a 1) (export a))\n"
        "(module m2 (import (prefix m1 m1:)) (export m1:a))");
    reader::ModuleLinker L;
    auto idx = L.index_modules(std::span<const reader::parser::SExprPtr>(forms.data(), forms.size()));
    BOOST_REQUIRE(idx.has_value());
    auto lk = L.link();
    BOOST_REQUIRE_MESSAGE(lk.has_value(), link_error_msg(lk));
    auto m2 = L.get("m2"); BOOST_REQUIRE(m2.has_value());
    const auto& M2 = m2->get();
    BOOST_CHECK(M2.exports.contains("m1:a"));
    BOOST_CHECK(M2.visible.contains("m1:a"));
}

BOOST_AUTO_TEST_CASE(linker_prefix_bad_syntax) {
    // (prefix) with wrong number of args — error during indexing
    auto forms = parse_and_expand(
        "(module m1 (define a 1) (export a))\n"
        "(module m2 (import (prefix m1)))");
    reader::ModuleLinker L;
    auto idx = L.index_modules(std::span<const reader::parser::SExprPtr>(forms.data(), forms.size()));
    BOOST_CHECK(!idx.has_value());
}

BOOST_AUTO_TEST_SUITE_END()
