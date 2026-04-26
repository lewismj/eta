/**
 * @file jupyter_magics_test.cpp
 * @brief Regression tests for Jupyter magic parsing helpers.
 */

#include <boost/test/unit_test.hpp>

#include "eta/jupyter/magics.h"

BOOST_AUTO_TEST_SUITE(jupyter_magics_tests)

BOOST_AUTO_TEST_CASE(parse_line_magic_time_with_expression) {
    const auto parsed = eta::jupyter::parse_magic("%time (+ 1 2)");
    BOOST_TEST(static_cast<int>(parsed.kind) == static_cast<int>(eta::jupyter::MagicKind::Line));
    BOOST_TEST(static_cast<int>(parsed.name) == static_cast<int>(eta::jupyter::MagicName::Time));
    BOOST_TEST(parsed.args == "(+ 1 2)");
}

BOOST_AUTO_TEST_CASE(parse_line_magic_with_leading_whitespace) {
    const auto parsed = eta::jupyter::parse_magic("   %reload std.logic  ");
    BOOST_TEST(static_cast<int>(parsed.kind) == static_cast<int>(eta::jupyter::MagicKind::Line));
    BOOST_TEST(static_cast<int>(parsed.name) == static_cast<int>(eta::jupyter::MagicName::Reload));
    BOOST_TEST(parsed.args == "std.logic");
}

BOOST_AUTO_TEST_CASE(parse_cell_magic_trace_with_body) {
    const auto parsed = eta::jupyter::parse_magic("%%trace\n(+ 1 2)\n(* 3 4)\n");
    BOOST_TEST(static_cast<int>(parsed.kind) == static_cast<int>(eta::jupyter::MagicKind::Cell));
    BOOST_TEST(static_cast<int>(parsed.name) == static_cast<int>(eta::jupyter::MagicName::Trace));
    BOOST_TEST(parsed.args.empty());
    BOOST_TEST(parsed.body == "(+ 1 2)\n(* 3 4)\n");
}

BOOST_AUTO_TEST_CASE(parse_non_magic_returns_none) {
    const auto parsed = eta::jupyter::parse_magic("(+ 1 2)");
    BOOST_TEST(static_cast<int>(parsed.kind) == static_cast<int>(eta::jupyter::MagicKind::None));
    BOOST_TEST(static_cast<int>(parsed.name) == static_cast<int>(eta::jupyter::MagicName::Unknown));
}

BOOST_AUTO_TEST_SUITE_END()
