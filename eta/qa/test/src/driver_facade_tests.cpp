/**
 * @file driver_facade_tests.cpp
 * @brief Smoke tests for the public eta::session::Driver facade.
 */

#include <boost/test/unit_test.hpp>

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>

#include "eta/runtime/builtin_catalog.h"
#include "eta/session/driver.h"
#include "eta/session/runtime_config.h"

namespace fs = std::filesystem;

namespace {

/**
 * @brief Temporary directory guard used by facade tests.
 */
struct TempDir {
    fs::path path;

    TempDir() {
        const auto suffix = std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count());
        path = fs::temp_directory_path() / ("eta_driver_facade_test_" + suffix);
        fs::create_directories(path);
    }

    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }

    fs::path write_file(const std::string& rel, const std::string& contents) const {
        const auto full = path / rel;
        fs::create_directories(full.parent_path());
        std::ofstream out(full, std::ios::out | std::ios::binary | std::ios::trunc);
        out << contents;
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

} // namespace

BOOST_AUTO_TEST_SUITE(driver_facade_tests)

BOOST_AUTO_TEST_CASE(parse_heap_env_var_wrapper_matches_runtime_config_helper) {
    constexpr const char* kEnvName = "ETA_DRIVER_FACADE_HEAP_LIMIT";
    constexpr std::size_t kDefault = 1024u;
    EnvVarGuard guard(kEnvName);

    set_env_var(kEnvName, "16M");
    BOOST_TEST(
        eta::session::Driver::parse_heap_env_var(kEnvName, kDefault) ==
        eta::session::parse_heap_env_var(kEnvName, kDefault));
}

BOOST_AUTO_TEST_CASE(file_id_and_resolver_are_forwarded_through_driver) {
    TempDir temp;
    const auto file = temp.write_file("src/example.eta", "(module example)\n");

    eta::session::Driver driver(
        eta::interpreter::ModulePathResolver({temp.path}),
        8u * 1024u * 1024u);

    const auto file_id = driver.ensure_file_id(file);
    BOOST_REQUIRE(file_id != 0u);
    BOOST_TEST(driver.file_id_for_path(file.string()) == file_id);

    const auto* resolved = driver.path_for_file_id(file_id);
    BOOST_REQUIRE(resolved != nullptr);
    BOOST_TEST(resolved->filename() == fs::path("example.eta"));

    const auto resolver = driver.file_resolver();
    BOOST_TEST(resolver(file_id) == "example.eta");
}

BOOST_AUTO_TEST_CASE(run_source_smoke_evaluates_expression) {
    eta::session::Driver driver;

    std::string out;
    const bool ok = driver.eval_string("(+ 40 2)", out);
    BOOST_REQUIRE(ok);
    BOOST_TEST(out == "42");
}

BOOST_AUTO_TEST_CASE(driver_bootstrap_builtin_slots_match_catalog_order) {
    eta::session::Driver driver;
    std::string out;
    BOOST_REQUIRE(driver.eval_string("(+ 1 2)", out));

    const auto catalog = eta::runtime::builtin_catalog();
    BOOST_TEST(driver.builtin_count() == catalog.size());

    const auto& global_names = driver.global_names();
    for (std::size_t i = 0; i < catalog.size(); ++i) {
        const auto it = global_names.find(static_cast<std::uint32_t>(i));
        BOOST_TEST_CONTEXT("slot " << i << " builtin=" << catalog[i].name) {
            BOOST_REQUIRE(it != global_names.end());
            BOOST_TEST(it->second == catalog[i].name);
        }
    }
}

BOOST_AUTO_TEST_SUITE_END()
