/**
 * @file display_classifier_tests.cpp
 * @brief Unit tests for eta::session::DisplayClassifier.
 */

#include <boost/test/unit_test.hpp>

#include <cstdint>
#include <expected>
#include <string>
#include <utility>
#include <vector>

#include "eta/runtime/factory.h"
#include "eta/runtime/memory/heap.h"
#include "eta/runtime/memory/intern_table.h"
#include "eta/session/display_classifier.h"

namespace {

template <typename T, typename E>
T require_ok(std::expected<T, E> value) {
    BOOST_REQUIRE(value.has_value());
    return std::move(*value);
}

} // namespace

BOOST_AUTO_TEST_SUITE(display_classifier_tests)

BOOST_AUTO_TEST_CASE(display_tag_for_mime_maps_known_types) {
    using eta::session::DisplayClassifier;
    using eta::session::DisplayTag;

    BOOST_TEST(
        static_cast<int>(DisplayClassifier::display_tag_for_mime("text/html")) ==
        static_cast<int>(DisplayTag::Html));
    BOOST_TEST(
        static_cast<int>(DisplayClassifier::display_tag_for_mime("image/png")) ==
        static_cast<int>(DisplayTag::Png));
    BOOST_TEST(
        static_cast<int>(DisplayClassifier::display_tag_for_mime("application/vnd.eta.facttable+json")) ==
        static_cast<int>(DisplayTag::FactTable));
    BOOST_TEST(
        static_cast<int>(DisplayClassifier::display_tag_for_mime("application/octet-stream")) ==
        static_cast<int>(DisplayTag::Text));
}

BOOST_AUTO_TEST_CASE(try_decode_string_decodes_symbols_and_strings) {
    namespace memory = eta::runtime::memory;
    using eta::runtime::nanbox::ops::encode;

    memory::heap::Heap heap(1024 * 1024);
    memory::intern::InternTable intern_table;
    eta::session::DisplayClassifier classifier(heap, intern_table);

    const auto symbol_value = require_ok(memory::factory::make_symbol(intern_table, "jupyter-display"));
    const auto string_value = require_ok(memory::factory::make_string(heap, intern_table, "text/html"));
    const auto fixnum_value = require_ok(encode<std::int64_t>(42));

    std::string decoded;
    BOOST_TEST(classifier.try_decode_string(symbol_value, &decoded));
    BOOST_TEST(decoded == "jupyter-display");

    decoded.clear();
    BOOST_TEST(classifier.try_decode_string(string_value, &decoded));
    BOOST_TEST(decoded == "text/html");

    decoded.clear();
    BOOST_TEST(!classifier.try_decode_string(fixnum_value, &decoded));
}

BOOST_AUTO_TEST_CASE(try_unpack_jupyter_display_extracts_mime_and_payload) {
    namespace memory = eta::runtime::memory;

    memory::heap::Heap heap(1024 * 1024);
    memory::intern::InternTable intern_table;
    eta::session::DisplayClassifier classifier(heap, intern_table);

    const auto marker = require_ok(memory::factory::make_symbol(intern_table, "jupyter-display"));
    const auto mime = require_ok(memory::factory::make_string(heap, intern_table, "text/markdown"));
    const auto payload = require_ok(memory::factory::make_string(heap, intern_table, "**ok**"));
    const auto wrapper = require_ok(memory::factory::make_vector(heap, {marker, mime, payload}));

    std::string unpacked_mime;
    eta::runtime::nanbox::LispVal unpacked_payload{eta::runtime::nanbox::Nil};
    BOOST_TEST(classifier.try_unpack_jupyter_display(wrapper, &unpacked_mime, &unpacked_payload));
    BOOST_TEST(unpacked_mime == "text/markdown");
    BOOST_TEST(unpacked_payload == payload);
}

BOOST_AUTO_TEST_CASE(classify_display_tag_handles_wrapper_fact_table_and_plain_values) {
    namespace memory = eta::runtime::memory;
    using eta::session::DisplayTag;

    memory::heap::Heap heap(1024 * 1024);
    memory::intern::InternTable intern_table;
    eta::session::DisplayClassifier classifier(heap, intern_table);

    const auto marker = require_ok(memory::factory::make_symbol(intern_table, "jupyter-display"));
    const auto html_mime = require_ok(memory::factory::make_string(heap, intern_table, "text/html"));
    const auto payload = require_ok(memory::factory::make_string(heap, intern_table, "<b>ok</b>"));
    const auto wrapper = require_ok(memory::factory::make_vector(heap, {marker, html_mime, payload}));
    BOOST_TEST(
        static_cast<int>(classifier.classify_display_tag(wrapper)) ==
        static_cast<int>(DisplayTag::Html));

    const auto fact_table = require_ok(memory::factory::make_fact_table(heap, {"x"}));
    BOOST_TEST(
        static_cast<int>(classifier.classify_display_tag(fact_table)) ==
        static_cast<int>(DisplayTag::FactTable));

    const auto plain = require_ok(eta::runtime::nanbox::ops::encode<std::int64_t>(7));
    BOOST_TEST(
        static_cast<int>(classifier.classify_display_tag(plain)) ==
        static_cast<int>(DisplayTag::Text));
}

BOOST_AUTO_TEST_SUITE_END()
