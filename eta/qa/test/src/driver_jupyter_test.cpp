/**
 * @file driver_jupyter_test.cpp
 * @brief Driver regression tests for notebook-facing evaluation APIs.
 */

#include <boost/test/unit_test.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "eta/session/eval_display.h"
#include "eta/session/driver.h"
#include "eta/interpreter/module_path.h"
#include "eta/native/sidecar_loader.h"

namespace fs = std::filesystem;

#ifndef ETA_STDLIB_DIR
#define ETA_STDLIB_DIR ""
#endif

#ifndef ETA_TEST_NATIVE_SIDECAR_PATH
#error ETA_TEST_NATIVE_SIDECAR_PATH must be defined by CMake
#endif

static fs::path stdlib_dir() {
    fs::path p(ETA_STDLIB_DIR);
    if (!p.empty() && fs::is_directory(p)) return p;

    const auto cwd = fs::current_path();
    for (const auto& candidate : {
        cwd / "stdlib",
        cwd / ".." / "stdlib",
        cwd / ".." / ".." / "stdlib",
        cwd / ".." / ".." / ".." / "stdlib",
    }) {
        if (fs::is_directory(candidate)) return fs::canonical(candidate);
    }
    return {};
}

static eta::interpreter::ModulePathResolver make_resolver() {
    auto stdlib = stdlib_dir();
    if (stdlib.empty()) return eta::interpreter::ModulePathResolver{};
    return eta::interpreter::ModulePathResolver({stdlib});
}

[[nodiscard]] fs::path sidecar_fixture_path() {
    return fs::path(ETA_TEST_NATIVE_SIDECAR_PATH);
}

[[nodiscard]] std::string host_target_triple() {
#if defined(_WIN32)
#if defined(_M_X64) || defined(__x86_64__)
    return "x86_64-pc-windows-msvc";
#elif defined(_M_ARM64) || defined(__aarch64__)
    return "aarch64-pc-windows-msvc";
#else
    return "unknown-pc-windows-msvc";
#endif
#elif defined(__APPLE__)
#if defined(__aarch64__)
    return "aarch64-apple-darwin";
#else
    return "x86_64-apple-darwin";
#endif
#elif defined(__linux__)
#if defined(__aarch64__)
    return "aarch64-unknown-linux-gnu";
#else
    return "x86_64-unknown-linux-gnu";
#endif
#else
    return "unknown-unknown-unknown";
#endif
}

struct CurrentPathGuard {
    fs::path original;

    CurrentPathGuard()
        : original(fs::current_path()) {}

    ~CurrentPathGuard() {
        std::error_code ec;
        fs::current_path(original, ec);
    }
};

struct ScopedTempDir {
    fs::path path;

    ScopedTempDir() {
        const auto stamp =
            std::chrono::steady_clock::now().time_since_epoch().count();
        path = fs::temp_directory_path() / ("eta_jupyter_s7_" + std::to_string(stamp));
        fs::create_directories(path);
    }

    ~ScopedTempDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};

struct ScopedEnvVar {
    std::string key;
    std::optional<std::string> previous;

    ScopedEnvVar(std::string key_in, std::string value)
        : key(std::move(key_in)) {
        if (const char* existing = std::getenv(key.c_str()); existing != nullptr) {
            previous = std::string(existing);
        }
        set(value);
    }

    ~ScopedEnvVar() {
        if (previous.has_value()) {
            set(*previous);
        } else {
            unset();
        }
    }

private:
    void set(const std::string& value) const {
#if defined(_WIN32)
        _putenv_s(key.c_str(), value.c_str());
#else
        setenv(key.c_str(), value.c_str(), 1);
#endif
    }

    void unset() const {
#if defined(_WIN32)
        _putenv_s(key.c_str(), "");
#else
        unsetenv(key.c_str());
#endif
    }
};

struct SidecarPackageFixture {
    fs::path app_root;
    fs::path sidecar_root;
    fs::path artifact_relpath;
};

[[nodiscard]] SidecarPackageFixture create_sidecar_package_fixture(
    const fs::path& root,
    const fs::path& sidecar_binary,
    const std::string& sidecar_sha,
    const std::string_view sidecar_package_name,
    const std::string_view extension_id,
    const std::string_view entrypoint) {
    SidecarPackageFixture fixture;
    fixture.app_root = root / "app";
    fixture.sidecar_root = root / std::string(sidecar_package_name);
    fixture.artifact_relpath = fs::path("native") / "test" / sidecar_binary.filename();

    fs::create_directories(fixture.app_root / "src");
    fs::create_directories((fixture.sidecar_root / fixture.artifact_relpath).parent_path());
    fs::copy_file(
        sidecar_binary,
        fixture.sidecar_root / fixture.artifact_relpath,
        fs::copy_options::overwrite_existing);

    {
        std::ofstream out(
            fixture.app_root / "eta.toml", std::ios::out | std::ios::binary | std::ios::trunc);
        BOOST_REQUIRE(out.is_open());
        out << "[package]\n"
            << "name = \"app\"\n"
            << "version = \"0.1.0\"\n"
            << "license = \"MIT\"\n\n"
            << "[compatibility]\n"
            << "eta = \">=0.6, <0.8\"\n\n"
            << "[dependencies]\n"
            << sidecar_package_name << " = { path = \"../" << sidecar_package_name << "\" }\n";
    }
    {
        std::ofstream out(
            fixture.sidecar_root / "eta.toml",
            std::ios::out | std::ios::binary | std::ios::trunc);
        BOOST_REQUIRE(out.is_open());
        out << "[package]\n"
            << "name = \"" << sidecar_package_name << "\"\n"
            << "version = \"0.1.0\"\n"
            << "license = \"MIT\"\n\n"
            << "[compatibility]\n"
            << "eta = \">=0.6, <0.8\"\n\n"
            << "[native]\n"
            << "kind = \"sidecar\"\n"
            << "abi = \"eta-native-v1\"\n"
            << "id = \"" << extension_id << "\"\n"
            << "entry = \"" << entrypoint << "\"\n\n"
            << "[[native.targets]]\n"
            << "triple = \"" << host_target_triple() << "\"\n"
            << "artifact = \"" << fixture.artifact_relpath.generic_string() << "\"\n"
            << "sha256 = \"" << sidecar_sha << "\"\n";
    }
    {
        std::ofstream out(
            fixture.app_root / "eta.lock", std::ios::out | std::ios::binary | std::ios::trunc);
        BOOST_REQUIRE(out.is_open());
        out << "version = 1\n\n"
            << "[[package]]\n"
            << "name = \"app\"\n"
            << "version = \"0.1.0\"\n"
            << "source = \"root\"\n"
            << "dependencies = [\"" << sidecar_package_name << "@0.1.0\"]\n\n"
            << "[[package]]\n"
            << "name = \"" << sidecar_package_name << "\"\n"
            << "version = \"0.1.0\"\n"
            << "source = \"path+../" << sidecar_package_name << "\"\n"
            << "native_id = \"" << extension_id << "\"\n"
            << "native_abi = \"eta-native-v1\"\n"
            << "native_entry = \"" << entrypoint << "\"\n"
            << "native_target_triple = \"" << host_target_triple() << "\"\n"
            << "native_artifact_relpath = \"" << fixture.artifact_relpath.generic_string() << "\"\n"
            << "native_sha256 = \"" << sidecar_sha << "\"\n"
            << "dependencies = []\n";
    }

    return fixture;
}

