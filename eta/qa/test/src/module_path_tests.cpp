/**
 * @file module_path_tests.cpp
 * @brief Unit tests for eta::interpreter::ModulePathResolver
 */

#include <boost/test/unit_test.hpp>
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <vector>

#include "eta/interpreter/module_path.h"

namespace fs = std::filesystem;

/// helpers

#ifdef _WIN32
static constexpr char kPathSep = ';';
#else
static constexpr char kPathSep = ':';
#endif

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

/// RAII guard that restores an environment variable to its previous value.
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

/// RAII guard that restores process current-working-directory.
struct CurrentPathGuard {
    fs::path original;

    CurrentPathGuard()
        : original(fs::current_path()) {}

    ~CurrentPathGuard() {
        std::error_code ec;
        fs::current_path(original, ec);
    }
};

/// test suite

BOOST_AUTO_TEST_SUITE(module_path_resolver_tests)

/// Bring the class name into scope within this test suite namespace.
using eta::interpreter::ModulePathResolver;

/// from_path_string

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

/// module_to_relative

BOOST_AUTO_TEST_CASE(module_to_relative_simple) {
    auto p = ModulePathResolver::module_to_relative("std.core");
    BOOST_TEST(p == fs::path("std/core.eta"));
}

BOOST_AUTO_TEST_CASE(module_to_relative_single_component) {
    auto p = ModulePathResolver::module_to_relative("core");
    BOOST_TEST(p == fs::path("core.eta"));
}

BOOST_AUTO_TEST_CASE(module_to_relative_deep) {
    auto p = ModulePathResolver::module_to_relative("a.b.c.d");
    BOOST_TEST(p == fs::path("a/b/c/d.eta"));
}

/// resolve

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
    /// Create a directory where the file should be (not a regular file)
    fs::create_directories(d.path / "std" / "core.eta");
    ModulePathResolver r{{d.path}};
    BOOST_TEST(!r.resolve("std.core").has_value());
}

BOOST_AUTO_TEST_CASE(resolve_prefers_etac_over_eta_within_same_root) {
    TempDir d;
    d.create_file("std/core.eta", "(module std.core)");
    d.create_file("std/core.etac", "ETAC");

    ModulePathResolver r{{d.path}};
    auto result = r.resolve("std.core");
    BOOST_REQUIRE(result.has_value());
    BOOST_TEST(result->extension() == ".etac");
}

BOOST_AUTO_TEST_CASE(resolve_prefers_eta_when_source_mode_enabled) {
    TempDir d;
    d.create_file("std/core.eta", "(module std.core)");
    d.create_file("std/core.etac", "ETAC");

    ModulePathResolver r{{d.path}};
    r.set_prefer_source(true);
    auto result = r.resolve("std.core");
    BOOST_REQUIRE(result.has_value());
    BOOST_TEST(result->extension() == ".eta");
}

BOOST_AUTO_TEST_CASE(resolve_keeps_first_hit_semantics_across_roots) {
    TempDir d1, d2;
    d1.create_file("std/core.eta", "(module std.core)");
    d2.create_file("std/core.etac", "ETAC");

    ModulePathResolver r{{d1.path, d2.path}};
    auto result = r.resolve("std.core");
    BOOST_REQUIRE(result.has_value());
    BOOST_TEST(result->string().find(d1.path.string()) != std::string::npos);
    BOOST_TEST(result->extension() == ".eta");
}

BOOST_AUTO_TEST_CASE(resolve_std_jupyter_finds_std_subdir_source) {
    TempDir d;
    d.create_file("std/jupyter.eta", "(module std.jupyter)");

    ModulePathResolver r{{d.path}};
    auto result = r.resolve("std.jupyter");
    BOOST_REQUIRE(result.has_value());
    BOOST_TEST(result->filename() == "jupyter.eta");
}

BOOST_AUTO_TEST_CASE(resolve_std_jupyter_prefers_std_subdir_etac) {
    TempDir d;
    d.create_file("std/jupyter.eta", "(module std.jupyter)");
    d.create_file("std/jupyter.etac", "ETAC");

    ModulePathResolver r{{d.path}};
    auto result = r.resolve("std.jupyter");
    BOOST_REQUIRE(result.has_value());
    BOOST_TEST(result->filename() == "jupyter.etac");
}

