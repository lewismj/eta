#include <boost/test/unit_test.hpp>

#include <string>

#include <eta/docs/markdown.h>
#include <eta/docs/stdlib_docs.h>

BOOST_AUTO_TEST_SUITE(stdlib_doc_tests)

BOOST_AUTO_TEST_CASE(lookup_stdlib_doc_known_binding_returns_markdown) {
    auto entry = eta::docs::lookup_stdlib_doc("assert-equal");
    BOOST_REQUIRE(entry.has_value());

    const auto markdown = eta::docs::render_markdown(*entry);
    BOOST_TEST(markdown.find("**assert-equal**") != std::string::npos);
    BOOST_TEST(markdown.find("(assert-equal expected actual . rest)") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(lookup_stdlib_doc_unknown_symbol_returns_null) {
    auto entry = eta::docs::lookup_stdlib_doc("__eta_stdlib_doc_missing__");
    BOOST_TEST(!entry.has_value());
}

BOOST_AUTO_TEST_CASE(stdlib_doc_registry_has_no_duplicate_symbols) {
    const auto duplicates = eta::docs::duplicate_stdlib_doc_symbols();
    if (!duplicates.empty()) {
        std::string message = "Duplicate stdlib docs:";
        for (const auto& symbol : duplicates) message += " " + symbol;
        BOOST_FAIL(message);
    }
}

BOOST_AUTO_TEST_SUITE_END()