[[nodiscard]] eta::interpreter::ModulePathResolver make_stdlib_resolver(
    const fs::path& app_root) {
    auto resolver = eta::interpreter::ModulePathResolver::from_args_or_env_at("", app_root);
    const auto stdlib = stdlib_dir();
    if (!stdlib.empty()) {
        resolver.add_dir(stdlib);
    }
    return resolver;
}

BOOST_AUTO_TEST_SUITE(driver_jupyter_tests)

BOOST_AUTO_TEST_CASE(startup_resolver_discovers_project_modules_from_lockfile) {
    ScopedTempDir temp;
    const auto project_root = temp.path / "app";
    fs::create_directories(project_root / ".eta" / "modules" / "dep-0.1.0" / "src");
    fs::create_directories(project_root / "src");

    {
        std::ofstream out(project_root / "eta.toml", std::ios::out | std::ios::binary | std::ios::trunc);
        BOOST_REQUIRE(out.is_open());
        out << "[package]\n"
            << "name = \"app\"\n"
            << "version = \"1.0.0\"\n"
            << "license = \"MIT\"\n\n"
            << "[compatibility]\n"
            << "eta = \">=0.6, <0.8\"\n\n"
            << "[dependencies]\n"
            << "dep = { path = \"../dep\" }\n";
    }
    {
        std::ofstream out(project_root / "eta.lock", std::ios::out | std::ios::binary | std::ios::trunc);
        BOOST_REQUIRE(out.is_open());
        out << "version = 1\n\n"
            << "[[package]]\n"
            << "name = \"app\"\n"
            << "version = \"1.0.0\"\n"
            << "source = \"root\"\n"
            << "dependencies = [\"dep@0.1.0\"]\n\n"
            << "[[package]]\n"
            << "name = \"dep\"\n"
            << "version = \"0.1.0\"\n"
            << "source = \"path+../dep\"\n"
            << "dependencies = []\n";
    }
    {
        std::ofstream out(project_root / ".eta" / "modules" / "dep-0.1.0" / "src" / "dep.eta",
                          std::ios::out | std::ios::binary | std::ios::trunc);
        BOOST_REQUIRE(out.is_open());
        out << "(module dep\n"
            << "  (export dep-value)\n"
            << "  (begin (define dep-value 9)))\n";
    }

    CurrentPathGuard cwd_guard;
    fs::current_path(project_root);

    auto resolver = eta::interpreter::ModulePathResolver::from_args_or_env_at(
        "", project_root);
    eta::session::Driver driver(std::move(resolver));

    eta::runtime::nanbox::LispVal result{eta::runtime::nanbox::Nil};
    const bool ok = driver.run_source(R"eta(
(module app.test
  (import dep)
  (define result dep-value))
)eta", &result, "result");
    BOOST_REQUIRE(ok);

    auto decoded = eta::runtime::nanbox::ops::decode<int64_t>(result);
    BOOST_REQUIRE(decoded.has_value());
    BOOST_TEST(*decoded == 9);
}