BOOST_AUTO_TEST_CASE(resolve_std_jupyter_prefers_source_when_source_mode_enabled) {
    TempDir d;
    d.create_file("std/jupyter.eta", "(module std.jupyter)");
    d.create_file("std/jupyter.etac", "ETAC");

    ModulePathResolver r{{d.path}};
    r.set_prefer_source(true);
    auto result = r.resolve("std.jupyter");
    BOOST_REQUIRE(result.has_value());
    BOOST_TEST(result->filename() == "jupyter.eta");
}

BOOST_AUTO_TEST_CASE(resolve_std_jupyter_does_not_use_legacy_root_files) {
    TempDir d;
    d.create_file("jupyter.eta", "(module std.jupyter)");
    d.create_file("jupyter.etac", "ETAC");

    ModulePathResolver r{{d.path}};
    BOOST_TEST(!r.resolve("std.jupyter").has_value());
}

/// find_file

BOOST_AUTO_TEST_CASE(find_file_locates_named_file) {
    TempDir d;
    d.create_file("core.eta", "(module core)");

    ModulePathResolver r{{d.path}};
    auto result = r.find_file("core.eta");
    BOOST_REQUIRE(result.has_value());
    BOOST_TEST(result->filename() == "core.eta");
}

BOOST_AUTO_TEST_CASE(find_file_returns_nullopt_when_absent) {
    TempDir d;
    ModulePathResolver r{{d.path}};
    BOOST_TEST(!r.find_file("missing.eta").has_value());
}

BOOST_AUTO_TEST_CASE(find_file_searches_dirs_in_order) {
    TempDir d1, d2;
    d2.create_file("core.eta", "(module core)");

    ModulePathResolver r{{d1.path, d2.path}};
    auto result = r.find_file("core.eta");
    BOOST_REQUIRE(result.has_value());
    BOOST_TEST(result->string().find(d2.path.string()) != std::string::npos);
}

/// add_dir / dirs / empty

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

/// from_args_or_env

BOOST_AUTO_TEST_CASE(from_args_or_env_cli_path_overrides_env) {
    TempDir cli_dir;
    TempDir env_dir;
    EnvVarGuard guard("ETA_MODULE_PATH");

#ifdef _WIN32
    _putenv_s("ETA_MODULE_PATH", env_dir.path.string().c_str());
#else
    setenv("ETA_MODULE_PATH", env_dir.path.string().c_str(), 1);
#endif

    auto r = ModulePathResolver::from_args_or_env(cli_dir.path.string());
    /// First dir should come from cli
    BOOST_REQUIRE(!r.empty());
    BOOST_TEST(r.dirs()[0] == cli_dir.path);
}

BOOST_AUTO_TEST_CASE(from_args_or_env_empty_cli_uses_env) {
    TempDir d;
    EnvVarGuard guard("ETA_MODULE_PATH");

#ifdef _WIN32
    _putenv_s("ETA_MODULE_PATH", d.path.string().c_str());
#else
    setenv("ETA_MODULE_PATH", d.path.string().c_str(), 1);
#endif

    auto r = ModulePathResolver::from_args_or_env("");
    BOOST_REQUIRE(!r.empty());
    BOOST_TEST(r.dirs()[0] == d.path);
}

BOOST_AUTO_TEST_CASE(from_args_or_env_pkg_entry_expands_release_and_src_roots) {
    TempDir d;
    const auto package_root = d.path / "demo";
    fs::create_directories(package_root / "target" / "release");
    fs::create_directories(package_root / "src");

    d.create_file("demo/eta.toml", R"toml(
[package]
name = "demo"
version = "0.1.0"
license = "MIT"

[compatibility]
eta = ">=0.6, <0.8"
)toml");

    EnvVarGuard guard("ETA_MODULE_PATH");
    const auto env_value = std::string("pkg+") + package_root.string();
#ifdef _WIN32
    _putenv_s("ETA_MODULE_PATH", env_value.c_str());
#else
    setenv("ETA_MODULE_PATH", env_value.c_str(), 1);
#endif

    auto r = ModulePathResolver::from_args_or_env_at("", d.path);
    BOOST_REQUIRE_GE(r.dirs().size(), 2u);
    BOOST_TEST(r.dirs()[0] == fs::weakly_canonical(package_root / "target" / "release"));
    BOOST_TEST(r.dirs()[1] == fs::weakly_canonical(package_root / "src"));
}

