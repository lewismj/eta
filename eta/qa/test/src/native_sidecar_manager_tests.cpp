/**
 * @file native_sidecar_manager_tests.cpp
 * @brief Unit tests for native sidecar orchestration through NativeSidecarManager.
 */

#include <boost/test/unit_test.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "eta/native/sidecar_loader.h"
#include "eta/native/sidecar_manager.h"
#include "eta/runtime/builtin_metadata.h"
#include "eta/runtime/error.h"
#include "eta/runtime/nanbox.h"

namespace fs = std::filesystem;

#ifndef ETA_TEST_NATIVE_SIDECAR_PATH
#error ETA_TEST_NATIVE_SIDECAR_PATH must be defined by CMake
#endif

namespace {

/**
 * @brief Temporary directory guard used by sidecar manager integration tests.
 */
struct TempDir {
    fs::path path;

    TempDir() {
        const auto suffix = std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count());
        path = fs::temp_directory_path() / ("eta_native_sidecar_manager_test_" + suffix);
        fs::create_directories(path);
    }

    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};

[[nodiscard]] fs::path sidecar_fixture_path() {
    return fs::path(ETA_TEST_NATIVE_SIDECAR_PATH);
}

struct LockfileFixture {
    fs::path app_root;
    fs::path start_dir;
};

struct ModulePathSidecarFixture {
    fs::path packages_root;
    fs::path package_start_dir;
};

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

/**
 * @brief Build a minimal package + lockfile fixture with one reachable sidecar.
 *
 * The lockfile also contains an unrelated helper package with intentionally
 * broken native metadata so closure selection mistakes fail loudly.
 */
[[nodiscard]] LockfileFixture create_lockfile_fixture(
    const TempDir& temp,
    const fs::path& sidecar_binary,
    const std::string& sidecar_sha) {
    LockfileFixture fixture;
    fixture.app_root = temp.path / "app";
    fixture.start_dir = fixture.app_root / "src";
    fs::create_directories(fixture.start_dir);

    const fs::path helper_root = temp.path / "helper";
    fs::create_directories(helper_root / "src");

    {
        std::ofstream out(
            fixture.app_root / "eta.toml",
            std::ios::out | std::ios::binary | std::ios::trunc);
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
            fixture.app_root / "eta.lock",
            std::ios::out | std::ios::binary | std::ios::trunc);
        BOOST_REQUIRE(out.is_open());
        out << "version = 1\n\n"
            << "[[package]]\n"
            << "name = \"app\"\n"
            << "version = \"0.1.0\"\n"
            << "source = \"root\"\n"
            << "dependencies = [\"lib@0.1.0\"]\n\n"
            << "[[package]]\n"
            << "name = \"lib\"\n"
            << "version = \"0.1.0\"\n"
            << "source = \"path+" << sidecar_binary.parent_path().generic_string() << "\"\n"
            << "native_id = \"eta.test.sidecar\"\n"
            << "native_abi = \"eta-native-v1\"\n"
            << "native_entry = \"eta_register_extension_v1\"\n"
            << "native_target_triple = \"x86_64-pc-windows-msvc\"\n"
            << "native_artifact_relpath = \"" << sidecar_binary.filename().generic_string() << "\"\n"
            << "native_sha256 = \"" << sidecar_sha << "\"\n"
            << "dependencies = []\n\n"
            << "[[package]]\n"
            << "name = \"helper\"\n"
            << "version = \"0.1.0\"\n"
            << "source = \"path+" << helper_root.generic_string() << "\"\n"
            << "native_id = \"helper.native\"\n"
            << "native_abi = \"eta-native-v1\"\n"
            << "native_entry = \"eta_register_extension_v1\"\n"
            << "native_target_triple = \"x86_64-pc-windows-msvc\"\n"
            << "native_artifact_relpath = \"native/missing/helper.dll\"\n"
            << "native_sha256 = \"0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef\"\n"
            << "dependencies = []\n";
    }

    return fixture;
}

/**
 * @brief Build a lockfile fixture that triggers duplicate sidecar extension ids.
 *
 * Both dependency packages declare the same native extension id, so the second
 * loader pass must fail with a registry-conflict diagnostic.
 */
