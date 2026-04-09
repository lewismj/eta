/**
 * @file example_runner_tests.cpp
 * @brief Integration tests that run every .eta file in the examples/ directory
 *        through the full Driver pipeline (lex → parse → expand → link → analyze
 *        → emit → execute) and verify they complete without runtime errors.
 *
 * The test discovers example files relative to the project root, loads the
 * prelude, and runs each example.  A per-file Boost test case is registered
 * so failures are reported individually.
 *
 * Paths are injected via CMake compile definitions:
 *   -DETA_EXAMPLES_DIR="..."
 *   -DETA_STDLIB_DIR="..."
 */

#include <boost/test/unit_test.hpp>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include "eta/interpreter/driver.h"
#include "eta/interpreter/module_path.h"
#include "eta/runtime/port.h"

namespace fs = std::filesystem;

// ── Path discovery ──────────────────────────────────────────────────────────
// Compile definitions set by CMake; fall back to source-relative paths.

#ifndef ETA_EXAMPLES_DIR
#define ETA_EXAMPLES_DIR ""
#endif

#ifndef ETA_STDLIB_DIR
#define ETA_STDLIB_DIR ""
#endif

static fs::path examples_dir() {
    fs::path p(ETA_EXAMPLES_DIR);
    if (!p.empty() && fs::is_directory(p)) return p;
    // Fall back: guess based on common build layout
    // Project root is typically 3 levels above the test binary
    auto cwd = fs::current_path();
    // Try a few common relative paths
    for (auto& candidate : {
        cwd / "examples",
        cwd / ".." / "examples",
        cwd / ".." / ".." / "examples",
        cwd / ".." / ".." / ".." / "examples",
        cwd / ".." / ".." / ".." / ".." / "examples",
    }) {
        if (fs::is_directory(candidate)) return fs::canonical(candidate);
    }
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

// ── Collect all .eta example files ──────────────────────────────────────────

static std::vector<fs::path> collect_examples() {
    std::vector<fs::path> files;
    auto dir = examples_dir();
    if (dir.empty() || !fs::is_directory(dir)) return files;

    for (auto& entry : fs::recursive_directory_iterator(dir)) {
        if (entry.is_regular_file() && entry.path().extension() == ".eta") {
            files.push_back(entry.path());
        }
    }
    // Sort for deterministic order
    std::sort(files.begin(), files.end());
    return files;
}

// ── Torch-dependent example filtering ───────────────────────────────────────
// When the interpreter is built without -DETA_BUILD_TORCH=ON the torch/*
// builtins are absent, so examples that `(import std.torch)` cannot run.
static bool requires_torch([[maybe_unused]] const fs::path& file) {
    auto stem = file.stem().string();
    if (stem == "torch") return true;
    // Scan for (import std.torch) in case of indirect usage
    std::ifstream ifs(file);
    std::string line;
    while (std::getline(ifs, line)) {
        if (line.find("std.torch") != std::string::npos) return true;
    }
    return false;
}

// ── Test fixture ────────────────────────────────────────────────────────────

struct ExampleRunnerFixture {
    fs::path stdlib;
    fs::path examples;

    ExampleRunnerFixture()
        : stdlib(stdlib_dir())
        , examples(examples_dir())
    {}

    /**
     * Run a single .eta example file through the Driver.
     * Returns true on success, false on any error.
     */
    bool run_example(const fs::path& file) {
        if (stdlib.empty()) {
            BOOST_TEST_MESSAGE("stdlib directory not found — skipping");
            return false;
        }

        eta::interpreter::ModulePathResolver resolver({stdlib});
        // Also add the example's own directory so sibling imports work
        resolver.add_dir(file.parent_path());
        eta::interpreter::Driver driver(std::move(resolver), 8 * 1024 * 1024);

        // Suppress stdout from examples: redirect to a string port
        auto null_port = std::make_shared<eta::runtime::StringPort>(
            eta::runtime::StringPort::Mode::Output);
        driver.set_output_port(null_port);
        driver.set_error_port(null_port);

        auto prelude = driver.load_prelude();
        if (!prelude.loaded) {
            BOOST_TEST_MESSAGE("Failed to load prelude from " << stdlib.string());
            return false;
        }

        bool ok = driver.run_file(file);
        if (!ok) {
            // Emit diagnostics for debugging
            for (const auto& diag : driver.diagnostics().diagnostics()) {
                BOOST_TEST_MESSAGE("  " << diag.message);
            }
        }
        return ok;
    }
};

// ── Test suite ──────────────────────────────────────────────────────────────

BOOST_FIXTURE_TEST_SUITE(example_runner_tests, ExampleRunnerFixture)

BOOST_AUTO_TEST_CASE(all_examples_run_without_errors) {
    auto files = collect_examples();
    if (files.empty()) {
        BOOST_TEST_MESSAGE("No example files found — skipping. "
                           "Set ETA_EXAMPLES_DIR and ETA_STDLIB_DIR compile definitions.");
        return;
    }

    BOOST_TEST_MESSAGE("Found " << files.size() << " example files");
    BOOST_TEST_MESSAGE("stdlib: " << stdlib.string());
    BOOST_TEST_MESSAGE("examples: " << examples.string());

    int passed = 0, failed = 0;
    std::vector<std::string> failures;

    for (const auto& file : files) {
        auto rel = fs::relative(file, examples);
#ifndef ETA_HAS_TORCH
        if (requires_torch(file)) {
            BOOST_TEST_MESSAGE("  ⊘ " << rel.string() << " (requires torch — skipped)");
            continue;
        }
#endif
        BOOST_TEST_CONTEXT("Example: " << rel.string()) {
            bool ok = run_example(file);
            if (ok) {
                ++passed;
                BOOST_TEST_MESSAGE("  ✓ " << rel.string());
            } else {
                ++failed;
                failures.push_back(rel.string());
                BOOST_TEST_MESSAGE("  ✗ " << rel.string());
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