BOOST_AUTO_TEST_CASE(from_args_or_env_auto_entry_expands_top_level_package_collection) {
    TempDir d;
    fs::create_directories(d.path / "packages" / "db" / "native" / "duckdb" / "src" / "db");

    d.create_file("packages/db/native/duckdb/eta.toml", R"toml(
[package]
name = "duckdb"
version = "0.1.0"
license = "MIT"

[compatibility]
eta = ">=0.6, <0.8"
)toml");
    const auto duckdb_module =
        d.create_file("packages/db/native/duckdb/src/db/duckdb.eta", "(module db.duckdb)\n");

    EnvVarGuard guard("ETA_MODULE_PATH");
    const auto env_value = (d.path / "packages").string();
#ifdef _WIN32
    _putenv_s("ETA_MODULE_PATH", env_value.c_str());
#else
    setenv("ETA_MODULE_PATH", env_value.c_str(), 1);
#endif

    auto r = ModulePathResolver::from_args_or_env_at("", d.path);
    auto resolved = r.resolve("db.duckdb");
    BOOST_REQUIRE(resolved.has_value());
    BOOST_TEST(*resolved == fs::weakly_canonical(duckdb_module));
}

BOOST_AUTO_TEST_CASE(from_args_or_env_pkg_entries_keep_precedence_for_duplicate_modules) {
    TempDir d;
    fs::create_directories(d.path / "first" / "src" / "dup");
    fs::create_directories(d.path / "second" / "src" / "dup");

    d.create_file("first/eta.toml", R"toml(
[package]
name = "dup-first"
version = "0.1.0"
license = "MIT"

[compatibility]
eta = ">=0.6, <0.8"
)toml");
    d.create_file("second/eta.toml", R"toml(
[package]
name = "dup-second"
version = "0.1.0"
license = "MIT"

[compatibility]
eta = ">=0.6, <0.8"
)toml");
    const auto first_module =
        d.create_file("first/src/dup/shared.eta", "(module dup.shared (begin 1))\n");
    const auto second_module =
        d.create_file("second/src/dup/shared.eta", "(module dup.shared (begin 2))\n");

    EnvVarGuard guard("ETA_MODULE_PATH");
    const auto env_value = std::string("pkg+") + (d.path / "first").string()
                         + kPathSep
                         + std::string("pkg+") + (d.path / "second").string();
#ifdef _WIN32
    _putenv_s("ETA_MODULE_PATH", env_value.c_str());
#else
    setenv("ETA_MODULE_PATH", env_value.c_str(), 1);
#endif

    auto r = ModulePathResolver::from_args_or_env_at("", d.path);
    auto matches = r.resolve_all("dup.shared");
    BOOST_REQUIRE_EQUAL(matches.size(), 2u);
    BOOST_TEST(matches[0] == fs::weakly_canonical(first_module));
    BOOST_TEST(matches[1] == fs::weakly_canonical(second_module));

    auto first = r.resolve("dup.shared");
    BOOST_REQUIRE(first.has_value());
    BOOST_TEST(*first == matches.front());
}

BOOST_AUTO_TEST_CASE(from_args_or_env_pkg_entries_dedupe_normalized_path_variants) {
    TempDir d;
    fs::create_directories(d.path / "norm" / "src" / "dup");
    d.create_file("norm/eta.toml", R"toml(
[package]
name = "norm"
version = "0.1.0"
license = "MIT"

[compatibility]
eta = ">=0.6, <0.8"
)toml");
    const auto module_file =
        d.create_file("norm/src/dup/norm.eta", "(module dup.norm (begin 1))\n");

    std::string normalized_variant;
#ifdef _WIN32
    normalized_variant = (d.path / "norm").string();
    std::replace(normalized_variant.begin(), normalized_variant.end(), '\\', '/');
#else
    normalized_variant = ((d.path / "norm") / ".").string();
#endif

    EnvVarGuard guard("ETA_MODULE_PATH");
    const auto env_value = std::string("pkg+") + (d.path / "norm").string()
                         + kPathSep
                         + std::string("pkg+") + normalized_variant;
#ifdef _WIN32
    _putenv_s("ETA_MODULE_PATH", env_value.c_str());
#else
    setenv("ETA_MODULE_PATH", env_value.c_str(), 1);
#endif

    auto r = ModulePathResolver::from_args_or_env_at("", d.path);
    auto matches = r.resolve_all("dup.norm");
    BOOST_REQUIRE_EQUAL(matches.size(), 1u);
    BOOST_TEST(matches[0] == fs::weakly_canonical(module_file));
}

