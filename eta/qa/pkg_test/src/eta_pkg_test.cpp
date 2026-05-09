#define BOOST_TEST_MODULE eta.pkg.test
#include <boost/test/unit_test.hpp>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <unordered_map>
#include <vector>

#include "eta/package/lockfile.h"
#include "eta/package/discovery.h"
#include "eta/package/manifest.h"
#include "eta/package/resolver.h"

namespace fs = std::filesystem;

#ifndef ETA_PKG_TEST_FIXTURES_DIR
#error ETA_PKG_TEST_FIXTURES_DIR must be defined by CMake
#endif

namespace {

const fs::path kPkgTestFixturesDir{ETA_PKG_TEST_FIXTURES_DIR};

/**
 * @brief Temporary directory guard for package graph integration tests.
 */
struct TempDir {
    fs::path path;

    TempDir() {
        const auto suffix = std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count());
        path = fs::temp_directory_path() / ("eta_pkg_test_" + suffix);
        fs::create_directories(path);
    }

    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }

    fs::path write_file(const std::string& rel, const std::string& text) const {
        const auto full = path / rel;
        fs::create_directories(full.parent_path());
        std::ofstream out(full, std::ios::out | std::ios::binary | std::ios::trunc);
        out << text;
        return full;
    }
};

std::string make_manifest(const std::string& name,
                          const std::string& version,
                          const std::string& deps_table = "") {
    std::string manifest;
    manifest += "[package]\n";
    manifest += "name = \"" + name + "\"\n";
    manifest += "version = \"" + version + "\"\n";
    manifest += "license = \"MIT\"\n\n";
    manifest += "[compatibility]\n";
    manifest += "eta = \">=0.6, <0.8\"\n\n";
    manifest += "[dependencies]\n";
    manifest += deps_table;
    return manifest;
}

std::vector<std::string> package_names(const eta::package::ResolvedGraph& graph) {
    std::vector<std::string> names;
    names.reserve(graph.packages.size());
    for (const auto& pkg : graph.packages) names.push_back(pkg.name);
    return names;
}

fs::path fixture_path(const std::string& rel) {
    return kPkgTestFixturesDir / rel;
}

[[nodiscard]] std::string host_native_target_triple() {
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
#elif defined(__x86_64__)
    return "x86_64-apple-darwin";
#else
    return "unknown-apple-darwin";
#endif
#elif defined(__linux__)
#if defined(__x86_64__)
    return "x86_64-unknown-linux-gnu";
#elif defined(__aarch64__)
    return "aarch64-unknown-linux-gnu";
#else
    return "unknown-unknown-linux-gnu";
#endif
#else
    return "unknown-unknown-unknown";
#endif
}

[[nodiscard]] std::string alternate_native_target_triple() {
    if (host_native_target_triple() == "x86_64-pc-windows-msvc") {
        return "x86_64-unknown-linux-gnu";
    }
    return "x86_64-pc-windows-msvc";
}

[[nodiscard]] std::string host_native_artifact_relpath(std::string_view basename) {
#if defined(_WIN32)
    return "native/windows-x64/eta_" + std::string(basename) + ".dll";
#elif defined(__APPLE__)
#if defined(__aarch64__)
    return "native/macos-arm64/libeta_" + std::string(basename) + ".dylib";
#else
    return "native/macos-x64/libeta_" + std::string(basename) + ".dylib";
#endif
#elif defined(__linux__)
#if defined(__aarch64__)
    return "native/linux-arm64/libeta_" + std::string(basename) + ".so";
#else
    return "native/linux-x64/libeta_" + std::string(basename) + ".so";
#endif
#else
    return "native/unknown/libeta_" + std::string(basename) + ".bin";
#endif
}

fs::path copy_fixture_tree(const TempDir& temp, const fs::path& rel) {
    const auto source = fixture_path(rel.string());
    const auto destination = temp.path / "fixture";
    std::error_code ec;
    fs::copy(source, destination, fs::copy_options::recursive, ec);
    BOOST_REQUIRE_MESSAGE(!ec,
                          "failed to copy fixture tree '" + source.string() + "': " + ec.message());
    return destination;
}

} // namespace

BOOST_AUTO_TEST_SUITE(eta_pkg_test)

