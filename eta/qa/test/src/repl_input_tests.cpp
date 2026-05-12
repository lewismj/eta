/**
 * @file repl_input_tests.cpp
 * @brief Unit tests for shared REPL input helpers.
 */

#include <boost/test/unit_test.hpp>

#include <cstddef>
#include <string>
#include <vector>

#include "eta/session/repl_input.h"

BOOST_AUTO_TEST_SUITE(repl_input_tests)

BOOST_AUTO_TEST_CASE(split_toplevel_forms_handles_forms_atoms_and_comments) {
    const std::string input =
        "(define seed 40)\n"
        "(define marker \"semi;inside\")\n"
        "; comment between forms\n"
        "seed\n"
        "(+ seed 2)\n";

    const auto forms = eta::session::split_toplevel_forms(input);
    const std::vector<std::string> expected = {
        "(define seed 40)",
        "(define marker \"semi;inside\")",
        "seed",
        "(+ seed 2)",
    };

    BOOST_REQUIRE_EQUAL(forms.size(), expected.size());
    for (std::size_t i = 0; i < expected.size(); ++i) {
        BOOST_TEST(forms[i] == expected[i]);
    }
}

BOOST_AUTO_TEST_CASE(split_toplevel_forms_keeps_non_whitespace_trailing_fragment) {
    const auto forms = eta::session::split_toplevel_forms("(+ 1");
    BOOST_REQUIRE_EQUAL(forms.size(), 1u);
    BOOST_TEST(forms.front() == "(+ 1");
}

BOOST_AUTO_TEST_CASE(is_complete_repl_input_reports_indent_for_open_paren_and_dot_continuation) {
    std::string indent;

    BOOST_TEST(!eta::session::is_complete_repl_input("(+ 1", &indent));
    BOOST_TEST(indent == "  ");

    BOOST_TEST(!eta::session::is_complete_repl_input("(+ 1 2)\n.cont", &indent));
    BOOST_TEST(indent == "  ");
}

BOOST_AUTO_TEST_CASE(is_complete_repl_input_handles_comments_strings_and_nested_block_comments) {
    std::string indent;

    BOOST_TEST(eta::session::is_complete_repl_input("; comment only", &indent));
    BOOST_TEST(indent.empty());

    BOOST_TEST(!eta::session::is_complete_repl_input("\"unterminated", &indent));
    BOOST_TEST(indent.empty());

    BOOST_TEST(!eta::session::is_complete_repl_input("#| nested #| block |# still-open", &indent));
    BOOST_TEST(eta::session::is_complete_repl_input("#| nested #| block |# done |#", &indent));
}

BOOST_AUTO_TEST_SUITE_END()
