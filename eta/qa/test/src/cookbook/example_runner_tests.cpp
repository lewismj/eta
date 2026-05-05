/**
 * @file example_runner_tests.cpp
 * @brief Integration tests that run every .eta file in the cookbook/ directory
 *
 * The test discovers example files relative to the project root and runs each
 * example.  A per-file Boost test case is registered
 * so failures are reported individually.
 *
 * Paths are injected via CMake compile definitions:
 *   -DETA_COOKBOOK_DIR="..."
 *   -DETA_STDLIB_DIR="..."
 */

#include <boost/test/unit_test.hpp>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

#include "eta/session/driver.h"
#include "eta/interpreter/module_path.h"
#include "eta/package/resolver.h"
#include "eta/runtime/port.h"

namespace fs = std::filesystem;

/**
 * Path discovery
 * Compile definitions set by CMake.
 */

#ifndef ETA_COOKBOOK_DIR
#define ETA_COOKBOOK_DIR ""
#endif

#ifndef ETA_STDLIB_DIR
#define ETA_STDLIB_DIR ""
#endif

static fs::path cookbook_dir() {
    fs::path p(ETA_COOKBOOK_DIR);
    if (!p.empty() && fs::is_directory(p)) return fs::canonical(p);
    return {};
}

static fs::path stdlib_dir() {
    fs::path p(ETA_STDLIB_DIR);
    if (!p.empty() && fs::is_directory(p)) return p;
    auto cwd = fs::current_path();
    for (auto& candidate : {
        cwd / "stdlib",
        cwd / ".." / "stdlib",
        cwd / ".." / ".." / "stdlib",
        cwd / ".." / ".." / ".." / "stdlib",
        cwd / ".." / ".." / ".." / ".." / "stdlib",
    }) {
        if (fs::is_directory(candidate)) return fs::canonical(candidate);
    }
    return {};
}

static fs::path canonicalize_path(const fs::path& path) {
    std::error_code ec;
    const auto canonical = fs::weakly_canonical(path, ec);
    if (!ec) return canonical;
    return path.lexically_normal();
}

