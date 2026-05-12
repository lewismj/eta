/**
 * @file source_file_registry_tests.cpp
 * @brief Unit tests for eta::session::SourceFileRegistry.
 */

#include <boost/test/unit_test.hpp>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>

#include "eta/reader/lexer.h"
#include "eta/semantics/emitter.h"
#include "eta/session/source_file_registry.h"

namespace fs = std::filesystem;

namespace {

/**
 * @brief Temporary directory guard used by SourceFileRegistry tests.
 */
struct TempDir {
    fs::path path;

    TempDir() {
        const auto suffix = std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count());
        path = fs::temp_directory_path() / ("eta_source_file_registry_test_" + suffix);
        fs::create_directories(path);
    }

    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }

    fs::path write_file(const std::string& rel) const {
        const auto full = path / rel;
        fs::create_directories(full.parent_path());
        std::ofstream out(full, std::ios::out | std::ios::binary | std::ios::trunc);
        out << "(module test)\n";
        return full;
    }
};

eta::reader::lexer::Span make_span(uint32_t file_id, uint32_t line) {
    return {
        file_id,
        eta::reader::lexer::Position{0, line, 1},
        eta::reader::lexer::Position{0, line, 1}
    };
}

} // namespace

BOOST_AUTO_TEST_SUITE(source_file_registry_tests)

BOOST_AUTO_TEST_CASE(ensure_file_id_deduplicates_equivalent_paths) {
    TempDir temp;
    const auto source_path = temp.write_file("nested/MixedCase.eta");

    eta::session::SourceFileRegistry source_files;
    const auto file_id = source_files.ensure_file_id(source_path);
    BOOST_REQUIRE(file_id != 0u);

    const auto alias_path =
        source_path.parent_path() / "." / ".." / source_path.parent_path().filename()
        / source_path.filename();
    BOOST_TEST(source_files.ensure_file_id(alias_path) == file_id);
    BOOST_TEST(source_files.file_id_for_path(source_path.string()) == file_id);

#if defined(_WIN32)
    const auto forward = source_path.generic_string();
    auto backward = forward;
    std::replace(backward.begin(), backward.end(), '/', '\\');
    auto upper = backward;
    std::transform(
        upper.begin(),
        upper.end(),
        upper.begin(),
        [](unsigned char ch) {
            return static_cast<char>(std::toupper(ch));
        });

    BOOST_TEST(source_files.file_id_for_path(forward) == file_id);
    BOOST_TEST(source_files.file_id_for_path(backward) == file_id);
    BOOST_TEST(source_files.file_id_for_path(upper) == file_id);
#endif
}

BOOST_AUTO_TEST_CASE(file_resolver_and_path_lookup_round_trip) {
    TempDir temp;
    const auto source_path = temp.write_file("src/example.eta");

    eta::session::SourceFileRegistry source_files;
    const auto file_id = source_files.allocate_file_id(source_path.string());
    BOOST_REQUIRE(file_id != 0u);

    const auto* stored_path = source_files.path_for_file_id(file_id);
    BOOST_REQUIRE(stored_path != nullptr);
    BOOST_TEST(stored_path->filename() == fs::path("example.eta"));
    BOOST_TEST(source_files.path_for_file_id(file_id + 100u) == nullptr);

    const auto resolve = source_files.file_resolver();
    BOOST_TEST(resolve(file_id) == "example.eta");
    BOOST_TEST(resolve(file_id + 100u).empty());
}

BOOST_AUTO_TEST_CASE(valid_lines_for_collects_unique_nonzero_lines) {
    eta::session::SourceFileRegistry source_files;
    const auto file_id = source_files.allocate_file_id("module.eta");
    BOOST_REQUIRE(file_id != 0u);

    eta::semantics::BytecodeFunctionRegistry function_registry;

    eta::runtime::vm::BytecodeFunction first;
    first.source_map.push_back(make_span(file_id, 3u));
    first.source_map.push_back(make_span(file_id, 3u));
    first.source_map.push_back(make_span(file_id, 7u));
    first.source_map.push_back(make_span(file_id, 0u));
    first.source_map.push_back(make_span(file_id + 1u, 9u));
    function_registry.add(std::move(first));

    eta::runtime::vm::BytecodeFunction second;
    second.source_map.push_back(make_span(file_id, 12u));
    function_registry.add(std::move(second));

    const auto lines = source_files.valid_lines_for(file_id, function_registry);
    BOOST_REQUIRE_EQUAL(lines.size(), 3u);
    BOOST_TEST(lines.count(3u) == 1u);
    BOOST_TEST(lines.count(7u) == 1u);
    BOOST_TEST(lines.count(12u) == 1u);
}

BOOST_AUTO_TEST_CASE(valid_lines_for_returns_empty_for_unknown_id) {
    eta::session::SourceFileRegistry source_files;
    eta::semantics::BytecodeFunctionRegistry function_registry;

    eta::runtime::vm::BytecodeFunction fn;
    fn.source_map.push_back(make_span(17u, 5u));
    function_registry.add(std::move(fn));

    BOOST_TEST(source_files.valid_lines_for(0u, function_registry).empty());
    BOOST_TEST(source_files.valid_lines_for(42u, function_registry).empty());
}

BOOST_AUTO_TEST_SUITE_END()
