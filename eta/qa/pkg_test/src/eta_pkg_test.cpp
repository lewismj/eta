#define BOOST_TEST_MODULE eta.pkg.test
#include <boost/test/unit_test.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <unordered_map>
#include <vector>

#include "eta/package/lockfile.h"
#include "eta/package/manifest.h"
#include "eta/package/resolver.h"

namespace fs = std::filesystem;

namespace {

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

BOOST_AUTO_TEST_CASE(lockfile_writer_is_deterministic) {
    eta::package::Lockfile lockfile;
    lockfile.packages = {
        {"beta", "0.2.0", "path+C:/beta", {{"gamma", "0.3.0"}, {"alpha", "0.1.0"}}},
        {"alpha", "0.1.0", "path+C:/alpha", {}},
        {"app", "1.0.0", "root", {{"beta", "0.2.0"}, {"alpha", "0.1.0"}}},
    };

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
