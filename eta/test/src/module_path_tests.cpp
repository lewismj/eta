/**
 * @file module_path_tests.cpp
 * @brief Unit tests for eta::interpreter::ModulePathResolver
 */

#include <boost/test/unit_test.hpp>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>

#include "eta/interpreter/module_path.h"

namespace fs = std::filesystem;

// helpers

/// RAII temporary directory that creates / destroys itself automatically.
struct TempDir {
    fs::path path;

    TempDir() {
        auto base = fs::temp_directory_path()
                  / ("eta_mpr_test_" + std::to_string(
                         std::chrono::steady_clock::now().time_since_epoch().count()));
        fs::create_directories(base);
        path = base;
    }

    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }

    /// Create a file relative to this directory with the given content.
    fs::path create_file(const std::string& rel, const std::string& content = "") const {
        auto full = path / rel;
        fs::create_directories(full.parent_path());
        std::ofstream f(full);
        f << content;
        return full;
    }
};

// test suite

BOOST_AUTO_TEST_SUITE(module_path_resolver_tests)

// Bring the class name into scope within this test suite namespace.
using eta::interpreter::ModulePathResolver;

// from_path_string

BOOST_AUTO_TEST_CASE(empty_path_string_gives_empty_resolver) {
    auto r = ModulePathResolver::from_path_string("");
    BOOST_TEST(r.empty());
    BOOST_TEST(r.dirs().empty());
}

BOOST_AUTO_TEST_CASE(single_dir_path_string) {
    auto r = ModulePathResolver::from_path_string("/usr/local/lib");
    BOOST_REQUIRE_EQUAL(r.dirs().size(), 1u);
    BOOST_TEST(r.dirs()[0] == fs::path("/usr/local/lib"));
}

BOOST_AUTO_TEST_CASE(multi_dir_path_string) {
#ifdef _WIN32
    auto r = ModulePathResolver::from_path_string("C:\\foo;C:\\bar;C:\\baz");
    BOOST_REQUIRE_EQUAL(r.dirs().size(), 3u);
    BOOST_TEST(r.dirs()[0] == fs::path("C:\\foo"));
    BOOST_TEST(r.dirs()[1] == fs::path("C:\\bar"));
    BOOST_TEST(r.dirs()[2] == fs::path("C:\\baz"));
#else
    auto r = ModulePathResolver::from_path_string("/a:/b:/c");
    BOOST_REQUIRE_EQUAL(r.dirs().size(), 3u);
    BOOST_TEST(r.dirs()[0] == fs::path("/a"));
    BOOST_TEST(r.dirs()[1] == fs::path("/b"));
    BOOST_TEST(r.dirs()[2] == fs::path("/c"));
#endif
}

BOOST_AUTO_TEST_CASE(trailing_separator_is_ignored) {
#ifdef _WIN32
    auto r = ModulePathResolver::from_path_string("C:\\foo;");
#else
    auto r = ModulePathResolver::from_path_string("/a:");
#endif
    BOOST_REQUIRE_EQUAL(r.dirs().size(), 1u);
}

BOOST_AUTO_TEST_CASE(consecutive_separators_are_ignored) {
#ifdef _WIN32
    auto r = ModulePathResolver::from_path_string("C:\\a;;C:\\b");
#else
    auto r = ModulePathResolver::from_path_string("/a::/b");
#endif
    BOOST_REQUIRE_EQUAL(r.dirs().size(), 2u);
}

// module_to_relative

BOOST_AUTO_TEST_CASE(module_to_relative_simple) {
    auto p = ModulePathResolver::module_to_relative("std.core");
    BOOST_TEST(p == fs::path("std/core.eta"));
}

BOOST_AUTO_TEST_CASE(module_to_relative_single_component) {
    auto p = ModulePathResolver::module_to_relative("prelude");
    BOOST_TEST(p == fs::path("prelude.eta"));
}

BOOST_AUTO_TEST_CASE(module_to_relative_deep) {
    auto p = ModulePathResolver::module_to_relative("a.b.c.d");
    BOOST_TEST(p == fs::path("a/b/c/d.eta"));
}

// resolve

BOOST_AUTO_TEST_CASE(resolve_finds_file_in_first_dir) {
    TempDir d1, d2;
    d1.create_file("std/core.eta", "(module std.core)");
    d2.create_file("std/core.eta", "(module std.core-other)");

    ModulePathResolver r{{d1.path, d2.path}};
    auto result = r.resolve("std.core");
    BOOST_REQUIRE(result.has_value());
    BOOST_TEST(result->string().find(d1.path.string()) != std::string::npos);
}