BOOST_AUTO_TEST_CASE(
    workspace_member_loads_reachable_sidecars_without_loading_unrelated_workspace_members) {
    ScopedTempDir temp;
    const auto fixture = sidecar_fixture_path();
    BOOST_REQUIRE_MESSAGE(
        fs::is_regular_file(fixture), "missing sidecar fixture binary: " + fixture.string());

    const auto fixture_sha = eta::native::compute_sidecar_sha256(fixture);
    BOOST_REQUIRE_MESSAGE(
        fixture_sha.has_value(), "failed to hash sidecar fixture: " + fixture.string());

    const auto workspace_root = temp.path / "ws";
    const auto app_root = workspace_root / "packages" / "app";
    const auto lib_root = workspace_root / "packages" / "lib";
    const auto helper_root = workspace_root / "packages" / "helper";
    fs::create_directories(app_root / "src");
    fs::create_directories(lib_root / "src");
    fs::create_directories(helper_root / "src");

    const auto lib_native_relpath = fs::path("native") / "test" / fixture.filename();
    fs::create_directories((lib_root / lib_native_relpath).parent_path());
    fs::copy_file(
        fixture, lib_root / lib_native_relpath, fs::copy_options::overwrite_existing);

    {
        std::ofstream out(
            workspace_root / "eta.toml", std::ios::out | std::ios::binary | std::ios::trunc);
        BOOST_REQUIRE(out.is_open());
        out << "[workspace]\n"
            << "members = [\"packages/*\"]\n";
    }
    {
        std::ofstream out(
            app_root / "eta.toml", std::ios::out | std::ios::binary | std::ios::trunc);
        BOOST_REQUIRE(out.is_open());
        out << "[package]\n"
            << "name = \"app\"\n"
            << "version = \"0.1.0\"\n"
            << "license = \"MIT\"\n\n"
            << "[compatibility]\n"
            << "eta = \">=0.6, <0.8\"\n\n"
            << "[dependencies]\n"
            << "lib = { path = \"../lib\" }\n";
    }
    {
        std::ofstream out(
            lib_root / "eta.toml", std::ios::out | std::ios::binary | std::ios::trunc);
        BOOST_REQUIRE(out.is_open());
        out << "[package]\n"
            << "name = \"lib\"\n"
            << "version = \"0.1.0\"\n"
            << "license = \"MIT\"\n\n"
            << "[compatibility]\n"
            << "eta = \">=0.6, <0.8\"\n\n"
            << "[dependencies]\n";
    }
    {
        std::ofstream out(
            helper_root / "eta.toml", std::ios::out | std::ios::binary | std::ios::trunc);
        BOOST_REQUIRE(out.is_open());
        out << "[package]\n"
            << "name = \"helper\"\n"
            << "version = \"0.1.0\"\n"
            << "license = \"MIT\"\n\n"
            << "[compatibility]\n"
            << "eta = \">=0.6, <0.8\"\n\n"
            << "[dependencies]\n";
    }
    {
        std::ofstream out(
            app_root / "src" / "app.eta", std::ios::out | std::ios::binary | std::ios::trunc);
        BOOST_REQUIRE(out.is_open());
        out << "(module app\n"
            << "  (import lib)\n"
            << "  (begin (define app-value lib-value)))\n";
    }
    {
        std::ofstream out(
            lib_root / "src" / "lib.eta", std::ios::out | std::ios::binary | std::ios::trunc);
        BOOST_REQUIRE(out.is_open());
        out << "(module lib\n"
            << "  (export lib-value)\n"
            << "  (begin (define lib-value 9)))\n";
    }
    {
        std::ofstream out(
            helper_root / "src" / "helper.eta", std::ios::out | std::ios::binary | std::ios::trunc);
        BOOST_REQUIRE(out.is_open());
        out << "(module helper\n"
            << "  (export helper-value)\n"
            << "  (begin (define helper-value 17)))\n";
    }
    {
        std::ofstream out(
            workspace_root / "eta.lock", std::ios::out | std::ios::binary | std::ios::trunc);
        BOOST_REQUIRE(out.is_open());
        out << "version = 1\n\n"
            << "[[package]]\n"
            << "name = \"app\"\n"
            << "version = \"0.1.0\"\n"
            << "source = \"workspace+packages/app\"\n"
            << "dependencies = [\"lib@0.1.0\"]\n\n"
            << "[[package]]\n"
            << "name = \"helper\"\n"
            << "version = \"0.1.0\"\n"
            << "source = \"workspace+packages/helper\"\n"
            << "native_id = \"helper.native\"\n"
            << "native_abi = \"eta-native-v1\"\n"
            << "native_entry = \"eta_register_extension_v1\"\n"
            << "native_target_triple = \"x86_64-pc-windows-msvc\"\n"
            << "native_artifact_relpath = \"native/windows-x64/missing_helper.dll\"\n"
            << "native_sha256 = \"0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef\"\n"
            << "dependencies = []\n\n"
            << "[[package]]\n"
            << "name = \"lib\"\n"
            << "version = \"0.1.0\"\n"
            << "source = \"workspace+packages/lib\"\n"
            << "native_id = \"eta.test.sidecar\"\n"
            << "native_abi = \"eta-native-v1\"\n"
            << "native_entry = \"eta_register_extension_v1\"\n"
            << "native_target_triple = \"x86_64-pc-windows-msvc\"\n"
            << "native_artifact_relpath = \"" << lib_native_relpath.generic_string() << "\"\n"
            << "native_sha256 = \"" << *fixture_sha << "\"\n"
            << "dependencies = []\n";
    }

    CurrentPathGuard cwd_guard;
    fs::current_path(app_root);

    auto resolver = eta::interpreter::ModulePathResolver::from_args_or_env_at("", app_root);
    eta::session::Driver driver(std::move(resolver));

    eta::runtime::nanbox::LispVal result{eta::runtime::nanbox::Nil};
    const bool ok = driver.run_source(R"eta(
(module app.runtime
  (import lib)
  (begin
    (define result native.test.add)))
)eta", &result, "result");
    BOOST_REQUIRE(ok);
    BOOST_TEST(result != eta::runtime::nanbox::Nil);
    BOOST_TEST(driver.extension_primitive_count() == 2u);

    const auto completion = driver.completions_at("native.test.a", 13);
    bool found_native_add = false;
    for (const auto& match : completion.matches) {
        if (match == "native.test.add") {
            found_native_add = true;
            break;
        }
    }
    BOOST_TEST(found_native_add);
}

