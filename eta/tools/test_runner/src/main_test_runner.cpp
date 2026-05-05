/**
 *
 * Discovers test files (recursively), runs each one, captures output,
 * and aggregates TAP 13 or JUnit XML.
 *
 * Usage:
 *   eta-test [options] [<path> ...]
 *
 * Options:
 *   --path <dirs>          Module search path (colon/semicolon-separated)
 *   --format tap|junit     Output format (default: tap)
 *   --help                 Show this message
 *
 * Any argument beginning with `--` that isn't in this list is a hard error
 * silently swallowed mistyped options (e.g. `--profile`) and produced
 * confusing "path does not exist" / "duplicate module" cascades.
 *
 * Test discovery rules:
 *   - If a path is a regular file, it is always accepted (any extension).
 *   - If a path is a directory, only files matching `*.test.eta` or
 *     `*_smoke.eta` are picked up.  Other `*.eta` files (e.g. stdlib
 *     that passing a stdlib-shaped directory does not try to run module
 *     sources as tests.
 *
 * Each *.test.eta file is expected to call (print-tap (run ...)) at top level.
 * eta-test captures that output, re-emits it aggregated, and reports a
 * combined summary.
 */

#include <algorithm>
#include <filesystem>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "eta/session/driver.h"
#include "eta/interpreter/module_path.h"
#include "eta/runtime/port.h"

namespace fs = std::filesystem;

/**
 * TAP result counters
 */

struct TapSummary {
    int total{0};
    int passed{0};
    int failed{0};
};

/**
 * Parse TAP 13 output from a single test file and return a summary.
 * Re-emits individual test lines to `out`, re-numbered from (base+1).
 */
static TapSummary parse_tap(const std::string& tap_output,
                              const std::string& file_label,
                              int base_test_num,
                              std::ostream& out,
                              const std::string& format)
{
    TapSummary sum;
    std::istringstream ss(tap_output);
    std::string line;
    bool seen_plan = false;
    int local_n = 0;

    while (std::getline(ss, line)) {
        if (line.empty()) continue;
        if (line.rfind("TAP version", 0) == 0) continue;
        if (line.rfind("1..", 0) == 0) { seen_plan = true; continue; }

        if (line.rfind("ok ", 0) == 0) {
            ++sum.total; ++sum.passed; ++local_n;
            if (format == "tap") {
                auto dash = line.find(" - ");
                std::string desc = (dash != std::string::npos) ? line.substr(dash + 3) : line.substr(3);
                out << "ok " << (base_test_num + local_n) << " - " << desc << "\n";
            }
        } else if (line.rfind("not ok ", 0) == 0) {
            ++sum.total; ++sum.failed; ++local_n;
            if (format == "tap") {
                auto dash = line.find(" - ");
                std::string desc = (dash != std::string::npos) ? line.substr(dash + 3) : line.substr(7);
                out << "not ok " << (base_test_num + local_n) << " - " << desc << "\n";
            }
        } else if (line.rfind("  ", 0) == 0 && format == "tap") {
            out << line << "\n";
        }
    }

    if (!seen_plan && sum.total == 0) {
        ++sum.total; ++sum.passed;
        if (format == "tap") {
            out << "ok " << (base_test_num + 1)
                << " - " << file_label << " (no TAP output)\n";
        }
    }

    return sum;
}

/**
 * File discovery
 */

/**
 * True iff `name` looks like a test file suitable for directory auto-discovery.
 * Accepts `*.test.eta` (canonical TAP tests) and `*_smoke.eta` (end-to-end
 * smoke drivers).  Rejects everything else so stdlib module sources like
 * `core.eta`, `supervisor.eta` are not accidentally run as
 * tests when a user points eta-test at the stdlib root.
 */
static bool is_discoverable_test_filename(const std::string& name) {
    auto ends_with = [&](std::string_view suffix) {
        return name.size() >= suffix.size() &&
               std::equal(suffix.rbegin(), suffix.rend(), name.rbegin());
    };
    return ends_with(".test.eta") || ends_with("_smoke.eta");
}