BOOST_AUTO_TEST_CASE(from_args_or_env_auto_entry_falls_back_to_literal_directory_root) {
    TempDir d;
    fs::create_directories(d.path / "legacy-modules" / "legacy");
    const auto module_file =
        d.create_file("legacy-modules/legacy/core.eta", "(module legacy.core)\n");

    EnvVarGuard guard("ETA_MODULE_PATH");
    const auto env_value = (d.path / "legacy-modules").string();
#ifdef _WIN32
    _putenv_s("ETA_MODULE_PATH", env_value.c_str());
#else
    setenv("ETA_MODULE_PATH", env_value.c_str(), 1);
#endif

    auto r = ModulePathResolver::from_args_or_env_at("", d.path);
    auto resolved = r.resolve("legacy.core");
    BOOST_REQUIRE(resolved.has_value());
    BOOST_TEST(*resolved == fs::weakly_canonical(module_file));
}

BOOST_AUTO_TEST_CASE(from_args_or_env_includes_compile_time_stdlib_fallback) {
    EnvVarGuard guard("ETA_MODULE_PATH");

#ifdef _WIN32
    _putenv_s("ETA_MODULE_PATH", "");
#else
    unsetenv("ETA_MODULE_PATH");
#endif

    auto r = ModulePathResolver::from_args_or_env("");

#ifdef ETA_STDLIB_DIR
    fs::path expected = fs::path(ETA_STDLIB_DIR);
    if (!expected.empty()) {
        const auto& dirs = r.dirs();
        auto it = std::find(dirs.begin(), dirs.end(), expected);
        const bool found = (it != dirs.end());
        BOOST_TEST(found);
    }
#endif
}

BOOST_AUTO_TEST_CASE(from_args_or_env_discovers_project_src_and_modules_from_lockfile) {
    TempDir d;
    const auto project_root = d.path / "app";
    fs::create_directories(project_root / "nested" / "path");
    fs::create_directories(project_root / "src");
    fs::create_directories(project_root / ".eta" / "modules" / "beta-0.2.0" / "target" / "release");
    fs::create_directories(project_root / ".eta" / "modules" / "beta-0.2.0" / "src");
    fs::create_directories(project_root / ".eta" / "modules" / "alpha-0.1.0" / "target" / "release");
    fs::create_directories(project_root / ".eta" / "modules" / "alpha-0.1.0" / "src");

    d.create_file("app/eta.toml", R"toml(
[package]
name = "app"
version = "1.0.0"
license = "MIT"

[compatibility]
eta = ">=0.6, <0.8"

[dependencies]
)toml");

    d.create_file("app/eta.lock", R"toml(
version = 1

[[package]]
name = "app"
version = "1.0.0"
source = "root"
dependencies = ["beta@0.2.0", "alpha@0.1.0"]

[[package]]
name = "beta"
version = "0.2.0"
source = "path+../beta"
dependencies = []

[[package]]
name = "alpha"
version = "0.1.0"
source = "path+../alpha"
dependencies = []
)toml");

    CurrentPathGuard cwd_guard;
    fs::current_path(project_root / "nested" / "path");

    auto r = ModulePathResolver::from_args_or_env("");
    BOOST_REQUIRE_GE(r.dirs().size(), 5u);

    const auto expected_root_src = fs::weakly_canonical(project_root / "src");
    const auto expected_beta_release =
        fs::weakly_canonical(project_root / ".eta" / "modules" / "beta-0.2.0" / "target" / "release");
    const auto expected_beta_src =
        fs::weakly_canonical(project_root / ".eta" / "modules" / "beta-0.2.0" / "src");
    const auto expected_alpha_release =
        fs::weakly_canonical(project_root / ".eta" / "modules" / "alpha-0.1.0" / "target" / "release");
    const auto expected_alpha_src =
        fs::weakly_canonical(project_root / ".eta" / "modules" / "alpha-0.1.0" / "src");

    BOOST_TEST(r.dirs()[0] == expected_root_src);
    BOOST_TEST(r.dirs()[1] == expected_beta_release);
    BOOST_TEST(r.dirs()[2] == expected_beta_src);
    BOOST_TEST(r.dirs()[3] == expected_alpha_release);
    BOOST_TEST(r.dirs()[4] == expected_alpha_src);
}