[[nodiscard]] LockfileFixture create_duplicate_sidecar_extension_fixture(
    const TempDir& temp,
    const fs::path& sidecar_binary,
    const std::string& sidecar_sha) {
    LockfileFixture fixture;
    fixture.app_root = temp.path / "app";
    fixture.start_dir = fixture.app_root / "src";
    fs::create_directories(fixture.start_dir);

    {
        std::ofstream out(
            fixture.app_root / "eta.toml",
            std::ios::out | std::ios::binary | std::ios::trunc);
        BOOST_REQUIRE(out.is_open());
        out << "[package]\n"
            << "name = \"app\"\n"
            << "version = \"0.1.0\"\n"
            << "license = \"MIT\"\n\n"
            << "[compatibility]\n"
            << "eta = \">=0.6, <0.8\"\n\n"
            << "[dependencies]\n"
            << "lib_a = { path = \"../lib_a\" }\n"
            << "lib_b = { path = \"../lib_b\" }\n";
    }

    {
        std::ofstream out(
            fixture.app_root / "eta.lock",
            std::ios::out | std::ios::binary | std::ios::trunc);
        BOOST_REQUIRE(out.is_open());
        out << "version = 1\n\n"
            << "[[package]]\n"
            << "name = \"app\"\n"
            << "version = \"0.1.0\"\n"
            << "source = \"root\"\n"
            << "dependencies = [\"lib_a@0.1.0\", \"lib_b@0.1.0\"]\n\n"
            << "[[package]]\n"
            << "name = \"lib_a\"\n"
            << "version = \"0.1.0\"\n"
            << "source = \"path+" << sidecar_binary.parent_path().generic_string() << "\"\n"
            << "native_id = \"eta.test.sidecar\"\n"
            << "native_abi = \"eta-native-v1\"\n"
            << "native_entry = \"eta_register_extension_v1\"\n"
            << "native_target_triple = \"x86_64-pc-windows-msvc\"\n"
            << "native_artifact_relpath = \"" << sidecar_binary.filename().generic_string() << "\"\n"
            << "native_sha256 = \"" << sidecar_sha << "\"\n"
            << "dependencies = []\n\n"
            << "[[package]]\n"
            << "name = \"lib_b\"\n"
            << "version = \"0.1.0\"\n"
            << "source = \"path+" << sidecar_binary.parent_path().generic_string() << "\"\n"
            << "native_id = \"eta.test.sidecar\"\n"
            << "native_abi = \"eta-native-v1\"\n"
            << "native_entry = \"eta_register_extension_v1\"\n"
            << "native_target_triple = \"x86_64-pc-windows-msvc\"\n"
            << "native_artifact_relpath = \"" << sidecar_binary.filename().generic_string() << "\"\n"
            << "native_sha256 = \"" << sidecar_sha << "\"\n"
            << "dependencies = []\n";
    }

    return fixture;
}

[[nodiscard]] ModulePathSidecarFixture create_module_path_sidecar_fixture(
    const TempDir& temp,
    const fs::path& sidecar_binary,
    const std::string& sidecar_sha) {
    ModulePathSidecarFixture fixture;
    fixture.packages_root = temp.path / "packages";

    const auto package_root = fixture.packages_root / "db" / "native" / "duckdb";
    fixture.package_start_dir = package_root / "src" / "db";
    fs::create_directories(fixture.package_start_dir);

    const auto native_dir = package_root / "native";
    fs::create_directories(native_dir);

    const auto staged_binary = native_dir / sidecar_binary.filename();
    std::error_code ec;
    fs::copy_file(sidecar_binary, staged_binary, fs::copy_options::overwrite_existing, ec);
    BOOST_REQUIRE_MESSAGE(
        !ec,
        "failed to copy sidecar fixture binary '" + sidecar_binary.string()
            + "' into module-path fixture: " + ec.message());

    {
        std::ofstream out(
            package_root / "eta.toml",
            std::ios::out | std::ios::binary | std::ios::trunc);
        BOOST_REQUIRE(out.is_open());
        out << "[package]\n"
            << "name = \"eta-duckdb\"\n"
            << "version = \"0.1.0\"\n"
            << "license = \"MIT\"\n\n"
            << "[compatibility]\n"
            << "eta = \">=0.6, <0.8\"\n\n"
            << "[native]\n"
            << "kind = \"sidecar\"\n"
            << "abi = \"eta-native-v1\"\n"
            << "id = \"eta.test.sidecar\"\n"
            << "entry = \"eta_register_extension_v1\"\n\n"
            << "[[native.targets]]\n"
            << "triple = \"" << host_target_triple() << "\"\n"
            << "artifact = \"native/" << sidecar_binary.filename().generic_string() << "\"\n"
            << "sha256 = \"" << sidecar_sha << "\"\n";
    }

    {
        std::ofstream out(
            fixture.package_start_dir / "duckdb.eta",
            std::ios::out | std::ios::binary | std::ios::trunc);
        BOOST_REQUIRE(out.is_open());
        out << "(module db.duckdb)\n";
    }

    return fixture;
}