static void collect_test_files(const fs::path& p, std::vector<fs::path>& out) {
    if (!fs::exists(p)) {
        std::cerr << "eta-test: path does not exist: " << p << "\n";
        return;
    }
    if (fs::is_regular_file(p)) {
        /// Explicit files: accept any `.eta` extension.
        if (p.extension() == ".eta") out.push_back(p);
        return;
    }
    if (fs::is_directory(p)) {
        std::vector<fs::path> entries;
        for (const auto& entry : fs::recursive_directory_iterator(p)) {
            if (!entry.is_regular_file())              continue;
            if (entry.path().extension() != ".eta")    continue;
            if (!is_discoverable_test_filename(entry.path().filename().string()))
                continue;
            entries.push_back(entry.path());
        }
        std::sort(entries.begin(), entries.end());
        for (auto& e : entries) out.push_back(std::move(e));
    }
}

/**
 * JUnit helpers
 */

struct JUnitTestCase {
    std::string name;
    bool passed{true};
    std::string failure_msg;
};

static std::string xml_escape(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        switch (c) {
            case '&': out += "&amp;";  break;
            case '<': out += "&lt;";   break;
            case '>': out += "&gt;";   break;
            case '"': out += "&quot;"; break;
            default:  out += c;        break;
        }
    }
    return out;
}

static std::vector<JUnitTestCase> parse_tap_for_junit(const std::string& tap_output,
                                                       const std::string& file_label,
                                                       bool file_failed)
{
    std::vector<JUnitTestCase> cases;
    if (file_failed) {
        cases.push_back({ file_label, false, "File failed to load/execute" });
        return cases;
    }

    std::istringstream ss(tap_output);
    std::string line;
    JUnitTestCase* current = nullptr;

    while (std::getline(ss, line)) {
        if (line.empty() || line.rfind("TAP version", 0) == 0 || line.rfind("1..", 0) == 0)
            continue;
        if (line.rfind("ok ", 0) == 0) {
            auto dash = line.find(" - ");
            std::string desc = (dash != std::string::npos) ? line.substr(dash + 3) : line.substr(3);
            cases.push_back({ desc, true, {} });
            current = &cases.back();
        } else if (line.rfind("not ok ", 0) == 0) {
            auto dash = line.find(" - ");
            std::string desc = (dash != std::string::npos) ? line.substr(dash + 3) : line.substr(7);
            cases.push_back({ desc, false, {} });
            current = &cases.back();
        } else if (line.rfind("  message: ", 0) == 0 && current) {
            current->failure_msg = line.substr(11);
        }
    }

    if (cases.empty())
        cases.push_back({ file_label + " (no TAP output)", true, {} });
    return cases;
}

/**
 * main
 */

static void print_usage(const char* prog) {
    std::cerr << "Usage: " << prog << " [options] [<path> ...]\n\n"
              << "Discover and run test files.  Directory scans pick up\n"
              << "`*.test.eta` and `*_smoke.eta` files only; explicit paths to\n"
              << "regular `.eta` files are always accepted.\n\n"
              << "Options:\n"
              << "  --path <dirs>     Module search path (";
#ifdef _WIN32
    std::cerr << "semicolon";
#else
    std::cerr << "colon";
#endif
    std::cerr << "-separated).  Falls back to ETA_MODULE_PATH.\n"
              << "  --format tap      Output TAP 13 (default).\n"
              << "  --format junit    Output JUnit XML.\n"
              << "  --help            Show this message.\n\n"
              << "If no paths are given, searches the current directory for tests.\n";
}

int main(int argc, char* argv[]) {
    std::string cli_path;
    std::string format = "tap";
    std::vector<std::string> raw_paths;

    /// Helper: append a dir to cli_path using the platform-native separator.
    auto append_to_cli_path = [&](const std::string& dir) {
#ifdef _WIN32
        constexpr char SEP = ';';
#else
        constexpr char SEP = ':';
#endif
        if (cli_path.empty()) cli_path = dir;
        else                  cli_path += std::string(1, SEP) + dir;
    };

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--help" || arg == "-h") { print_usage(argv[0]); return 0; }
        if (arg == "--path") {
            if (i + 1 >= argc) { std::cerr << "error: --path requires a value\n"; return 1; }
            append_to_cli_path(argv[++i]); continue;
        }
        if (arg == "--format") {
            if (i + 1 >= argc) { std::cerr << "error: --format requires a value\n"; return 1; }
            format = argv[++i];
            if (format != "tap" && format != "junit") {
                std::cerr << "error: --format must be 'tap' or 'junit'\n"; return 1;
            }
            continue;
        }
        /**
         * Anything else that LOOKS like an option (starts with '-') is rejected
         * outright.  Previously unknown options were silently treated as
         * positional paths, producing confusing cascades like
         * "path does not exist: \"--profile\"" followed by duplicate-module
         * errors when the next positional was a whole stdlib directory.
         */
        if (!arg.empty() && arg[0] == '-') {
            std::cerr << "error: invalid argument: " << arg << "\n\n";
            print_usage(argv[0]);
            return 1;
        }
        raw_paths.push_back(arg);
    }

    if (raw_paths.empty()) raw_paths.push_back(".");

    /// Discover test files
    std::vector<fs::path> test_files;
    for (const auto& p : raw_paths) collect_test_files(fs::path(p), test_files);

    if (test_files.empty()) {
        std::cerr << "eta-test: no *.test.eta files found\n";
        return 0;
    }

    /// Base module path resolver (includes stdlib dir from build-time define)
    auto base_resolver = eta::interpreter::ModulePathResolver::from_args_or_env(cli_path);