BOOST_AUTO_TEST_CASE(from_args_or_env_at_discovers_project_from_explicit_start_dir) {
    TempDir d;
    const auto project_root = d.path / "app";
    const auto nested_dir = project_root / "src" / "pkg";
    fs::create_directories(nested_dir);
    fs::create_directories(project_root / "src");
    fs::create_directories(project_root / ".eta" / "modules" / "dep-0.1.0" / "target" / "release");
    fs::create_directories(project_root / ".eta" / "modules" / "dep-0.1.0" / "src");

    d.create_file("app/eta.toml", R"toml(
[package]
name = "app"
version = "1.0.0"
license = "MIT"

[compatibility]
eta = ">=0.6, <0.8"

[dependencies]
dep = { path = "../dep" }
)toml");

    d.create_file("app/eta.lock", R"toml(
version = 1

[[package]]
name = "app"
version = "1.0.0"
source = "root"
dependencies = ["dep@0.1.0"]

[[package]]
name = "dep"
version = "0.1.0"
source = "path+../dep"
dependencies = []
)toml");

    CurrentPathGuard cwd_guard;
    fs::current_path(d.path);

    auto r = ModulePathResolver::from_args_or_env_at("", nested_dir);
    BOOST_REQUIRE_GE(r.dirs().size(), 3u);

    const auto expected_root_src = fs::weakly_canonical(project_root / "src");
    const auto expected_dep_release =
        fs::weakly_canonical(project_root / ".eta" / "modules" / "dep-0.1.0" / "target" / "release");
    const auto expected_dep_src =
        fs::weakly_canonical(project_root / ".eta" / "modules" / "dep-0.1.0" / "src");

    BOOST_TEST(r.dirs()[0] == expected_root_src);
    BOOST_TEST(r.dirs()[1] == expected_dep_release);
    BOOST_TEST(r.dirs()[2] == expected_dep_src);
}

BOOST_AUTO_TEST_CASE(from_args_or_env_at_workspace_member_includes_workspace_member_roots) {
    TempDir d;
    const auto workspace_root = d.path / "ws";
    const auto app_root = workspace_root / "packages" / "app";
    const auto lib_root = workspace_root / "packages" / "lib";
    fs::create_directories(app_root / "src" / "nested");
    fs::create_directories(lib_root / "src");
    fs::create_directories(workspace_root / ".eta" / "modules" / "dep-0.2.0" / "target" / "release");
    fs::create_directories(workspace_root / ".eta" / "modules" / "dep-0.2.0" / "src");

    d.create_file("ws/eta.toml", R"toml(
[workspace]
members = ["packages/*"]
)toml");
    d.create_file("ws/packages/app/eta.toml", R"toml(
[package]
name = "app"
version = "0.1.0"
license = "MIT"

[compatibility]
eta = ">=0.6, <0.8"

[dependencies]
lib = { path = "../lib" }
dep = { path = "../../dep" }
)toml");
    d.create_file("ws/packages/lib/eta.toml", R"toml(
[package]
name = "lib"
version = "0.1.0"
license = "MIT"

[compatibility]
eta = ">=0.6, <0.8"

[dependencies]
)toml");
    d.create_file("ws/eta.lock", R"toml(
version = 1

[[package]]
name = "app"
version = "0.1.0"
source = "workspace+packages/app"
dependencies = ["lib@0.1.0", "dep@0.2.0"]

[[package]]
name = "lib"
version = "0.1.0"
source = "workspace+packages/lib"
dependencies = []

[[package]]
name = "dep"
version = "0.2.0"
source = "path+../../dep"
dependencies = []
)toml");

    CurrentPathGuard cwd_guard;
    fs::current_path(d.path);

    auto r = ModulePathResolver::from_args_or_env_at("", app_root / "src" / "nested");
    BOOST_REQUIRE_GE(r.dirs().size(), 4u);

    const auto expected_app_src = fs::weakly_canonical(app_root / "src");
    const auto expected_lib_src = fs::weakly_canonical(lib_root / "src");
    const auto expected_dep_release =
        fs::weakly_canonical(workspace_root / ".eta" / "modules" / "dep-0.2.0" / "target" / "release");
    const auto expected_dep_src =
        fs::weakly_canonical(workspace_root / ".eta" / "modules" / "dep-0.2.0" / "src");

    BOOST_TEST(r.dirs()[0] == expected_app_src);
    BOOST_TEST(r.dirs()[1] == expected_lib_src);
    BOOST_TEST(r.dirs()[2] == expected_dep_release);
    BOOST_TEST(r.dirs()[3] == expected_dep_src);
}