[[nodiscard]] std::string join_lines(const std::vector<std::string>& lines) {
    std::ostringstream out;
    bool first = true;
    for (const auto& line : lines) {
        if (!first) out << '\n';
        first = false;
        out << line;
    }
    return out.str();
}

/**
 * @brief Test host recorder for NativeSidecarManager callbacks.
 */
class HostRecorder final : public eta::native::NativeSidecarManager::Host {
public:
    struct ExtensionRegistration {
        std::string name;
        uint32_t arity{0};
        bool has_rest{false};
        eta::runtime::types::PrimitiveFunc func;
    };

    bool allow_extension_registration{true};
    std::unordered_set<std::string> builtin_names;
    std::unordered_map<std::string, eta::runtime::types::PrimitiveFunc> builtin_funcs;
    std::vector<ExtensionRegistration> extension_registrations;
    std::vector<std::string> diagnostics;
    std::size_t invalidate_calls{0};

    void register_builtin_primitive(std::string name,
                                    uint32_t arity,
                                    bool has_rest,
                                    eta::runtime::types::PrimitiveFunc func) override {
        builtin_names.insert(name);
        builtin_funcs[std::move(name)] = std::move(func);
        (void)arity;
        (void)has_rest;
    }

    [[nodiscard]] bool has_builtin_primitive(std::string_view name) const override {
        return builtin_names.contains(std::string(name));
    }

    void overwrite_builtin_primitive(std::string_view name,
                                     eta::runtime::types::PrimitiveFunc func) override {
        builtin_names.insert(std::string(name));
        builtin_funcs[std::string(name)] = std::move(func);
    }

    void register_extension_primitive(std::string name,
                                      uint32_t arity,
                                      bool has_rest,
                                      eta::runtime::types::PrimitiveFunc func) override {
        extension_registrations.push_back(
            ExtensionRegistration{std::move(name), arity, has_rest, std::move(func)});
    }

    [[nodiscard]] bool can_register_extension_primitives() const noexcept override {
        return allow_extension_registration;
    }

    void invalidate_primitive_installer() override {
        ++invalidate_calls;
    }

    void emit_sidecar_error(std::string message) override {
        diagnostics.push_back(std::move(message));
    }
};

} // namespace

BOOST_AUTO_TEST_SUITE(native_sidecar_manager_tests)

BOOST_AUTO_TEST_CASE(builtin_sidecar_package_mapping_includes_nng_symbols) {
    const auto send_owner = eta::runtime::builtin_native_sidecar_package("send!");
    BOOST_REQUIRE(send_owner.has_value());
    BOOST_TEST(*send_owner == "eta-nng");

    const auto torch_owner = eta::runtime::builtin_native_sidecar_package("torch/tensor");
    BOOST_REQUIRE(torch_owner.has_value());
    BOOST_TEST(*torch_owner == "eta-torch");

    const auto add_owner = eta::runtime::builtin_native_sidecar_package("+");
    BOOST_TEST(!add_owner.has_value());
}

BOOST_AUTO_TEST_CASE(package_sidecar_loading_uses_active_lockfile_closure_only) {
    const auto fixture_path = sidecar_fixture_path();
    BOOST_REQUIRE_MESSAGE(
        fs::is_regular_file(fixture_path), "missing sidecar fixture binary: " + fixture_path.string());
    const auto fixture_sha = eta::native::compute_sidecar_sha256(fixture_path);
    BOOST_REQUIRE_MESSAGE(
        fixture_sha.has_value(), "failed to hash sidecar fixture: " + fixture_path.string());

    TempDir temp;
    const auto lockfile_fixture = create_lockfile_fixture(temp, fixture_path, *fixture_sha);

    HostRecorder host;
    eta::native::NativeSidecarManager manager(host);
    const std::vector<fs::path> module_dirs;

    const bool first_ok = manager.ensure_package_sidecars_loaded(
        lockfile_fixture.start_dir, module_dirs, "");
    BOOST_REQUIRE_MESSAGE(first_ok, join_lines(host.diagnostics));
    BOOST_TEST(host.extension_registrations.size() == 2u);
    BOOST_TEST(host.invalidate_calls == 1u);
    BOOST_TEST(host.diagnostics.empty());

    const bool second_ok = manager.ensure_package_sidecars_loaded(
        lockfile_fixture.start_dir, module_dirs, "");
    BOOST_REQUIRE_MESSAGE(second_ok, join_lines(host.diagnostics));
    BOOST_TEST(host.extension_registrations.size() == 2u);
    BOOST_TEST(host.invalidate_calls == 1u);
}