#ifdef ETA_STDLIB_DIR
    base_resolver.add_dir(fs::path(ETA_STDLIB_DIR));
#endif

    /// Per-file results
    struct FileResult {
        fs::path path;
        std::string tap_output;
        bool file_ok{false};
    };
    std::vector<FileResult> file_results;
    file_results.reserve(test_files.size());

    for (const auto& test_file : test_files) {
        const std::size_t heap_bytes =
            eta::session::Driver::parse_heap_env_var("ETA_HEAP_SOFT_LIMIT");

        /// Fresh resolver per file: includes the file's own directory
        auto resolver = base_resolver;
        resolver.add_dir(fs::absolute(test_file).parent_path());

        eta::session::Driver driver(resolver, heap_bytes);

        /// Redirect VM output to a StringPort
        auto sp = std::make_shared<eta::runtime::StringPort>(
            eta::runtime::StringPort::Mode::Output);
        driver.set_output_port(sp);

        bool ok = driver.run_file(fs::absolute(test_file));
        if (!ok) {
            std::cerr << "eta-test: error in " << test_file.filename() << ":\n";
            driver.diagnostics().print_all(std::cerr, false, driver.file_resolver());
        }

        file_results.push_back({ test_file, sp->get_string(), ok });
    }


    if (format == "tap") {
        std::ostringstream body;
        int base = 0;
        int grand_total = 0;

        for (const auto& fr : file_results) {
            std::string label = fr.path.filename().string();
            if (!fr.file_ok) {
                ++grand_total;
                body << "not ok " << (base + 1) << " - " << label << " (load error)\n";
                ++base;
            } else {
                TapSummary s = parse_tap(fr.tap_output, label, base, body, "tap");
                grand_total += s.total;
                base += s.total;
            }
        }

        std::cout << "TAP version 13\n1.." << grand_total << "\n" << body.str();

        /// Exit non-zero on any failure
        for (const auto& fr : file_results) {
            if (!fr.file_ok) return 1;
            std::istringstream ss(fr.tap_output);
            std::string line;
            while (std::getline(ss, line)) {
                if (line.rfind("not ok ", 0) == 0) return 1;
            }
        }
        return 0;

    } else {
        /// JUnit XML
        int total_tests = 0, total_failures = 0;
        std::vector<std::pair<std::string, std::vector<JUnitTestCase>>> suites;

        for (const auto& fr : file_results) {
            std::string label = fr.path.stem().string();
            auto cases = parse_tap_for_junit(fr.tap_output, label, !fr.file_ok);
            for (const auto& c : cases) {
                ++total_tests;
                if (!c.passed) ++total_failures;
            }
            suites.push_back({ label, std::move(cases) });
        }

        std::cout << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
                  << "<testsuites tests=\"" << total_tests
                  << "\" failures=\"" << total_failures << "\">\n";

        for (const auto& [sname, cases] : suites) {
            int sf = 0;
            for (const auto& c : cases) if (!c.passed) ++sf;
            std::cout << "  <testsuite name=\"" << xml_escape(sname)
                      << "\" tests=\"" << cases.size()
                      << "\" failures=\"" << sf << "\">\n";
            for (const auto& c : cases) {
                if (c.passed) {
                    std::cout << "    <testcase name=\"" << xml_escape(c.name) << "\"/>\n";
                } else {
                    std::cout << "    <testcase name=\"" << xml_escape(c.name) << "\">\n"
                              << "      <failure message=\"" << xml_escape(c.failure_msg) << "\"/>\n"
                              << "    </testcase>\n";
                }
            }
            std::cout << "  </testsuite>\n";
        }
        std::cout << "</testsuites>\n";

        return total_failures > 0 ? 1 : 0;
    }
}