BOOST_AUTO_TEST_CASE(workspace_virtual_root_runs_core_only_without_selected_package_context) {
    ScopedTempDir temp;
    const auto workspace_root = temp.path / "ws";
    const auto app_root = workspace_root / "packages" / "app";
    fs::create_directories(app_root / "src");

    {
        std::ofstream out(
            workspace_root / "eta.toml", std::ios::out | std::ios::binary | std::ios::trunc);
        BOOST_REQUIRE(out.is_open());
        out << "[workspace]\n"
            << "members = [\"packages/*\"]\n";
    }
    {
        std::ofstream out(
            app_root / "eta.toml", std::ios::out | std::ios::binary | std::ios::trunc);
        BOOST_REQUIRE(out.is_open());
        out << "[package]\n"
            << "name = \"app\"\n"
            << "version = \"0.1.0\"\n"
            << "license = \"MIT\"\n\n"
            << "[compatibility]\n"
            << "eta = \">=0.6, <0.8\"\n\n"
            << "[dependencies]\n";
    }
    {
        std::ofstream out(
            app_root / "src" / "app.eta", std::ios::out | std::ios::binary | std::ios::trunc);
        BOOST_REQUIRE(out.is_open());
        out << "(module app\n"
            << "  (begin (define app-value 3)))\n";
    }
    {
        std::ofstream out(
            workspace_root / "eta.lock", std::ios::out | std::ios::binary | std::ios::trunc);
        BOOST_REQUIRE(out.is_open());
        out << "version = 1\n\n"
            << "[[package]]\n"
            << "name = \"app\"\n"
            << "version = \"0.1.0\"\n"
            << "source = \"workspace+packages/app\"\n"
            << "native_id = \"app.native\"\n"
            << "native_abi = \"eta-native-v1\"\n"
            << "native_entry = \"eta_register_extension_v1\"\n"
            << "native_target_triple = \"x86_64-pc-windows-msvc\"\n"
            << "native_artifact_relpath = \"native/windows-x64/missing_app.dll\"\n"
            << "native_sha256 = \"0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef\"\n"
            << "dependencies = []\n";
    }

    CurrentPathGuard cwd_guard;
    fs::current_path(workspace_root);

    auto resolver = eta::interpreter::ModulePathResolver::from_args_or_env_at("", workspace_root);
    eta::session::Driver driver(std::move(resolver));

    eta::runtime::nanbox::LispVal result{eta::runtime::nanbox::Nil};
    const bool ok = driver.run_source(R"eta(
(module repl.root
  (begin
    (define result 1)))
)eta", &result, "result");
    BOOST_REQUIRE(ok);

    const auto decoded = eta::runtime::nanbox::ops::decode<int64_t>(result);
    BOOST_REQUIRE(decoded.has_value());
    BOOST_TEST(*decoded == 1);
    BOOST_TEST(driver.extension_primitive_count() == 0u);
}

BOOST_AUTO_TEST_CASE(log_primitives_available_without_package_sidecar) {
    eta::session::Driver driver(make_resolver());

    const bool ok = driver.run_source(R"eta(
(module sidecar.log.requirement
  (import std.log)
  (begin
    (define result (log:default))))
)eta");
    BOOST_TEST(!ok);
    bool has_expected_diag = false;
    for (const auto& diag : driver.diagnostics().diagnostics()) {
        if (diag.message.find("requires package dependency 'eta-log-sidecar'") != std::string::npos) {
            has_expected_diag = true;
            break;
        }
    }
    BOOST_TEST(has_expected_diag);
}

BOOST_AUTO_TEST_CASE(native_builtin_fallback_env_var_is_ignored) {
    ScopedEnvVar fallback("ETA_NATIVE_BUILTIN_FALLBACK", "ON");
    eta::session::Driver driver(make_resolver());

    const bool ok = driver.run_source(R"eta(
(module sidecar.log.fallback.removed
  (import std.log)
  (begin
    (define result (log:default))))
)eta");
    BOOST_TEST(!ok);
    bool has_expected_diag = false;
    for (const auto& diag : driver.diagnostics().diagnostics()) {
        if (diag.message.find("requires package dependency 'eta-log-sidecar'") != std::string::npos) {
            has_expected_diag = true;
            break;
        }
    }
    BOOST_TEST(has_expected_diag);
}

BOOST_AUTO_TEST_CASE(log_sidecar_activates_std_log_wrappers) {
    ScopedTempDir temp;

    const auto fixture = sidecar_fixture_path();
    BOOST_REQUIRE_MESSAGE(
        fs::is_regular_file(fixture), "missing sidecar fixture binary: " + fixture.string());
    const auto fixture_sha = eta::native::compute_sidecar_sha256(fixture);
    BOOST_REQUIRE_MESSAGE(
        fixture_sha.has_value(), "failed to hash sidecar fixture: " + fixture.string());

    const auto sidecar_fixture = create_sidecar_package_fixture(
        temp.path,
        fixture,
        *fixture_sha,
        "eta-log-sidecar",
        "eta.log.sidecar",
        "eta_register_log_extension_v1");

    CurrentPathGuard cwd_guard;
    fs::current_path(sidecar_fixture.app_root);

    eta::session::Driver driver(make_stdlib_resolver(sidecar_fixture.app_root));

    eta::runtime::nanbox::LispVal result{eta::runtime::nanbox::Nil};
    const bool ok = driver.run_source(R"eta(
(module sidecar.log.runtime
  (import std.log)
  (begin
    (define sink (log:make-error-port-sink))
    (define logger (log:make-logger "sidecar.log.runtime" (list sink)))
    (log:set-pattern! logger "%v")
    (log:info logger "sidecar-log-message")
    (log:flush! logger)
    (define result 9)))
)eta", &result, "result");
    BOOST_REQUIRE(ok);

    const auto decoded = eta::runtime::nanbox::ops::decode<int64_t>(result);
    BOOST_REQUIRE(decoded.has_value());
    BOOST_TEST(*decoded == 9);
}