BOOST_AUTO_TEST_CASE(manifest_reports_missing_required_fields) {
    const auto parsed = eta::package::parse_manifest(R"toml(
[package]
name = "mathx"
license = "MIT"

[compatibility]
eta = ">=0.6, <0.8"
)toml");

    BOOST_REQUIRE(!parsed);
    BOOST_TEST(static_cast<int>(parsed.error().code)
               == static_cast<int>(eta::package::ManifestError::Code::MissingRequiredField));
    BOOST_TEST(parsed.error().message.find("[package].version") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(manifest_rejects_non_path_dependency_specs_in_s1) {
    const auto parsed = eta::package::parse_manifest(R"toml(
[package]
name = "app"
version = "0.1.0"
license = "MIT"

[compatibility]
eta = ">=0.6, <0.8"

[dependencies]
mathx = { git = "https://example.com/mathx.git", rev = "0123456789abcdef0123456789abcdef01234567" }
archive_dep = { tarball = "../archive.tar.gz", sha256 = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef" }

[dev-dependencies]
test_helpers = { path = "../test_helpers" }
)toml");

    BOOST_REQUIRE(parsed);
    BOOST_REQUIRE_EQUAL(parsed->dependencies.size(), 2u);
    BOOST_TEST(parsed->dependencies[0].name == "archive_dep");
    BOOST_TEST(static_cast<int>(parsed->dependencies[0].kind)
               == static_cast<int>(eta::package::ManifestDependencyKind::Tarball));
    BOOST_TEST(parsed->dependencies[1].name == "mathx");
    BOOST_TEST(static_cast<int>(parsed->dependencies[1].kind)
               == static_cast<int>(eta::package::ManifestDependencyKind::Git));
    BOOST_REQUIRE_EQUAL(parsed->dev_dependencies.size(), 1u);
    BOOST_TEST(parsed->dev_dependencies[0].name == "test_helpers");
    BOOST_TEST(static_cast<int>(parsed->dev_dependencies[0].kind)
               == static_cast<int>(eta::package::ManifestDependencyKind::Path));
}

BOOST_AUTO_TEST_CASE(manifest_parses_and_sorts_path_dependencies) {
    const auto parsed = eta::package::parse_manifest(R"toml(
[package]
name = "app"
version = "0.1.0"
license = "MIT OR Apache-2.0"

[compatibility]
eta = ">=0.6, <0.8"

[dependencies]
zeta = { path = "../zeta" }
alpha = { path = "../alpha" }
)toml");

    BOOST_REQUIRE(parsed);
    BOOST_TEST(parsed->name == "app");
    BOOST_TEST(parsed->version == "0.1.0");
    BOOST_REQUIRE_EQUAL(parsed->dependencies.size(), 2u);
    BOOST_TEST(parsed->dependencies[0].name == "alpha");
    BOOST_TEST(parsed->dependencies[1].name == "zeta");
}

BOOST_AUTO_TEST_CASE(manifest_document_parses_package_only_manifest) {
    const auto parsed = eta::package::parse_manifest_document(R"toml(
[package]
name = "app"
version = "0.1.0"
license = "MIT"

[compatibility]
eta = ">=0.6, <0.8"

[dependencies]
alpha = { path = "../alpha" }
)toml");

    BOOST_REQUIRE(parsed);
    BOOST_REQUIRE(parsed->package.has_value());
    BOOST_TEST(!parsed->workspace.has_value());
    BOOST_TEST(parsed->package->name == "app");
    BOOST_TEST(parsed->package->version == "0.1.0");
    BOOST_REQUIRE_EQUAL(parsed->package->dependencies.size(), 1u);
    BOOST_TEST(parsed->package->dependencies[0].name == "alpha");
}

BOOST_AUTO_TEST_CASE(manifest_document_parses_native_sidecar_metadata) {
    const auto parsed = eta::package::parse_manifest_document(R"toml(
[package]
name = "sidecar_pkg"
version = "0.1.0"
license = "MIT"

[compatibility]
eta = ">=0.6, <0.8"

[dependencies]

[native]
kind = "sidecar"
abi = "eta-native-v1"
id = "sidecar_pkg_native"
entry = "eta_register_extension_v1"

[[native.targets]]
triple = "x86_64-pc-windows-msvc"
artifact = "native/windows-x64/eta_sidecar_pkg_native.dll"
sha256 = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"

[[native.targets]]
triple = "aarch64-apple-darwin"
artifact = "native/macos-arm64/libeta_sidecar_pkg_native.dylib"
sha256 = "fedcba9876543210fedcba9876543210fedcba9876543210fedcba9876543210"
)toml");

    BOOST_REQUIRE(parsed);
    BOOST_REQUIRE(parsed->package.has_value());
    BOOST_REQUIRE(parsed->package->native.has_value());
    const auto& native = *parsed->package->native;
    BOOST_TEST(native.kind == "sidecar");
    BOOST_TEST(native.abi == "eta-native-v1");
    BOOST_TEST(native.id == "sidecar_pkg_native");
    BOOST_TEST(native.entry == "eta_register_extension_v1");
    BOOST_REQUIRE_EQUAL(native.targets.size(), 2u);
    BOOST_TEST(native.targets[0].triple == "aarch64-apple-darwin");
    BOOST_TEST(native.targets[1].triple == "x86_64-pc-windows-msvc");
}

BOOST_AUTO_TEST_CASE(manifest_document_rejects_native_without_package_section) {
    const auto parsed = eta::package::parse_manifest_document(R"toml(
[workspace]
members = ["packages/*"]

[native]
kind = "sidecar"
abi = "eta-native-v1"
id = "workspace_native"
entry = "eta_register_extension_v1"

[[native.targets]]
triple = "x86_64-pc-windows-msvc"
artifact = "native/windows-x64/eta_workspace_native.dll"
sha256 = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"
)toml");

    BOOST_REQUIRE(!parsed);
    BOOST_TEST(static_cast<int>(parsed.error().code)
               == static_cast<int>(eta::package::ManifestError::Code::InvalidValue));
    BOOST_TEST(parsed.error().message.find("[native] requires a [package] section")
               != std::string::npos);
}

BOOST_AUTO_TEST_CASE(manifest_document_rejects_native_targets_missing_required_fields) {
    const auto parsed = eta::package::parse_manifest_document(R"toml(
[package]
name = "broken_sidecar"
version = "0.1.0"
license = "MIT"

[compatibility]
eta = ">=0.6, <0.8"

[dependencies]

[native]
kind = "sidecar"
abi = "eta-native-v1"
id = "broken_sidecar_native"
entry = "eta_register_extension_v1"

[[native.targets]]
triple = "x86_64-pc-windows-msvc"
artifact = "native/windows-x64/eta_broken_sidecar_native.dll"
)toml");

    BOOST_REQUIRE(!parsed);
    BOOST_TEST(static_cast<int>(parsed.error().code)
               == static_cast<int>(eta::package::ManifestError::Code::MissingRequiredField));
    BOOST_TEST(parsed.error().message.find("sha256") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(manifest_document_rejects_duplicate_native_target_triples) {
    const auto parsed = eta::package::parse_manifest_document(R"toml(
[package]
name = "dup_sidecar"
version = "0.1.0"
license = "MIT"

[compatibility]
eta = ">=0.6, <0.8"

[dependencies]

[native]
kind = "sidecar"
abi = "eta-native-v1"
id = "dup_sidecar_native"
entry = "eta_register_extension_v1"

[[native.targets]]
triple = "x86_64-pc-windows-msvc"
artifact = "native/windows-x64/eta_dup_sidecar_native.dll"
sha256 = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"

[[native.targets]]
triple = "x86_64-pc-windows-msvc"
artifact = "native/windows-x64/eta_dup_sidecar_native_alt.dll"
sha256 = "fedcba9876543210fedcba9876543210fedcba9876543210fedcba9876543210"
)toml");

    BOOST_REQUIRE(!parsed);
    BOOST_TEST(static_cast<int>(parsed.error().code)
               == static_cast<int>(eta::package::ManifestError::Code::InvalidValue));
    BOOST_TEST(parsed.error().message.find("duplicate native target triple")
               != std::string::npos);
}

BOOST_AUTO_TEST_CASE(manifest_document_parses_workspace_only_fixture) {
    const auto parsed =
        eta::package::read_manifest_document(fixture_path("workspace/virtual_root/eta.toml"));

    BOOST_REQUIRE(parsed);
    BOOST_TEST(!parsed->package.has_value());
    BOOST_REQUIRE(parsed->workspace.has_value());
    BOOST_REQUIRE_EQUAL(parsed->workspace->members.size(), 1u);
    BOOST_TEST(parsed->workspace->members[0] == "packages/*");
    BOOST_REQUIRE_EQUAL(parsed->workspace->exclude.size(), 1u);
    BOOST_TEST(parsed->workspace->exclude[0] == "packages/experimental/*");
    BOOST_REQUIRE_EQUAL(parsed->workspace->default_members.size(), 1u);
    BOOST_TEST(parsed->workspace->default_members[0] == "packages/app");
}

BOOST_AUTO_TEST_CASE(manifest_document_parses_rooted_workspace_fixture) {
    const auto parsed =
        eta::package::read_manifest_document(fixture_path("workspace/rooted_root/eta.toml"));

    BOOST_REQUIRE(parsed);
    BOOST_REQUIRE(parsed->package.has_value());
    BOOST_REQUIRE(parsed->workspace.has_value());
    BOOST_TEST(parsed->package->name == "root_tools");
    BOOST_REQUIRE_EQUAL(parsed->workspace->members.size(), 1u);
    BOOST_TEST(parsed->workspace->members[0] == "packages/*");
}

BOOST_AUTO_TEST_CASE(manifest_document_requires_workspace_members_key) {
    const auto parsed = eta::package::parse_manifest_document(R"toml(
[workspace]
exclude = ["packages/experimental/*"]
)toml");

    BOOST_REQUIRE(!parsed);
    BOOST_TEST(static_cast<int>(parsed.error().code)
               == static_cast<int>(eta::package::ManifestError::Code::MissingRequiredField));
    BOOST_TEST(parsed.error().message.find("[workspace].members") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(manifest_document_rejects_empty_workspace_members) {
    const auto parsed = eta::package::parse_manifest_document(R"toml(
[workspace]
members = []
)toml");

    BOOST_REQUIRE(!parsed);
    BOOST_TEST(static_cast<int>(parsed.error().code)
               == static_cast<int>(eta::package::ManifestError::Code::InvalidValue));
    BOOST_TEST(parsed.error().message.find("[workspace].members") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(manifest_document_rejects_non_string_workspace_members) {
    const auto parsed = eta::package::parse_manifest_document(R"toml(
[workspace]
members = [1]
)toml");

    BOOST_REQUIRE(!parsed);
    BOOST_TEST(static_cast<int>(parsed.error().code)
               == static_cast<int>(eta::package::ManifestError::Code::ParseError));
    BOOST_TEST(parsed.error().message.find("[workspace].members") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(discover_manifest_context_classifies_standalone_package) {
    TempDir temp;
    const auto manifest_path = temp.write_file("app/eta.toml", make_manifest("app", "0.1.0"));
    fs::create_directories(temp.path / "app" / "src" / "nested");

    const auto discovered =
        eta::package::discover_manifest_context(temp.path / "app" / "src" / "nested");
    BOOST_REQUIRE(discovered);
    BOOST_REQUIRE(discovered->context.has_value());
    BOOST_TEST(static_cast<int>(*discovered->context)
               == static_cast<int>(eta::package::ManifestContextKind::StandalonePackage));
    BOOST_REQUIRE(discovered->active_manifest_path.has_value());
    BOOST_REQUIRE(discovered->package_manifest_path.has_value());
    BOOST_TEST(!discovered->workspace_manifest_path.has_value());

    std::error_code ec;
    BOOST_TEST(fs::equivalent(*discovered->active_manifest_path, manifest_path, ec));
    BOOST_TEST(!ec);
}

BOOST_AUTO_TEST_CASE(discover_manifest_context_classifies_virtual_workspace_root) {
    const auto workspace_manifest = fixture_path("workspace/virtual_root/eta.toml");
    const auto discovered =
        eta::package::discover_manifest_context(workspace_manifest.parent_path());
    BOOST_REQUIRE(discovered);
    BOOST_REQUIRE(discovered->context.has_value());
    BOOST_TEST(static_cast<int>(*discovered->context)
               == static_cast<int>(eta::package::ManifestContextKind::WorkspaceRoot));
    BOOST_REQUIRE(discovered->workspace_manifest_path.has_value());
    BOOST_REQUIRE(discovered->active_manifest_path.has_value());
    BOOST_TEST(!discovered->package_manifest_path.has_value());

    std::error_code ec;
    BOOST_TEST(fs::equivalent(*discovered->workspace_manifest_path, workspace_manifest, ec));
    BOOST_TEST(!ec);
    ec.clear();
    BOOST_TEST(fs::equivalent(*discovered->active_manifest_path, workspace_manifest, ec));
    BOOST_TEST(!ec);
}

BOOST_AUTO_TEST_CASE(discover_manifest_context_classifies_workspace_member) {
    const auto workspace_manifest = fixture_path("workspace/virtual_root/eta.toml");
    const auto member_manifest = fixture_path("workspace/virtual_root/packages/app/eta.toml");
    const auto discovered =
        eta::package::discover_manifest_context(fixture_path("workspace/virtual_root/packages/app/src"));
    BOOST_REQUIRE(discovered);
    BOOST_REQUIRE(discovered->context.has_value());
    BOOST_TEST(static_cast<int>(*discovered->context)
               == static_cast<int>(eta::package::ManifestContextKind::WorkspaceMember));
    BOOST_REQUIRE(discovered->workspace_manifest_path.has_value());
    BOOST_REQUIRE(discovered->package_manifest_path.has_value());
    BOOST_REQUIRE(discovered->active_manifest_path.has_value());

    std::error_code ec;
    BOOST_TEST(fs::equivalent(*discovered->workspace_manifest_path, workspace_manifest, ec));
    BOOST_TEST(!ec);
    ec.clear();
    BOOST_TEST(fs::equivalent(*discovered->package_manifest_path, member_manifest, ec));
    BOOST_TEST(!ec);
    ec.clear();
    BOOST_TEST(fs::equivalent(*discovered->active_manifest_path, member_manifest, ec));
    BOOST_TEST(!ec);
}

BOOST_AUTO_TEST_CASE(discover_manifest_context_classifies_workspace_non_member) {
    TempDir temp;
    const auto workspace_root = copy_fixture_tree(temp, fs::path("workspace") / "virtual_root");
    fs::create_directories(workspace_root / "notes" / "drafts");

    const auto workspace_manifest = workspace_root / "eta.toml";
    const auto discovered =
        eta::package::discover_manifest_context(workspace_root / "notes" / "drafts");
    BOOST_REQUIRE(discovered);
    BOOST_REQUIRE(discovered->context.has_value());
    BOOST_TEST(static_cast<int>(*discovered->context)
               == static_cast<int>(eta::package::ManifestContextKind::WorkspaceNonMember));
    BOOST_TEST(!discovered->package_manifest_path.has_value());
    BOOST_REQUIRE(discovered->workspace_manifest_path.has_value());
    BOOST_REQUIRE(discovered->active_manifest_path.has_value());

    std::error_code ec;
    BOOST_TEST(fs::equivalent(*discovered->workspace_manifest_path, workspace_manifest, ec));
    BOOST_TEST(!ec);
    ec.clear();
    BOOST_TEST(fs::equivalent(*discovered->active_manifest_path, workspace_manifest, ec));
    BOOST_TEST(!ec);
}

BOOST_AUTO_TEST_CASE(discover_manifest_context_returns_empty_when_no_manifest_exists) {
    TempDir temp;
    fs::create_directories(temp.path / "orphan" / "nested");
    const auto discovered = eta::package::discover_manifest_context(temp.path / "orphan" / "nested");

    BOOST_REQUIRE(discovered);
    BOOST_TEST(!discovered->context.has_value());
    BOOST_TEST(!discovered->active_manifest_path.has_value());
    BOOST_TEST(!discovered->package_manifest_path.has_value());
    BOOST_TEST(!discovered->workspace_manifest_path.has_value());
}

BOOST_AUTO_TEST_CASE(workspace_virtual_root_fixture_fails_package_parse) {
    const auto parsed =
        eta::package::read_manifest(fixture_path("workspace/virtual_root/eta.toml"));

    BOOST_REQUIRE(!parsed);
    BOOST_TEST(static_cast<int>(parsed.error().code)
               == static_cast<int>(eta::package::ManifestError::Code::MissingRequiredField));
    BOOST_TEST(parsed.error().message.find("[package].name") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(workspace_rooted_fixture_parses_as_package_manifest) {
    const auto parsed =
        eta::package::read_manifest(fixture_path("workspace/rooted_root/eta.toml"));

    BOOST_REQUIRE(parsed);
    BOOST_TEST(parsed->name == "root_tools");
    BOOST_TEST(parsed->version == "0.1.0");
    BOOST_TEST(parsed->license == "MIT");
    BOOST_TEST(parsed->compatibility_eta == ">=0.6, <0.8");
    BOOST_TEST(parsed->dependencies.empty());
    BOOST_TEST(parsed->dev_dependencies.empty());
}

BOOST_AUTO_TEST_CASE(standalone_native_sidecar_fixture_parses_as_package_manifest) {
    const auto parsed =
        eta::package::read_manifest(fixture_path("native_sidecar/standalone/eta.toml"));

    BOOST_REQUIRE(parsed);
    BOOST_TEST(parsed->name == "standalone_app");
    BOOST_TEST(parsed->version == "0.1.0");
    BOOST_TEST(parsed->license == "MIT");
    BOOST_TEST(parsed->compatibility_eta == ">=0.6, <0.8");
    BOOST_REQUIRE_EQUAL(parsed->dependencies.size(), 1u);
    BOOST_TEST(parsed->dependencies[0].name == "standalone_lib");
    BOOST_REQUIRE(parsed->native.has_value());
    BOOST_TEST(parsed->native->kind == "sidecar");
    BOOST_TEST(parsed->native->abi == "eta-native-v1");
    BOOST_TEST(parsed->native->id == "standalone_app_native");
    BOOST_TEST(parsed->native->entry == "eta_register_extension_v1");
    BOOST_REQUIRE(parsed->native->targets.size() >= 3u);
    const auto windows_target = std::find_if(
        parsed->native->targets.begin(),
        parsed->native->targets.end(),
        [](const eta::package::ManifestNativeTarget& target) {
            return target.triple == "x86_64-pc-windows-msvc";
        });
    BOOST_REQUIRE(windows_target != parsed->native->targets.end());
    BOOST_TEST(windows_target->artifact.generic_string()
               == "native/windows-x64/eta_standalone_app_native.dll");
}

BOOST_AUTO_TEST_CASE(discover_manifest_context_classifies_native_sidecar_workspace_member) {
    const auto workspace_manifest = fixture_path("native_sidecar/workspace/virtual_root/eta.toml");
    const auto member_manifest =
        fixture_path("native_sidecar/workspace/virtual_root/packages/app/eta.toml");
    const auto discovered = eta::package::discover_manifest_context(
        fixture_path("native_sidecar/workspace/virtual_root/packages/app"));
    BOOST_REQUIRE(discovered);
    BOOST_REQUIRE(discovered->context.has_value());
    BOOST_TEST(static_cast<int>(*discovered->context)
               == static_cast<int>(eta::package::ManifestContextKind::WorkspaceMember));
    BOOST_REQUIRE(discovered->workspace_manifest_path.has_value());
    BOOST_REQUIRE(discovered->package_manifest_path.has_value());
    BOOST_REQUIRE(discovered->active_manifest_path.has_value());

    std::error_code ec;
    BOOST_TEST(fs::equivalent(*discovered->workspace_manifest_path, workspace_manifest, ec));
    BOOST_TEST(!ec);
    ec.clear();
    BOOST_TEST(fs::equivalent(*discovered->package_manifest_path, member_manifest, ec));
    BOOST_TEST(!ec);
    ec.clear();
    BOOST_TEST(fs::equivalent(*discovered->active_manifest_path, member_manifest, ec));
    BOOST_TEST(!ec);
}

BOOST_AUTO_TEST_CASE(resolver_treats_workspace_member_fixture_as_regular_root_manifest) {
    const auto graph = eta::package::resolve_path_dependencies(
        fixture_path("workspace/virtual_root/packages/app/eta.toml"));
    if (!graph.has_value()) {
        BOOST_FAIL(graph.error().message);
    }
    BOOST_REQUIRE(graph.has_value());

    const std::vector<std::string> names = package_names(*graph);
    const std::vector<std::string> expected_names{"app", "lib"};
    BOOST_TEST(names == expected_names, boost::test_tools::per_element());
    BOOST_TEST(graph->root_name == "app");
}

BOOST_AUTO_TEST_CASE(workspace_members_include_implicit_root_for_rooted_workspace) {
    const auto workspace =
        eta::package::resolve_workspace_members(fixture_path("workspace/rooted_root/eta.toml"));

    BOOST_REQUIRE(workspace);
    BOOST_REQUIRE_EQUAL(workspace->members.size(), 2u);

    const auto root_it = std::find_if(
        workspace->members.begin(),
        workspace->members.end(),
        [](const eta::package::WorkspaceMember& member) {
            return member.name == "root_tools";
        });
    BOOST_REQUIRE(root_it != workspace->members.end());
    BOOST_TEST(root_it->source == "workspace+.");

    const auto helper_it = std::find_if(
        workspace->members.begin(),
        workspace->members.end(),
        [](const eta::package::WorkspaceMember& member) {
            return member.name == "helper";
        });
    BOOST_REQUIRE(helper_it != workspace->members.end());
    BOOST_TEST(helper_it->source == "workspace+packages/helper");
}

BOOST_AUTO_TEST_CASE(workspace_members_include_implicit_root_for_native_sidecar_rooted_workspace) {
    const auto workspace = eta::package::resolve_workspace_members(
        fixture_path("native_sidecar/workspace/rooted_root/eta.toml"));

    BOOST_REQUIRE(workspace);
    BOOST_REQUIRE_EQUAL(workspace->members.size(), 2u);

    const auto root_it = std::find_if(
        workspace->members.begin(),
        workspace->members.end(),
        [](const eta::package::WorkspaceMember& member) {
            return member.name == "root_tools";
        });
    BOOST_REQUIRE(root_it != workspace->members.end());
    BOOST_TEST(root_it->source == "workspace+.");

    const auto helper_it = std::find_if(
        workspace->members.begin(),
        workspace->members.end(),
        [](const eta::package::WorkspaceMember& member) {
            return member.name == "helper";
        });
    BOOST_REQUIRE(helper_it != workspace->members.end());
    BOOST_TEST(helper_it->source == "workspace+packages/helper");
}

BOOST_AUTO_TEST_CASE(workspace_members_reject_duplicate_package_names) {
    TempDir temp;
    temp.write_file("ws/eta.toml", R"toml(
[workspace]
members = ["packages/*"]
)toml");
    temp.write_file("ws/packages/one/eta.toml", make_manifest("dup", "0.1.0"));
    temp.write_file("ws/packages/two/eta.toml", make_manifest("dup", "0.2.0"));

    const auto workspace = eta::package::resolve_workspace_members(temp.path / "ws" / "eta.toml");
    BOOST_REQUIRE(!workspace);
    BOOST_TEST(static_cast<int>(workspace.error().code)
               == static_cast<int>(eta::package::ResolveError::Code::DuplicatePackageName));
}

BOOST_AUTO_TEST_CASE(workspace_resolver_unions_members_and_emits_workspace_sources) {
    const auto workspace =
        eta::package::resolve_workspace_members(fixture_path("workspace/virtual_root/eta.toml"));
    BOOST_REQUIRE(workspace);

    const auto graph = eta::package::resolve_workspace_dependencies(*workspace);
    if (!graph.has_value()) {
        BOOST_FAIL(graph.error().message);
    }
    BOOST_REQUIRE(graph.has_value());

    const std::vector<std::string> names = package_names(*graph);
    const std::vector<std::string> expected_names{"app", "lib"};
    BOOST_TEST(names == expected_names, boost::test_tools::per_element());

    const auto* app = graph->find("app");
    BOOST_REQUIRE(app != nullptr);
    BOOST_TEST(app->source == "workspace+packages/app");

    const auto* lib = graph->find("lib");
    BOOST_REQUIRE(lib != nullptr);
    BOOST_TEST(lib->source == "workspace+packages/lib");

    const auto lockfile = eta::package::build_lockfile(*graph);
    const auto rendered = eta::package::write_lockfile(lockfile);
    BOOST_TEST(rendered.find("source = \"workspace+packages/app\"") != std::string::npos);
    BOOST_TEST(rendered.find("source = \"workspace+packages/lib\"") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(lockfile_writer_is_deterministic) {
    eta::package::Lockfile lockfile;
    eta::package::LockfilePackage beta;
    beta.name = "beta";
    beta.version = "0.2.0";
    beta.source = "path+C:/beta";
    beta.dependencies = {{"gamma", "0.3.0"}, {"alpha", "0.1.0"}};

    eta::package::LockfilePackage alpha;
    alpha.name = "alpha";
    alpha.version = "0.1.0";
    alpha.source = "path+C:/alpha";

    eta::package::LockfilePackage app;
    app.name = "app";
    app.version = "1.0.0";
    app.source = "root";
    app.dependencies = {{"beta", "0.2.0"}, {"alpha", "0.1.0"}};

    lockfile.packages = {beta, alpha, app};

    const auto first = eta::package::write_lockfile(lockfile);
    const auto second = eta::package::write_lockfile(lockfile);
    BOOST_TEST(first == second);

    const auto parsed = eta::package::parse_lockfile(first);
    BOOST_REQUIRE(parsed);
    BOOST_REQUIRE_EQUAL(parsed->packages.size(), 3u);
    BOOST_TEST(parsed->packages[0].name == "app");
    BOOST_REQUIRE_EQUAL(parsed->packages[0].dependencies.size(), 2u);
    BOOST_TEST(parsed->packages[0].dependencies[0].name == "alpha");
    BOOST_TEST(parsed->packages[0].dependencies[1].name == "beta");
}

BOOST_AUTO_TEST_CASE(lockfile_round_trips_native_metadata_for_mixed_graphs) {
    eta::package::Lockfile lockfile;

    eta::package::LockfilePackage app;
    app.name = "app";
    app.version = "1.0.0";
    app.source = "root";
    app.dependencies = {{"native_dep", "0.2.0"}, {"plain_dep", "0.3.0"}};

    eta::package::LockfilePackage native_dep;
    native_dep.name = "native_dep";
    native_dep.version = "0.2.0";
    native_dep.source = "path+C:/deps/native_dep";
    native_dep.native_id = "native_dep";
    native_dep.native_abi = "eta-native-v1";
    native_dep.native_entry = "eta_register_extension_v1";
    native_dep.native_target_triple = "x86_64-pc-windows-msvc";
    native_dep.native_artifact_relpath = "native/windows-x64/eta_native_dep.dll";
    native_dep.native_sha256 =
        "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";

    eta::package::LockfilePackage plain_dep;
    plain_dep.name = "plain_dep";
    plain_dep.version = "0.3.0";
    plain_dep.source = "path+C:/deps/plain_dep";

    lockfile.packages = {native_dep, plain_dep, app};

    const auto rendered = eta::package::write_lockfile(lockfile);
    const auto reparsed = eta::package::parse_lockfile(rendered);
    BOOST_REQUIRE(reparsed);
    BOOST_REQUIRE_EQUAL(reparsed->packages.size(), 3u);
    BOOST_TEST(reparsed->packages[0].name == "app");

    const auto native_it = std::find_if(
        reparsed->packages.begin(),
        reparsed->packages.end(),
        [](const eta::package::LockfilePackage& pkg) {
            return pkg.name == "native_dep";
        });
    BOOST_REQUIRE(native_it != reparsed->packages.end());
    BOOST_REQUIRE(native_it->native_id.has_value());
    BOOST_REQUIRE(native_it->native_abi.has_value());
    BOOST_REQUIRE(native_it->native_entry.has_value());
    BOOST_REQUIRE(native_it->native_target_triple.has_value());
    BOOST_REQUIRE(native_it->native_artifact_relpath.has_value());
    BOOST_REQUIRE(native_it->native_sha256.has_value());
    BOOST_TEST(*native_it->native_id == "native_dep");
    BOOST_TEST(*native_it->native_abi == "eta-native-v1");
    BOOST_TEST(*native_it->native_entry == "eta_register_extension_v1");
    BOOST_TEST(*native_it->native_target_triple == "x86_64-pc-windows-msvc");
    BOOST_TEST(*native_it->native_artifact_relpath == "native/windows-x64/eta_native_dep.dll");
    BOOST_TEST(
        *native_it->native_sha256
        == "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef");
}

BOOST_AUTO_TEST_CASE(lockfile_rejects_incomplete_native_metadata) {
    const auto parsed = eta::package::parse_lockfile(R"toml(
version = 1

[[package]]
name = "native_dep"
version = "0.2.0"
source = "path+C:/deps/native_dep"
native_id = "native_dep"
native_abi = "eta-native-v1"
dependencies = []
)toml");

    BOOST_REQUIRE(!parsed);
    BOOST_TEST(static_cast<int>(parsed.error().code)
               == static_cast<int>(eta::package::LockfileError::Code::MissingRequiredField));
    BOOST_TEST(parsed.error().message.find("incomplete native metadata") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(resolver_selects_native_target_for_host_triple_when_building_lockfile) {
    TempDir temp;
    const std::string host_triple = host_native_target_triple();
    const std::string host_artifact = host_native_artifact_relpath("native_dep");
    const std::string host_sha =
        "802c6554f455da66ffe90a685b5aeb97b42c97a73d3f857ea3267a32f826e32a";

    temp.write_file("app/eta.toml",
                    make_manifest("app", "1.0.0", "native_dep = { path = \"../native_dep\" }\n"));
    temp.write_file("native_dep/eta.toml",
                    make_manifest("native_dep", "0.2.0")
                        + "\n[native]\n"
                          "kind = \"sidecar\"\n"
                          "abi = \"eta-native-v1\"\n"
                          "id = \"native_dep\"\n"
                          "entry = \"eta_register_extension_v1\"\n\n"
                          "[[native.targets]]\n"
                          "triple = \"" + host_triple + "\"\n"
                          "artifact = \"" + host_artifact + "\"\n"
                          "sha256 = \"" + host_sha + "\"\n\n"
                          "[[native.targets]]\n"
                          "triple = \"" + alternate_native_target_triple() + "\"\n"
                          "artifact = \"native/alt/libeta_native_dep_alt.bin\"\n"
                          "sha256 = \"0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef\"\n");

    const auto graph = eta::package::resolve_path_dependencies(temp.path / "app" / "eta.toml");
    if (!graph.has_value()) {
        BOOST_FAIL(graph.error().message);
    }

    const auto lockfile = eta::package::build_lockfile(*graph);
    const auto native_it = std::find_if(
        lockfile.packages.begin(),
        lockfile.packages.end(),
        [](const eta::package::LockfilePackage& pkg) {
            return pkg.name == "native_dep";
        });
    BOOST_REQUIRE(native_it != lockfile.packages.end());
    BOOST_REQUIRE(native_it->native_id.has_value());
    BOOST_REQUIRE(native_it->native_abi.has_value());
    BOOST_REQUIRE(native_it->native_entry.has_value());
    BOOST_REQUIRE(native_it->native_target_triple.has_value());
    BOOST_REQUIRE(native_it->native_artifact_relpath.has_value());
    BOOST_REQUIRE(native_it->native_sha256.has_value());
    BOOST_TEST(*native_it->native_id == "native_dep");
    BOOST_TEST(*native_it->native_abi == "eta-native-v1");
    BOOST_TEST(*native_it->native_entry == "eta_register_extension_v1");
    BOOST_TEST(*native_it->native_target_triple == host_triple);
    BOOST_TEST(*native_it->native_artifact_relpath == host_artifact);
    BOOST_TEST(*native_it->native_sha256 == host_sha);
}

BOOST_AUTO_TEST_CASE(resolver_reports_missing_native_target_for_host_triple) {
    TempDir temp;
    temp.write_file("app/eta.toml",
                    make_manifest("app", "1.0.0", "native_dep = { path = \"../native_dep\" }\n"));
    temp.write_file("native_dep/eta.toml",
                    make_manifest("native_dep", "0.2.0")
                        + "\n[native]\n"
                          "kind = \"sidecar\"\n"
                          "abi = \"eta-native-v1\"\n"
                          "id = \"native_dep\"\n"
                          "entry = \"eta_register_extension_v1\"\n\n"
                          "[[native.targets]]\n"
                          "triple = \"" + alternate_native_target_triple() + "\"\n"
                          "artifact = \"native/alt/libeta_native_dep_alt.bin\"\n"
                          "sha256 = \"0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef\"\n");

    const auto graph = eta::package::resolve_path_dependencies(temp.path / "app" / "eta.toml");
    BOOST_REQUIRE(!graph);
    BOOST_TEST(static_cast<int>(graph.error().code)
               == static_cast<int>(eta::package::ResolveError::Code::MissingNativeTargetForTriple));
}

BOOST_AUTO_TEST_CASE(path_dependency_graph_resolves_and_builds_lockfile) {
    TempDir temp;
    const auto root_manifest = temp.write_file(
        "app/eta.toml",
        make_manifest(
            "app",
            "1.0.0",
            "beta = { path = \"../beta\" }\n"
            "alpha = { path = \"../alpha\" }\n"));

    temp.write_file("alpha/eta.toml",
                    make_manifest("alpha", "0.1.0",
                                  "gamma = { path = \"../gamma\" }\n"));
    temp.write_file("beta/eta.toml", make_manifest("beta", "0.2.0"));
    temp.write_file("gamma/eta.toml", make_manifest("gamma", "0.3.0"));

    const auto graph = eta::package::resolve_path_dependencies(root_manifest);
    if (!graph.has_value()) {
        BOOST_FAIL(graph.error().message);
    }
    BOOST_REQUIRE(graph.has_value());

    const std::vector<std::string> names = package_names(*graph);
    const std::vector<std::string> expected_names{"app", "alpha", "beta", "gamma"};
    BOOST_TEST(names == expected_names, boost::test_tools::per_element());

    const auto* app = graph->find("app");
    BOOST_REQUIRE(app != nullptr);
    BOOST_REQUIRE_EQUAL(app->dependency_names.size(), 2u);
    BOOST_TEST(app->dependency_names[0] == "alpha");
    BOOST_TEST(app->dependency_names[1] == "beta");

    const auto lockfile = eta::package::build_lockfile(*graph);
    const auto rendered = eta::package::write_lockfile(lockfile);
    const auto parsed = eta::package::parse_lockfile(rendered);
    BOOST_REQUIRE(parsed);
    BOOST_REQUIRE_EQUAL(parsed->packages.size(), 4u);
    BOOST_TEST(parsed->packages[0].name == "app");
}

BOOST_AUTO_TEST_CASE(resolve_dependencies_uses_locator_for_non_path_sources) {
    TempDir temp;
    const auto root_manifest = temp.write_file(
        "app/eta.toml",
        make_manifest(
            "app",
            "1.0.0",
            "alpha = { git = \"https://example.com/alpha.git\", rev = \"0123456789abcdef0123456789abcdef01234567\" }\n"));

    temp.write_file("cache/alpha/eta.toml", make_manifest("alpha", "0.1.0"));

    eta::package::ResolveOptions options;
    options.dependency_locator =
        [&](const eta::package::Manifest& owner,
            const eta::package::ManifestDependency& dep)
            -> std::expected<eta::package::ResolvedDependencyLocation, eta::package::ResolveError> {
        (void)owner;
        eta::package::ResolvedDependencyLocation location;
        location.manifest_path = temp.path / "cache" / dep.name / "eta.toml";
        location.source = "git+https://example.com/alpha.git#0123456789abcdef0123456789abcdef01234567";
        return location;
    };

    const auto graph = eta::package::resolve_dependencies(root_manifest, options);
    if (!graph.has_value()) {
        BOOST_FAIL(graph.error().message);
    }
    BOOST_REQUIRE(graph.has_value());

    const auto* alpha = graph->find("alpha");
    BOOST_REQUIRE(alpha != nullptr);
    BOOST_TEST(alpha->source == "git+https://example.com/alpha.git#0123456789abcdef0123456789abcdef01234567");
}

BOOST_AUTO_TEST_CASE(resolver_reports_dependency_name_mismatch) {
    TempDir temp;
    const auto root_manifest = temp.write_file(
        "app/eta.toml",
        make_manifest("app", "1.0.0", "alpha = { path = \"../dep-one\" }\n"));

    temp.write_file("dep-one/eta.toml", make_manifest("dep_one", "0.1.0"));

    const auto graph = eta::package::resolve_path_dependencies(root_manifest);
    BOOST_REQUIRE(!graph);
    BOOST_TEST(static_cast<int>(graph.error().code)
               == static_cast<int>(eta::package::ResolveError::Code::DependencyNameMismatch));
}

BOOST_AUTO_TEST_SUITE_END()
