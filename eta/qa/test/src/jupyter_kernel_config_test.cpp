/**
 * @file jupyter_kernel_config_test.cpp
 * @brief Tests for eta_jupyter kernel.toml parsing and defaults.
 */

#include <boost/test/unit_test.hpp>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

#include "eta/jupyter/kernel_config.h"

namespace fs = std::filesystem;

namespace {

void set_env_var(const char* name, const char* value) {
#if defined(_WIN32)
    _putenv_s(name, value ? value : "");
#else
    if (value) setenv(name, value, 1);
    else unsetenv(name);
#endif
}

void clear_env_var(const char* name) {
#if defined(_WIN32)
    _putenv_s(name, "");
#else
    unsetenv(name);
#endif
}

} // namespace

BOOST_AUTO_TEST_SUITE(jupyter_kernel_config_tests)

BOOST_AUTO_TEST_CASE(load_kernel_config_defaults_when_explicit_path_missing) {
    set_env_var("ETA_KERNEL_CONFIG", "C:/definitely/missing/eta-kernel.toml");
    const auto loaded = eta::jupyter::load_kernel_config();
    clear_env_var("ETA_KERNEL_CONFIG");

    BOOST_TEST(!loaded.warnings.empty());
    BOOST_REQUIRE(!loaded.config.autoimport_modules.empty());
    BOOST_TEST(loaded.config.autoimport_modules.front() == "std.io");
    BOOST_TEST(loaded.config.display.table_max_rows == 1000u);
    BOOST_TEST(loaded.config.display.tensor_preview == 8u);
    BOOST_TEST(loaded.config.display.plot_theme == "light");
}

BOOST_AUTO_TEST_CASE(load_kernel_config_parses_valid_file) {
    const fs::path tmp_root = fs::temp_directory_path() / "eta-jupyter-kernel-config-test";
    fs::create_directories(tmp_root);
    const fs::path config_path = tmp_root / "kernel.toml";

    std::ofstream out(config_path, std::ios::out | std::ios::trunc);
    BOOST_REQUIRE_MESSAGE(out.good(), "failed to create temporary kernel.toml");
    out << "[autoimport]\n";
    out << "modules = [\"std.logic\", \"std.stats\"]\n\n";
    out << "[display]\n";
    out << "table_max_rows = 250\n";
    out << "tensor_preview = 5\n";
    out << "plot_theme = \"paper\"\n\n";
    out << "[interrupt]\n";
    out << "hard_kill_after_seconds = 42\n";
    out.close();

    set_env_var("ETA_KERNEL_CONFIG", config_path.string().c_str());
    const auto loaded = eta::jupyter::load_kernel_config();
    clear_env_var("ETA_KERNEL_CONFIG");

    std::error_code ec;
    fs::remove(config_path, ec);
    fs::remove(tmp_root, ec);

    BOOST_TEST(loaded.warnings.empty());
    BOOST_REQUIRE_EQUAL(loaded.config.autoimport_modules.size(), 3u);
    BOOST_TEST(loaded.config.autoimport_modules[0] == "std.io");
    BOOST_TEST(loaded.config.autoimport_modules[1] == "std.logic");
    BOOST_TEST(loaded.config.autoimport_modules[2] == "std.stats");
    BOOST_TEST(loaded.config.display.table_max_rows == 250u);
    BOOST_TEST(loaded.config.display.tensor_preview == 5u);
    BOOST_TEST(loaded.config.display.plot_theme == "paper");
    BOOST_TEST(loaded.config.hard_kill_after_seconds == 42);
}

BOOST_AUTO_TEST_SUITE_END()