static std::string canonical_key(const fs::path& path) {
    auto normalized = canonicalize_path(path).generic_string();
#ifdef _WIN32
    std::transform(normalized.begin(),
                   normalized.end(),
                   normalized.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
#endif
    return normalized;
}

static std::optional<fs::path> find_manifest_path(fs::path start_dir) {
    start_dir = canonicalize_path(start_dir);
    while (true) {
        auto candidate = start_dir / "eta.toml";
        std::error_code ec;
        if (fs::is_regular_file(candidate, ec) && !ec) {
            return canonicalize_path(candidate);
        }
        auto parent = start_dir.parent_path();
        if (parent.empty() || parent == start_dir) break;
        start_dir = parent;
    }
    return std::nullopt;
}

/// Collect all .eta example files

static std::vector<fs::path> collect_examples() {
    std::vector<fs::path> files;
    auto dir = cookbook_dir();
    if (dir.empty() || !fs::is_directory(dir)) return files;

    for (auto& entry : fs::recursive_directory_iterator(dir)) {
        if (entry.is_regular_file() && entry.path().extension() == ".eta") {
            files.push_back(entry.path());
        }
    }
    /// Sort for deterministic order
    std::sort(files.begin(), files.end());
    return files;
}

/**
 * Networking example filtering
 * Three kinds of files need to be skipped:
 *      parent.  Running standalone returns Nil and the first recv! errors out.
 *      (import std.net).  Without a real etai binary as the process manager the
 *      spawn call blocks forever waiting for the child to connect.
 *      directly without importing std.net (e.g. distributed-compute.eta,
 *      echo-server.eta).  These need a live peer and cannot run standalone.
 *
 * We scan only non-comment lines so that files whose doc-comment *mentions*
 * these symbols (e.g. message-passing.eta) are not falsely excluded.
 */
static bool requires_net(const fs::path& file) {
    std::ifstream ifs(file);
    std::string line;
    while (std::getline(ifs, line)) {
        /// Treat any line whose first non-whitespace character is ';' as a comment
        auto pos = line.find_first_not_of(" \t");
        if (pos != std::string::npos && line[pos] == ';') continue;
        if (line.find("current-mailbox") != std::string::npos) return true;
        if (line.find("std.net")         != std::string::npos) return true;
        if (line.find("nng-socket")      != std::string::npos) return true;
        if (line.find("nng-dial")        != std::string::npos) return true;
        if (line.find("nng-listen")      != std::string::npos) return true;
    }
    return false;
}

/**
 * Torch-dependent example filtering
 */
static bool requires_torch([[maybe_unused]] const fs::path& file) {
    auto stem = file.stem().string();
    if (stem == "torch") return true;
    /// Scan for (import std.torch) in case of indirect usage
    std::ifstream ifs(file);
    std::string line;
    while (std::getline(ifs, line)) {
        if (line.find("std.torch") != std::string::npos) return true;
    }
    return false;
}

/// Test fixture

struct ExampleRunnerFixture {
    fs::path stdlib;
    fs::path cookbook;

    ExampleRunnerFixture()
        : stdlib(stdlib_dir())
        , cookbook(cookbook_dir())
    {}

    /**
     * Run a single .eta example file through the Driver.
     * Returns true on success, false on any error.
     */
    bool run_example(const fs::path& file) {
        if (stdlib.empty()) {
            BOOST_TEST_MESSAGE("stdlib directory not found  -  skipping");
            return false;
        }

        eta::interpreter::ModulePathResolver resolver({stdlib});
        std::unordered_set<std::string> seen_dirs;
        seen_dirs.insert(canonical_key(stdlib));
        auto add_module_dir = [&](const fs::path& dir) {
            if (dir.empty()) return;
            std::error_code ec;
            if (!fs::is_directory(dir, ec) || ec) return;
            auto canonical = canonicalize_path(dir);
            if (seen_dirs.insert(canonical_key(canonical)).second) {
                resolver.add_dir(std::move(canonical));
            }
        };

        if (auto manifest = find_manifest_path(file.parent_path())) {
            auto root_manifest = eta::package::resolve_path_dependencies(*manifest);
            if (root_manifest) {
                for (const auto& pkg : root_manifest->packages) {
                    auto source_dir = pkg.package_root / "src";
                    std::error_code ec;
                    if (fs::is_directory(source_dir, ec) && !ec) {
                        add_module_dir(source_dir);
                    } else {
                        add_module_dir(pkg.package_root);
                    }
                }
            } else {
                BOOST_TEST_MESSAGE("Path dependency resolution failed for "
                                   << manifest->string() << ": "
                                   << root_manifest.error().message);
            }
        }

        /// Also add the example's own directory so sibling imports work
        add_module_dir(file.parent_path());
        eta::session::Driver driver(std::move(resolver), 8 * 1024 * 1024);

        /// Suppress stdout from cookbook programs: redirect to a string port
        auto null_port = std::make_shared<eta::runtime::StringPort>(
            eta::runtime::StringPort::Mode::Output);
        driver.set_output_port(null_port);
        driver.set_error_port(null_port);

        bool ok = driver.run_file(file);
        if (!ok) {
            /// Emit diagnostics for debugging
            for (const auto& diag : driver.diagnostics().diagnostics()) {
                BOOST_TEST_MESSAGE("  " << diag.message);
            }
        }
        return ok;
    }
};

/// Test suite

BOOST_FIXTURE_TEST_SUITE(example_runner_tests, ExampleRunnerFixture)

BOOST_AUTO_TEST_CASE(all_examples_run_without_errors) {
    auto files = collect_examples();
    if (files.empty()) {
        BOOST_TEST_MESSAGE("No example files found  -  skipping. "
                           "Set ETA_COOKBOOK_DIR and ETA_STDLIB_DIR compile definitions.");
        return;
    }

    BOOST_TEST_MESSAGE("Found " << files.size() << " example files");
    BOOST_TEST_MESSAGE("stdlib: " << stdlib.string());
    BOOST_TEST_MESSAGE("cookbook: " << cookbook.string());

    int passed = 0, failed = 0;
    std::vector<std::string> failures;

    for (const auto& file : files) {
        auto rel = fs::relative(file, cookbook);
#if !defined(ETA_HAS_TORCH) || defined(ETA_TORCH_DEBUG_SKIP)
        if (requires_torch(file)) {
#ifdef ETA_TORCH_DEBUG_SKIP
            BOOST_TEST_MESSAGE("  [SKIP] " << rel.string() << " (requires torch  -  skipped in MSVC Debug)");
#else
            BOOST_TEST_MESSAGE("  [SKIP] " << rel.string() << " (requires torch  -  skipped)");
#endif
            continue;
        }
#endif
        if (requires_net(file)) {
            BOOST_TEST_MESSAGE("  [SKIP] " << rel.string() << " (requires networking runtime  -  skipped)");
            continue;
        }
        BOOST_TEST_CONTEXT("Example: " << rel.string()) {
            bool ok = run_example(file);
            if (ok) {
                ++passed;
                BOOST_TEST_MESSAGE("  [OK] " << rel.string());
            } else {
                ++failed;
                failures.push_back(rel.string());
                BOOST_TEST_MESSAGE("  [FAIL] " << rel.string());
            }
            BOOST_CHECK_MESSAGE(ok, "Example " << rel.string() << " failed");
        }
    }

    BOOST_TEST_MESSAGE("");
    BOOST_TEST_MESSAGE("Results: " << passed << " passed, " << failed << " failed out of " << files.size());
    if (!failures.empty()) {
        BOOST_TEST_MESSAGE("Failures:");
        for (const auto& f : failures) {
            BOOST_TEST_MESSAGE("  - " << f);
        }
    }
}

BOOST_AUTO_TEST_SUITE_END()