BOOST_AUTO_TEST_CASE(package_sidecar_loading_rejects_post_start_registration) {
    const auto fixture_path = sidecar_fixture_path();
    BOOST_REQUIRE_MESSAGE(
        fs::is_regular_file(fixture_path), "missing sidecar fixture binary: " + fixture_path.string());
    const auto fixture_sha = eta::native::compute_sidecar_sha256(fixture_path);
    BOOST_REQUIRE_MESSAGE(
        fixture_sha.has_value(), "failed to hash sidecar fixture: " + fixture_path.string());

    TempDir temp;
    const auto lockfile_fixture = create_lockfile_fixture(temp, fixture_path, *fixture_sha);

    HostRecorder host;
    host.allow_extension_registration = false;
    eta::native::NativeSidecarManager manager(host);
    const std::vector<fs::path> module_dirs;

    const bool ok = manager.ensure_package_sidecars_loaded(
        lockfile_fixture.start_dir, module_dirs, "");
    BOOST_TEST(!ok);
    BOOST_TEST(host.extension_registrations.empty());

    const auto diagnostics = join_lines(host.diagnostics);
    BOOST_TEST(
        diagnostics.find("native sidecar registration was requested after the session started executing modules")
        != std::string::npos);
}

BOOST_AUTO_TEST_CASE(module_path_sidecars_preload_supports_late_module_imports) {
    const auto fixture_path = sidecar_fixture_path();
    BOOST_REQUIRE_MESSAGE(
        fs::is_regular_file(fixture_path), "missing sidecar fixture binary: " + fixture_path.string());
    const auto fixture_sha = eta::native::compute_sidecar_sha256(fixture_path);
    BOOST_REQUIRE_MESSAGE(
        fixture_sha.has_value(), "failed to hash sidecar fixture: " + fixture_path.string());

    TempDir temp;
    const auto fixture =
        create_module_path_sidecar_fixture(temp, fixture_path, *fixture_sha);

    HostRecorder host;
    eta::native::NativeSidecarManager manager(host);
    const std::vector<fs::path> module_dirs{fixture.packages_root};

    const bool preload_ok = manager.ensure_package_sidecars_loaded(
        temp.path, module_dirs, "");
    BOOST_REQUIRE_MESSAGE(preload_ok, join_lines(host.diagnostics));
    BOOST_TEST(host.extension_registrations.size() == 2u);
    BOOST_TEST(host.invalidate_calls == 1u);
    BOOST_TEST(host.diagnostics.empty());

    host.allow_extension_registration = false;
    const bool followup_ok = manager.ensure_package_sidecars_loaded(
        fixture.package_start_dir, module_dirs, "");
    BOOST_REQUIRE_MESSAGE(followup_ok, join_lines(host.diagnostics));
    BOOST_TEST(host.extension_registrations.size() == 2u);
    BOOST_TEST(host.invalidate_calls == 1u);
    BOOST_TEST(host.diagnostics.empty());
}

BOOST_AUTO_TEST_CASE(package_sidecar_loading_reports_duplicate_extension_conflict) {
    const auto fixture_path = sidecar_fixture_path();
    BOOST_REQUIRE_MESSAGE(
        fs::is_regular_file(fixture_path), "missing sidecar fixture binary: " + fixture_path.string());
    const auto fixture_sha = eta::native::compute_sidecar_sha256(fixture_path);
    BOOST_REQUIRE_MESSAGE(
        fixture_sha.has_value(), "failed to hash sidecar fixture: " + fixture_path.string());

    TempDir temp;
    const auto lockfile_fixture = create_duplicate_sidecar_extension_fixture(
        temp, fixture_path, *fixture_sha);

    HostRecorder host;
    eta::native::NativeSidecarManager manager(host);
    const std::vector<fs::path> module_dirs;

    const bool ok = manager.ensure_package_sidecars_loaded(
        lockfile_fixture.start_dir, module_dirs, "");
    BOOST_TEST(!ok);
    BOOST_TEST(host.extension_registrations.empty());
    BOOST_TEST(host.invalidate_calls == 0u);

    const auto diagnostics = join_lines(host.diagnostics);
    BOOST_TEST(
        diagnostics.find("failed to load native sidecar for package 'lib_b'") != std::string::npos);
    BOOST_TEST(
        diagnostics.find("duplicate extension id 'eta.test.sidecar'") != std::string::npos);
}

BOOST_AUTO_TEST_SUITE_END()