BOOST_AUTO_TEST_CASE(
    from_args_or_env_at_workspace_member_with_mock_native_metadata_preserves_module_path_order) {
    TempDir d;
    const auto workspace_root = d.path / "ws";
    const auto app_root = workspace_root / "packages" / "app";
    const auto lib_root = workspace_root / "packages" / "lib";
    fs::create_directories(app_root / "src" / "nested");
    fs::create_directories(lib_root / "src");
    fs::create_directories(workspace_root / ".eta" / "modules" / "dep-0.2.0" / "target" / "release");
    fs::create_directories(workspace_root / ".eta" / "modules" / "dep-0.2.0" / "src");

    d.create_file("ws/eta.toml", R"toml(
[workspace]
members = ["packages/*"]
)toml");
    d.create_file("ws/packages/app/eta.toml", R"toml(
[package]
name = "app"
version = "0.1.0"
license = "MIT"

[compatibility]
eta = ">=0.6, <0.8"

[dependencies]
lib = { path = "../lib" }
dep = { path = "../../dep" }

[native]
kind = "sidecar"
abi = "eta-native-v1"
id = "app_native"
entry = "eta_register_extension_v1"

[[native.targets]]
triple = "x86_64-pc-windows-msvc"
artifact = "native/windows-x64/eta_app_native.dll"
sha256 = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"
)toml");
    d.create_file("ws/packages/lib/eta.toml", R"toml(
[package]
name = "lib"
version = "0.1.0"
license = "MIT"

[compatibility]
eta = ">=0.6, <0.8"

[dependencies]
)toml");
    d.create_file("ws/eta.lock", R"toml(
version = 1

[[package]]
name = "app"
version = "0.1.0"
source = "workspace+packages/app"
dependencies = ["lib@0.1.0", "dep@0.2.0"]

[[package]]
name = "lib"
version = "0.1.0"
source = "workspace+packages/lib"
dependencies = []

[[package]]
name = "dep"
version = "0.2.0"
source = "path+../../dep"
dependencies = []
)toml");

    CurrentPathGuard cwd_guard;
    fs::current_path(d.path);

    auto r = ModulePathResolver::from_args_or_env_at("", app_root / "src" / "nested");
    BOOST_REQUIRE_GE(r.dirs().size(), 4u);

    const auto expected_app_src = fs::weakly_canonical(app_root / "src");
    const auto expected_lib_src = fs::weakly_canonical(lib_root / "src");
    const auto expected_dep_release =
        fs::weakly_canonical(workspace_root / ".eta" / "modules" / "dep-0.2.0" / "target" / "release");
    const auto expected_dep_src =
        fs::weakly_canonical(workspace_root / ".eta" / "modules" / "dep-0.2.0" / "src");

    BOOST_TEST(r.dirs()[0] == expected_app_src);
    BOOST_TEST(r.dirs()[1] == expected_lib_src);
    BOOST_TEST(r.dirs()[2] == expected_dep_release);
    BOOST_TEST(r.dirs()[3] == expected_dep_src);
}

BOOST_AUTO_TEST_CASE(from_args_or_env_at_virtual_workspace_root_includes_member_sources_without_lockfile) {
    TempDir d;
    const auto workspace_root = d.path / "ws";
    fs::create_directories(workspace_root / "packages" / "app" / "src");
    fs::create_directories(workspace_root / "packages" / "lib" / "src");

    d.create_file("ws/eta.toml", R"toml(
[workspace]
members = ["packages/*"]
)toml");
    d.create_file("ws/packages/app/eta.toml", R"toml(
[package]
name = "app"
version = "0.1.0"
license = "MIT"

[compatibility]
eta = ">=0.6, <0.8"

[dependencies]
lib = { path = "../lib" }
)toml");
    d.create_file("ws/packages/lib/eta.toml", R"toml(
[package]
name = "lib"
version = "0.1.0"
license = "MIT"

[compatibility]
eta = ">=0.6, <0.8"

[dependencies]
)toml");

    CurrentPathGuard cwd_guard;
    fs::current_path(d.path);

    auto r = ModulePathResolver::from_args_or_env_at("", workspace_root);
    BOOST_REQUIRE_GE(r.dirs().size(), 2u);

    const auto expected_app_src = fs::weakly_canonical(workspace_root / "packages" / "app" / "src");
    const auto expected_lib_src = fs::weakly_canonical(workspace_root / "packages" / "lib" / "src");

    BOOST_TEST(r.dirs()[0] == expected_app_src);
    BOOST_TEST(r.dirs()[1] == expected_lib_src);
}

BOOST_AUTO_TEST_SUITE_END()