BOOST_AUTO_TEST_CASE(resolve_falls_back_to_second_dir) {
    TempDir d1, d2;
    d2.create_file("std/core.eta", "(module std.core)");

    ModulePathResolver r{{d1.path, d2.path}};
    auto result = r.resolve("std.core");
    BOOST_REQUIRE(result.has_value());
    BOOST_TEST(result->string().find(d2.path.string()) != std::string::npos);
}

BOOST_AUTO_TEST_CASE(resolve_returns_nullopt_when_not_found) {
    TempDir d;
    ModulePathResolver r{{d.path}};
    auto result = r.resolve("nonexistent.module");
    BOOST_TEST(!result.has_value());
}

BOOST_AUTO_TEST_CASE(resolve_empty_resolver_returns_nullopt) {
    ModulePathResolver r;
    BOOST_TEST(!r.resolve("std.core").has_value());
}

BOOST_AUTO_TEST_CASE(resolve_only_matches_regular_files) {
    TempDir d;
    // Create a directory where the file should be (not a regular file)
    fs::create_directories(d.path / "std" / "core.eta");
    ModulePathResolver r{{d.path}};
    BOOST_TEST(!r.resolve("std.core").has_value());
}

// find_file

BOOST_AUTO_TEST_CASE(find_file_locates_prelude) {
    TempDir d;
    d.create_file("prelude.eta", "(module prelude)");

    ModulePathResolver r{{d.path}};
    auto result = r.find_file("prelude.eta");
    BOOST_REQUIRE(result.has_value());
    BOOST_TEST(result->filename() == "prelude.eta");
}

BOOST_AUTO_TEST_CASE(find_file_returns_nullopt_when_absent) {
    TempDir d;
    ModulePathResolver r{{d.path}};
    BOOST_TEST(!r.find_file("missing.eta").has_value());
}

BOOST_AUTO_TEST_CASE(find_file_searches_dirs_in_order) {
    TempDir d1, d2;
    d2.create_file("prelude.eta", "(module prelude)");

    ModulePathResolver r{{d1.path, d2.path}};
    auto result = r.find_file("prelude.eta");
    BOOST_REQUIRE(result.has_value());
    BOOST_TEST(result->string().find(d2.path.string()) != std::string::npos);
}

// add_dir / dirs / empty

BOOST_AUTO_TEST_CASE(add_dir_appends) {
    ModulePathResolver r;
    BOOST_TEST(r.empty());
    r.add_dir(fs::path("/x"));
    r.add_dir(fs::path("/y"));
    BOOST_REQUIRE_EQUAL(r.dirs().size(), 2u);
    BOOST_TEST(r.dirs()[0] == fs::path("/x"));
    BOOST_TEST(r.dirs()[1] == fs::path("/y"));
    BOOST_TEST(!r.empty());
}

BOOST_AUTO_TEST_CASE(explicit_dirs_constructor) {
    ModulePathResolver r{{fs::path("/a"), fs::path("/b")}};
    BOOST_REQUIRE_EQUAL(r.dirs().size(), 2u);
}

// from_args_or_env

BOOST_AUTO_TEST_CASE(from_args_or_env_cli_path_overrides_env) {
    TempDir d;
    d.create_file("dummy.txt");
    auto r = ModulePathResolver::from_args_or_env(d.path.string());
    // First dir should come from cli
    BOOST_REQUIRE(!r.empty());
    BOOST_TEST(r.dirs()[0] == d.path);
}

BOOST_AUTO_TEST_CASE(from_args_or_env_empty_cli_uses_env) {
    TempDir d;
    d.create_file("dummy.txt");

#ifdef _WIN32
    _putenv_s("ETA_MODULE_PATH", d.path.string().c_str());
#else
    setenv("ETA_MODULE_PATH", d.path.string().c_str(), 1);
#endif

    auto r = ModulePathResolver::from_args_or_env("");
    BOOST_REQUIRE(!r.empty());
    BOOST_TEST(r.dirs()[0] == d.path);

#ifdef _WIN32
    _putenv_s("ETA_MODULE_PATH", "");
#else
    unsetenv("ETA_MODULE_PATH");
#endif
}

BOOST_AUTO_TEST_SUITE_END()

