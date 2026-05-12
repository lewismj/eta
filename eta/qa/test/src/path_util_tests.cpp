/**
 * @file path_util_tests.cpp
 * @brief Unit tests for shared path and runtime configuration helpers.
 */

#include <boost/test/unit_test.hpp>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>

#include "eta/session/runtime_config.h"
#include "eta/util/path.h"

namespace fs = std::filesystem;

namespace {

/**
 * @brief Temporary directory guard used by filesystem utility tests.
 */
struct TempDir {
    fs::path path;

    TempDir() {
        const auto suffix = std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count());
        path = fs::temp_directory_path() / ("eta_path_util_test_" + suffix);
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
        out << "ok";
        return full;
    }
};

/**
 * @brief Environment variable guard restoring original values on teardown.
 */
struct EnvVarGuard {
    std::string name;
    std::optional<std::string> original;

    explicit EnvVarGuard(std::string env_name)
        : name(std::move(env_name)) {
        if (const char* value = std::getenv(name.c_str()); value != nullptr) {
            original = value;
        }
    }

    ~EnvVarGuard() {
#ifdef _WIN32
        if (original.has_value()) {
            _putenv_s(name.c_str(), original->c_str());
        } else {
            _putenv_s(name.c_str(), "");
        }
#else
        if (original.has_value()) {
            setenv(name.c_str(), original->c_str(), 1);
        } else {
            unsetenv(name.c_str());
        }
#endif
    }
};

void set_env_var(const std::string& name, const std::string& value) {
#ifdef _WIN32
    _putenv_s(name.c_str(), value.c_str());
#else
    setenv(name.c_str(), value.c_str(), 1);
#endif
}

void unset_env_var(const std::string& name) {
#ifdef _WIN32
    _putenv_s(name.c_str(), "");
#else
    unsetenv(name.c_str());
#endif
}

} // namespace

BOOST_AUTO_TEST_SUITE(path_util_tests)

BOOST_AUTO_TEST_CASE(canonicalize_path_normalizes_existing_paths) {
    TempDir temp;
    const auto file_path = temp.write_file("src/module.eta");

    const auto canonical = eta::util::canonicalize_path(
        temp.path / "src" / ".." / "src" / "module.eta");
    BOOST_TEST(canonical == fs::weakly_canonical(file_path));
}

BOOST_AUTO_TEST_CASE(canonical_path_key_matches_equivalent_paths) {
    TempDir temp;
    const auto file_path = temp.write_file("nested/main.eta");

    const auto first_key = eta::util::canonical_path_key(
        temp.path / "nested" / "." / "main.eta");
    const auto second_key = eta::util::canonical_path_key(file_path);
    BOOST_TEST(first_key == second_key);
}

BOOST_AUTO_TEST_CASE(current_executable_path_and_sibling_path_are_resolved) {
    const auto self_path = eta::util::current_executable_path();
    BOOST_REQUIRE(self_path.has_value());

    std::error_code ec;
    BOOST_TEST(fs::is_regular_file(*self_path, ec));
    BOOST_TEST(!ec);

    const auto sibling = eta::util::sibling_executable_path("etai");
#ifdef _WIN32
    BOOST_TEST(sibling.filename() == fs::path("etai.exe"));
#else
    BOOST_TEST(sibling.filename() == fs::path("etai"));
#endif
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE(runtime_config_tests)

BOOST_AUTO_TEST_CASE(parse_heap_env_var_returns_default_when_unset_or_invalid) {
    constexpr const char* kEnvName = "ETA_TEST_HEAP_LIMIT";
    constexpr std::size_t kDefault = 123456u;
    EnvVarGuard guard(kEnvName);

    unset_env_var(kEnvName);
    BOOST_TEST(eta::session::parse_heap_env_var(kEnvName, kDefault) == kDefault);

    set_env_var(kEnvName, "not-a-number");
    BOOST_TEST(eta::session::parse_heap_env_var(kEnvName, kDefault) == kDefault);
}

BOOST_AUTO_TEST_CASE(parse_heap_env_var_parses_suffixes) {
    constexpr const char* kEnvName = "ETA_TEST_HEAP_LIMIT";
    constexpr std::size_t kDefault = 1u;
    EnvVarGuard guard(kEnvName);

    set_env_var(kEnvName, "512K");
    BOOST_TEST(eta::session::parse_heap_env_var(kEnvName, kDefault) == 512u * 1024u);

    set_env_var(kEnvName, "4M");
    BOOST_TEST(eta::session::parse_heap_env_var(kEnvName, kDefault) == 4u * 1024u * 1024u);

    set_env_var(kEnvName, "2G");
    BOOST_TEST(
        eta::session::parse_heap_env_var(kEnvName, kDefault) == 2ull * 1024ull * 1024ull * 1024ull);
}

BOOST_AUTO_TEST_SUITE_END()