BOOST_AUTO_TEST_CASE(stats_primitives_available_without_package_sidecar) {
    eta::session::Driver driver(make_resolver());

    const bool ok = driver.run_source(R"eta(
(module sidecar.stats.requirement
  (import std.stats)
  (begin
    (define result (car (stats:mean-vec (list '(1 2 3)))))))
)eta");
    BOOST_TEST(!ok);
    bool has_expected_diag = false;
    for (const auto& diag : driver.diagnostics().diagnostics()) {
        if (diag.message.find("requires package dependency 'eta-stats-sidecar'") != std::string::npos) {
            has_expected_diag = true;
            break;
        }
    }
    BOOST_TEST(has_expected_diag);
}

BOOST_AUTO_TEST_CASE(stats_sidecar_activates_std_stats_wrappers) {
    ScopedTempDir temp;

    const auto fixture = sidecar_fixture_path();
    BOOST_REQUIRE_MESSAGE(
        fs::is_regular_file(fixture), "missing sidecar fixture binary: " + fixture.string());
    const auto fixture_sha = eta::native::compute_sidecar_sha256(fixture);
    BOOST_REQUIRE_MESSAGE(
        fixture_sha.has_value(), "failed to hash sidecar fixture: " + fixture.string());

    const auto sidecar_fixture = create_sidecar_package_fixture(
        temp.path,
        fixture,
        *fixture_sha,
        "eta-stats-sidecar",
        "eta.stats.sidecar",
        "eta_register_stats_extension_v1");

    CurrentPathGuard cwd_guard;
    fs::current_path(sidecar_fixture.app_root);

    eta::session::Driver driver(make_stdlib_resolver(sidecar_fixture.app_root));

    eta::runtime::nanbox::LispVal result{eta::runtime::nanbox::Nil};
    const bool ok = driver.run_source(R"eta(
(module sidecar.stats.runtime
  (import std.stats)
  (begin
    (define result (car (stats:mean-vec (list '(1 2 3)))))))
)eta", &result, "result");
    BOOST_REQUIRE(ok);

    const auto as_double = eta::runtime::nanbox::ops::decode<double>(result);
    if (as_double.has_value()) {
        BOOST_TEST(*as_double > 1.99);
        BOOST_TEST(*as_double < 2.01);
    } else {
        const auto as_int = eta::runtime::nanbox::ops::decode<int64_t>(result);
        BOOST_REQUIRE(as_int.has_value());
        BOOST_TEST(*as_int == 2);
    }
}

BOOST_AUTO_TEST_CASE(nng_primitives_available_without_package_sidecar) {
    eta::session::Driver driver(make_resolver());

    const bool ok = driver.run_source(R"eta(
(module sidecar.nng.requirement
  (import std.net)
  (begin
    (define result
      (with-socket 'pair
        (lambda (sock) (nng-socket? sock))))))
)eta");
    BOOST_TEST(!ok);
    bool has_expected_diag = false;
    for (const auto& diag : driver.diagnostics().diagnostics()) {
        if (diag.message.find("requires package dependency 'eta-nng-sidecar'") != std::string::npos) {
            has_expected_diag = true;
            break;
        }
    }
    BOOST_TEST(has_expected_diag);
}

BOOST_AUTO_TEST_CASE(nng_sidecar_activates_network_primitives) {
    ScopedTempDir temp;

    const auto fixture = sidecar_fixture_path();
    BOOST_REQUIRE_MESSAGE(
        fs::is_regular_file(fixture), "missing sidecar fixture binary: " + fixture.string());
    const auto fixture_sha = eta::native::compute_sidecar_sha256(fixture);
    BOOST_REQUIRE_MESSAGE(
        fixture_sha.has_value(), "failed to hash sidecar fixture: " + fixture.string());

    const auto sidecar_fixture = create_sidecar_package_fixture(
        temp.path,
        fixture,
        *fixture_sha,
        "eta-nng-sidecar",
        "eta.nng.sidecar",
        "eta_register_nng_extension_v1");

    CurrentPathGuard cwd_guard;
    fs::current_path(sidecar_fixture.app_root);

    eta::session::Driver driver(make_stdlib_resolver(sidecar_fixture.app_root));

    eta::runtime::nanbox::LispVal result{eta::runtime::nanbox::Nil};
    const bool ok = driver.run_source(R"eta(
(module sidecar.nng.runtime
  (import std.net)
  (begin
    (define result
      (with-socket 'pair
        (lambda (sock) (nng-socket? sock))))))
)eta", &result, "result");
    BOOST_REQUIRE(ok);
    BOOST_TEST(result == eta::runtime::nanbox::True);
}

BOOST_AUTO_TEST_CASE(torch_primitives_available_without_package_sidecar) {
    eta::session::Driver driver(make_resolver());

    const bool ok = driver.run_source(R"eta(
(module sidecar.torch.requirement
  (import std.torch)
  (begin
    (define result (numel (ones '(2 3))))))
)eta");
    BOOST_TEST(!ok);
    bool has_expected_diag = false;
    for (const auto& diag : driver.diagnostics().diagnostics()) {
        if (diag.message.find("requires package dependency 'eta-torch-sidecar'") != std::string::npos) {
            has_expected_diag = true;
            break;
        }
    }
    BOOST_TEST(has_expected_diag);
}

BOOST_AUTO_TEST_CASE(torch_sidecar_activates_std_torch_wrappers) {
#ifdef ETA_TORCH_DEBUG_SKIP
    BOOST_TEST_MESSAGE("ETA_TORCH_DEBUG_SKIP set - skipping torch sidecar activation test");
#else
    ScopedTempDir temp;

    const auto fixture = sidecar_fixture_path();
    BOOST_REQUIRE_MESSAGE(
        fs::is_regular_file(fixture), "missing sidecar fixture binary: " + fixture.string());
    const auto fixture_sha = eta::native::compute_sidecar_sha256(fixture);
    BOOST_REQUIRE_MESSAGE(
        fixture_sha.has_value(), "failed to hash sidecar fixture: " + fixture.string());

    const auto sidecar_fixture = create_sidecar_package_fixture(
        temp.path,
        fixture,
        *fixture_sha,
        "eta-torch-sidecar",
        "eta.torch.sidecar",
        "eta_register_torch_extension_v1");

    CurrentPathGuard cwd_guard;
    fs::current_path(sidecar_fixture.app_root);

    eta::session::Driver driver(make_stdlib_resolver(sidecar_fixture.app_root));

    eta::runtime::nanbox::LispVal result{eta::runtime::nanbox::Nil};
    const bool ok = driver.run_source(R"eta(
(module sidecar.torch.runtime
  (import std.torch)
  (begin
    (define result (numel (ones '(2 3))))))
)eta", &result, "result");
    BOOST_REQUIRE(ok);

    const auto decoded = eta::runtime::nanbox::ops::decode<int64_t>(result);
    BOOST_REQUIRE(decoded.has_value());
    BOOST_TEST(*decoded == 6);
#endif
}

BOOST_AUTO_TEST_CASE(is_complete_expression_unbalanced_paren_with_indent_hint) {
    eta::session::Driver driver(make_resolver());
    std::string indent;
    const bool complete = driver.is_complete_expression("(+ 1", &indent);
    BOOST_TEST(!complete);
    BOOST_TEST(indent == "  ");
}

BOOST_AUTO_TEST_CASE(is_complete_expression_balanced_input_returns_true) {
    eta::session::Driver driver(make_resolver());
    std::string indent;
    const bool complete = driver.is_complete_expression("(+ 1 2)", &indent);
    BOOST_TEST(complete);
    BOOST_TEST(indent.empty());
}

BOOST_AUTO_TEST_CASE(is_complete_expression_unterminated_string_returns_false) {
    eta::session::Driver driver(make_resolver());
    std::string indent;
    const bool complete = driver.is_complete_expression("\"unterminated", &indent);
    BOOST_TEST(!complete);
}

BOOST_AUTO_TEST_CASE(is_complete_expression_representative_inputs) {
    eta::session::Driver driver(make_resolver());

    struct Probe {
        std::string input;
        bool complete{false};
        std::string indent;
    };

    const std::vector<Probe> probes = {
        {"", true, ""},
        {"(+ 1", false, "  "},
        {"(+ 1 2)", true, ""},
        {"\"unterminated", false, ""},
        {"(display \"ok\")", true, ""},
        {"(+ 1\n  (+ 2 3)", false, "  "},
        {"#| block", false, ""},
        {"#| nested #| block |# still-open", false, ""},
        {"#| nested #| block |# done |#", true, ""},
        {"; comment only", true, ""},
        {"(+ 1 2)\n.continue", false, "  "},
        {"(+ 1 2)\n  ; trailing comment", true, ""},
    };
    BOOST_REQUIRE_EQUAL(probes.size(), 12u);

    for (const auto& probe : probes) {
        std::string indent;
        const bool complete = driver.is_complete_expression(probe.input, &indent);
        BOOST_CHECK_MESSAGE(complete == probe.complete, "input: " << probe.input);
        if (!probe.complete) {
            BOOST_CHECK_MESSAGE(indent == probe.indent, "input: " << probe.input);
        }
    }
}

BOOST_AUTO_TEST_CASE(completions_at_import_prefix_includes_std_torch) {
    eta::session::Driver driver(make_resolver());

    const std::string code = "(import std.to";
    const auto completion = driver.completions_at(code, code.size());

    BOOST_TEST(completion.cursor_start < completion.cursor_end);
    BOOST_TEST(completion.cursor_end == code.size());

    bool found_std_torch = false;
    for (const auto& m : completion.matches) {
        if (m == "std.torch") {
            found_std_torch = true;
            break;
        }
    }
    BOOST_TEST(found_std_torch);
}

BOOST_AUTO_TEST_CASE(extension_primitives_are_available_to_run_source_and_completions) {
    eta::session::Driver driver(make_resolver());
    driver.register_extension_primitive(
        "ext.answer",
        0u,
        false,
        [](eta::runtime::types::PrimitiveArgs) {
            return std::expected<eta::runtime::nanbox::LispVal, eta::runtime::error::RuntimeError>(
                eta::runtime::nanbox::ops::encode(int64_t{42}).value());
        });

    eta::runtime::nanbox::LispVal result{eta::runtime::nanbox::Nil};
    const bool ok = driver.run_source(R"eta(
(module ext.runtime
  (define result (ext.answer)))
)eta", &result, "result");
    BOOST_REQUIRE(ok);

    const auto decoded = eta::runtime::nanbox::ops::decode<int64_t>(result);
    BOOST_REQUIRE(decoded.has_value());
    BOOST_TEST(*decoded == 42);

    const uint32_t extension_slot = static_cast<uint32_t>(driver.builtin_count());
    const auto name_it = driver.global_names().find(extension_slot);
    BOOST_REQUIRE(name_it != driver.global_names().end());
    BOOST_TEST(name_it->second == "ext.answer");

    const auto completion = driver.completions_at("ext.ans", 7);
    bool found_extension = false;
    for (const auto& match : completion.matches) {
        if (match == "ext.answer") {
            found_extension = true;
            break;
        }
    }
    BOOST_TEST(found_extension);
}

BOOST_AUTO_TEST_CASE(hover_at_resolves_imported_binding_docs) {
    eta::session::Driver driver(make_resolver());
    const auto imported = driver.eval_to_display("(import std.test)");
    BOOST_REQUIRE(static_cast<int>(imported.tag) != static_cast<int>(eta::session::DisplayTag::Error));

    const auto markdown = driver.hover_at("make-test");
    BOOST_TEST(!markdown.empty());
    BOOST_TEST(markdown.find("make-test") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(hover_at_known_special_form_returns_markdown) {
    eta::session::Driver driver(make_resolver());
    const auto markdown = driver.hover_at("if");
    BOOST_TEST(!markdown.empty());
    BOOST_TEST(markdown.find("**if**") != std::string::npos);
    BOOST_TEST(markdown.find("(if test consequent alternate)") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(hover_at_known_builtin_returns_markdown) {
    eta::session::Driver driver(make_resolver());
    const auto markdown = driver.hover_at("map");
    BOOST_TEST(!markdown.empty());
    BOOST_TEST(markdown.find("**map**") != std::string::npos);
    BOOST_TEST(markdown.find("(map proc list ...)") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(hover_at_known_stdlib_binding_returns_markdown) {
    eta::session::Driver driver(make_resolver());
    const auto markdown = driver.hover_at("assert-equal");
    BOOST_TEST(!markdown.empty());
    BOOST_TEST(markdown.find("**assert-equal**") != std::string::npos);
    BOOST_TEST(markdown.find("(assert-equal expected actual . rest)") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(completions_at_uses_metadata_names) {
    eta::session::Driver driver(make_resolver());

    const auto special_form_completion = driver.completions_at("ca", 2);
    const auto builtin_completion = driver.completions_at("ma", 2);

    bool found_catch = false;
    for (const auto& match : special_form_completion.matches) {
        if (match == "catch") {
            found_catch = true;
            break;
        }
    }
    BOOST_TEST(found_catch);

    bool found_map = false;
    for (const auto& match : builtin_completion.matches) {
        if (match == "map") {
            found_map = true;
            break;
        }
    }
    BOOST_TEST(found_map);
}

BOOST_AUTO_TEST_CASE(request_interrupt_stops_runaway_evaluation_quickly) {
    using namespace std::chrono_literals;

    eta::session::Driver driver(make_resolver());

    static constexpr auto kRunawaySource = R"eta(
(module driver-jupyter-interrupt
  (begin
    (define (spin i)
      (if (< i 500000000)
          (spin (+ i 1))
          i))
    (spin 0)))
)eta";

    std::atomic<bool> finished{false};
    bool ok = true;
    std::thread worker([&]() {
        ok = driver.run_source(kRunawaySource);
        finished.store(true, std::memory_order_release);
    });

    std::this_thread::sleep_for(5ms);
    const auto interrupt_start = std::chrono::steady_clock::now();
    driver.request_interrupt();

    while (!finished.load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(1ms);
    }
    worker.join();

    const auto elapsed = std::chrono::steady_clock::now() - interrupt_start;
    BOOST_TEST(!ok);
    BOOST_TEST(std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count() <= 50);
}

BOOST_AUTO_TEST_CASE(eval_to_display_tags_tensor_values) {
    ScopedTempDir temp;

    const auto fixture = sidecar_fixture_path();
    BOOST_REQUIRE_MESSAGE(
        fs::is_regular_file(fixture), "missing sidecar fixture binary: " + fixture.string());
    const auto fixture_sha = eta::native::compute_sidecar_sha256(fixture);
    BOOST_REQUIRE_MESSAGE(
        fixture_sha.has_value(), "failed to hash sidecar fixture: " + fixture.string());

    const auto sidecar_fixture = create_sidecar_package_fixture(
        temp.path,
        fixture,
        *fixture_sha,
        "eta-torch-sidecar",
        "eta.torch.sidecar",
        "eta_register_torch_extension_v1");

    CurrentPathGuard cwd_guard;
    fs::current_path(sidecar_fixture.app_root);

    eta::session::Driver driver(make_stdlib_resolver(sidecar_fixture.app_root));

    const auto imported = driver.eval_to_display("(import std.torch)");
    BOOST_REQUIRE(static_cast<int>(imported.tag) != static_cast<int>(eta::session::DisplayTag::Error));

    auto display = driver.eval_to_display("(torch/zeros '(3 3))");
    BOOST_TEST(static_cast<int>(display.tag) == static_cast<int>(eta::session::DisplayTag::Tensor));
}

BOOST_AUTO_TEST_CASE(eval_to_display_tags_fact_table_values) {
    eta::session::Driver driver(make_resolver());

    const auto imported = driver.eval_to_display("(import std.fact_table)");
    BOOST_REQUIRE(static_cast<int>(imported.tag) != static_cast<int>(eta::session::DisplayTag::Error));

    auto display = driver.eval_to_display("(%make-fact-table '(x y))");
    BOOST_TEST(static_cast<int>(display.tag) == static_cast<int>(eta::session::DisplayTag::FactTable));
}

BOOST_AUTO_TEST_CASE(eval_to_display_tags_jupyter_wrapper_values) {
    eta::session::Driver driver(make_resolver());

    auto html = driver.eval_to_display("(vector 'jupyter-display \"text/html\" \"<b>ok</b>\")");
    BOOST_TEST(static_cast<int>(html.tag) == static_cast<int>(eta::session::DisplayTag::Html));

    auto vega = driver.eval_to_display(
        "(vector 'jupyter-display \"application/vnd.vegalite.v5+json\" \"{\\\"mark\\\":\\\"line\\\"}\")");
    BOOST_TEST(static_cast<int>(vega.tag) == static_cast<int>(eta::session::DisplayTag::VegaLite));
}

BOOST_AUTO_TEST_CASE(eval_to_display_import_std_jupyter_module) {
    eta::session::Driver driver(make_resolver());

    const auto imported = driver.eval_to_display("(import std.jupyter)");
    BOOST_TEST(static_cast<int>(imported.tag) != static_cast<int>(eta::session::DisplayTag::Error));
}

BOOST_AUTO_TEST_CASE(eval_to_display_persists_imports_between_calls) {
    eta::session::Driver driver(make_resolver());

    const auto first = driver.eval_to_display("(import (only std.aad grad))");
    BOOST_TEST(static_cast<int>(first.tag) != static_cast<int>(eta::session::DisplayTag::Error));

    const auto second = driver.eval_to_display(
        "(grad (lambda (x y) (+ (* x y) (sin x))) '(2 3))");
    BOOST_TEST(static_cast<int>(second.tag) != static_cast<int>(eta::session::DisplayTag::Error));
}

BOOST_AUTO_TEST_CASE(set_stream_sinks_routes_stdout_and_stderr) {
    eta::session::Driver driver(make_resolver());

    std::string stdout_text;
    std::string stderr_text;
    driver.set_stream_sinks(
        [&stdout_text](std::string_view chunk) { stdout_text.append(chunk); },
        [&stderr_text](std::string_view chunk) { stderr_text.append(chunk); });

    std::string out;
    const bool ok = driver.eval_string(
        "(begin "
        "  (display \"hello\")"
        "  (newline)"
        "  (display \"boom\" (current-error-port))"
        "  (newline (current-error-port))"
        "  42)",
        out);
    BOOST_TEST(ok);
    BOOST_TEST(stdout_text.find("hello") != std::string::npos);
    BOOST_TEST(stderr_text.find("boom") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(spawn_thread_routes_stdout_to_spawning_stream_sink) {
    using namespace std::chrono_literals;

    ScopedTempDir temp;

    const auto fixture = sidecar_fixture_path();
    BOOST_REQUIRE_MESSAGE(
        fs::is_regular_file(fixture), "missing sidecar fixture binary: " + fixture.string());
    const auto fixture_sha = eta::native::compute_sidecar_sha256(fixture);
    BOOST_REQUIRE_MESSAGE(
        fixture_sha.has_value(), "failed to hash sidecar fixture: " + fixture.string());

    const auto sidecar_fixture = create_sidecar_package_fixture(
        temp.path,
        fixture,
        *fixture_sha,
        "eta-nng-sidecar",
        "eta.nng.sidecar",
        "eta_register_nng_extension_v1");

    CurrentPathGuard cwd_guard;
    fs::current_path(sidecar_fixture.app_root);

    eta::session::Driver driver(make_stdlib_resolver(sidecar_fixture.app_root));

    std::mutex stdout_mu;
    std::string stdout_text;
    std::atomic<long long> first_marker_ms{-1};
    const auto start = std::chrono::steady_clock::now();

    driver.set_stream_sinks(
        [&](std::string_view chunk) {
            if (chunk.empty()) return;
            bool marker_seen = false;
            {
                std::lock_guard<std::mutex> lk(stdout_mu);
                stdout_text.append(chunk);
                marker_seen = stdout_text.find("routing-hi") != std::string::npos;
            }
            if (marker_seen) {
                const auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - start).count();
                long long expected = -1;
                (void)first_marker_ms.compare_exchange_strong(
                    expected, now_ms, std::memory_order_acq_rel);
            }
        },
        [](std::string_view) {});

    std::string out;
    const bool spawned = driver.eval_string(
        "(define routed-thread "
        "  (spawn-thread "
        "    (lambda () "
        "      (define (spin i) "
        "        (if (< i 1500000) "
        "            (spin (+ i 1)) "
        "            i)) "
        "      (spin 0) "
        "      (display \"routing-hi\") "
        "      (newline))))",
        out);
    if (!spawned) {
        std::ostringstream diagnostics;
        driver.diagnostics().print_all(
            diagnostics, /*use_color=*/false, driver.file_resolver());
        BOOST_FAIL("spawn-thread setup failed: " + diagnostics.str());
    }
    const auto eval_done_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start).count();

    const auto deadline = std::chrono::steady_clock::now() + 3s;
    while (first_marker_ms.load(std::memory_order_acquire) < 0 &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(5ms);
    }

    std::string join_out;
    BOOST_REQUIRE(driver.eval_string("(thread-join routed-thread)", join_out));

    const auto marker_ms = first_marker_ms.load(std::memory_order_acquire);
    BOOST_REQUIRE(marker_ms >= 0);
    BOOST_TEST(marker_ms >= eval_done_ms);
}

BOOST_AUTO_TEST_CASE(runtime_error_populates_user_error_diagnostic) {
    eta::session::Driver driver(make_resolver());

    std::string out;
    const bool ok = driver.eval_string("(error \"x\")", out);
    BOOST_TEST(!ok);

    const auto& diags = driver.diagnostics().diagnostics();
    BOOST_REQUIRE(!diags.empty());
    BOOST_TEST(static_cast<int>(diags.front().code) ==
               static_cast<int>(eta::diagnostic::DiagnosticCode::UserError));
    BOOST_TEST(diags.front().message.find("x") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(clear_module_cache_allows_module_reload) {
    namespace fs = std::filesystem;

    auto stdlib = stdlib_dir();
    BOOST_REQUIRE(!stdlib.empty());

    const auto tmp = fs::temp_directory_path() / "eta_driver_reload_test";
    std::error_code ec;
    fs::create_directories(tmp, ec);
    BOOST_REQUIRE(!ec);

    const auto module_path = tmp / "jupyter_reload_test.eta";
    {
        std::ofstream out(module_path, std::ios::out | std::ios::binary | std::ios::trunc);
        out << "(module jupyter_reload_test\n"
            << "  (export reload-value)\n"
            << "  (begin\n"
            << "    (define reload-value 1)))\n";
    }

    eta::interpreter::ModulePathResolver resolver({tmp, stdlib});
    eta::session::Driver driver(std::move(resolver));

    BOOST_REQUIRE(driver.run_file(module_path));
    BOOST_TEST(driver.has_module("jupyter_reload_test"));

    BOOST_TEST(driver.clear_module_cache("jupyter_reload_test"));
    BOOST_TEST(!driver.has_module("jupyter_reload_test"));

    BOOST_REQUIRE(driver.run_file(module_path));
    BOOST_TEST(driver.has_module("jupyter_reload_test"));

    fs::remove(module_path, ec);
}

BOOST_AUTO_TEST_SUITE_END()

