#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <limits>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include "eta/session/driver.h"
#include "eta/interpreter/module_path.h"
#include "eta/interpreter/repl_wrap.h"
#include "eta/runtime/nanbox.h"
#include "eta/runtime/prof/archive.h"
#include "eta/runtime/prof/pprof.h"
#include "eta/runtime/prof/profiler.h"

namespace fs = std::filesystem;

static void print_usage(const char* prog) {
    std::cerr << "Usage: " << prog << " [options]\n"
              << "\n"
              << "Options:\n"
              << "  --path <dirs>   Search path for .eta modules (";
#ifdef _WIN32
    std::cerr << "semicolon";
#else
    std::cerr << "colon";
#endif
    std::cerr << "-separated).\n"
              << "                  Falls back to ETA_MODULE_PATH environment variable.\n"
              << "  --strict-shadows Fail when a module resolves to multiple files.\n"
              << "  --help          Show this help message.\n";
}

/**
 * Check whether a line of input has balanced parentheses.
 * Returns true when the expression is complete (or on empty input).
 */
static bool is_balanced(const std::string& input) {
    int depth = 0;
    bool in_string = false;
    bool escape = false;

    for (char c : input) {
        if (escape) {
            escape = false;
            continue;
        }
        if (c == '\\' && in_string) {
            escape = true;
            continue;
        }
        if (c == '"') {
            in_string = !in_string;
            continue;
        }
        if (in_string) continue;
        if (c == '(') ++depth;
        else if (c == ')') --depth;
    }
    return depth <= 0 && !in_string;
}

/**
 * Split input into top-level forms (simple paren-balanced splitting).
 * Each returned string is one complete top-level form or bare atom.
 */
static std::vector<std::string> split_toplevel_forms(const std::string& input) {
    std::vector<std::string> forms;
    int depth = 0;
    bool in_string = false;
    bool escape = false;
    std::size_t form_start = std::string::npos;

    for (std::size_t i = 0; i < input.size(); ++i) {
        char c = input[i];

        if (escape) { escape = false; continue; }
        if (c == '\\' && in_string) { escape = true; continue; }
        if (c == '"') { in_string = !in_string; if (form_start == std::string::npos) form_start = i; continue; }
        if (in_string) continue;

        if (std::isspace(static_cast<unsigned char>(c))) {
            /// If we're outside parens and have accumulated a bare token, finish it
            if (depth == 0 && form_start != std::string::npos) {
                forms.push_back(input.substr(form_start, i - form_start));
                form_start = std::string::npos;
            }
            continue;
        }

        if (c == ';') {
            if (depth == 0 && form_start != std::string::npos) {
                forms.push_back(input.substr(form_start, i - form_start));
                form_start = std::string::npos;
            }
            while (i < input.size() && input[i] != '\n') ++i;
            continue;
        }

        if (form_start == std::string::npos) form_start = i;

        if (c == '(') {
            ++depth;
        } else if (c == ')') {
            --depth;
            if (depth == 0) {
                forms.push_back(input.substr(form_start, i + 1 - form_start));
                form_start = std::string::npos;
            }
        }
    }
    /// Trailing bare token
    if (form_start != std::string::npos) {
        auto trailing = input.substr(form_start);
        if (trailing.find_first_not_of(" \t\n\r") != std::string::npos) {
            forms.push_back(trailing);
        }
    }
    return forms;
}

[[nodiscard]] std::string trim_copy(std::string_view text) {
    const auto first = text.find_first_not_of(" \t\r\n");
    if (first == std::string_view::npos) return {};
    const auto last = text.find_last_not_of(" \t\r\n");
    return std::string(text.substr(first, last - first + 1u));
}

struct ReplProfileOptions {
    std::string mode{"sample"};
    std::uint32_t hz{1000};
    std::string format{"pretty"};
};

[[nodiscard]] std::optional<std::uint32_t> parse_positive_u32(std::string_view text) {
    if (text.empty()) return std::nullopt;
    std::uint64_t value = 0;
    for (const char ch : text) {
        if (!std::isdigit(static_cast<unsigned char>(ch))) return std::nullopt;
        value = value * 10u + static_cast<std::uint64_t>(ch - '0');
        if (value > static_cast<std::uint64_t>((std::numeric_limits<std::uint32_t>::max)())) {
            return std::nullopt;
        }
    }
    if (value == 0u) return std::nullopt;
    return static_cast<std::uint32_t>(value);
}

[[nodiscard]] bool is_valid_prof_format(std::string_view format) {
    return format == "pretty"
        || format == "json"
        || format == "speedscope"
        || format == "eta-prof"
        || format == "chrome"
        || format == "pprof";
}

[[nodiscard]] std::optional<ReplProfileOptions> parse_prof_options(
    std::string_view args,
    std::string* error_out) {
    if (error_out) error_out->clear();

    ReplProfileOptions options;
    std::istringstream in{std::string(args)};
    std::vector<std::string> tokens;
    std::string token;
    while (in >> token) {
        tokens.push_back(token);
    }

    for (std::size_t i = 0; i < tokens.size(); ++i) {
        const auto& current = tokens[i];
        if (current == "sample" || current == "trace") {
            options.mode = current;
            continue;
        }
        if (current == "--mode") {
            if (i + 1u >= tokens.size()) {
                if (error_out) *error_out = ":prof: --mode requires a value";
                return std::nullopt;
            }
            const auto& mode = tokens[++i];
            if (mode != "sample" && mode != "trace") {
                if (error_out) *error_out = ":prof: --mode must be sample or trace";
                return std::nullopt;
            }
            options.mode = mode;
            continue;
        }
        if (current.rfind("--mode=", 0) == 0) {
            const auto mode = current.substr(7);
            if (mode != "sample" && mode != "trace") {
                if (error_out) *error_out = ":prof: --mode must be sample or trace";
                return std::nullopt;
            }
            options.mode = mode;
            continue;
        }
        if (current == "--hz") {
            if (i + 1u >= tokens.size()) {
                if (error_out) *error_out = ":prof: --hz requires a value";
                return std::nullopt;
            }
            const auto hz = parse_positive_u32(tokens[++i]);
            if (!hz) {
                if (error_out) *error_out = ":prof: --hz must be a positive integer";
                return std::nullopt;
            }
            options.hz = *hz;
            continue;
        }
        if (current.rfind("--hz=", 0) == 0) {
            const auto hz = parse_positive_u32(current.substr(5));
            if (!hz) {
                if (error_out) *error_out = ":prof: --hz must be a positive integer";
                return std::nullopt;
            }
            options.hz = *hz;
            continue;
        }
        if (current == "--format") {
            if (i + 1u >= tokens.size()) {
                if (error_out) *error_out = ":prof: --format requires a value";
                return std::nullopt;
            }
            const auto& format = tokens[++i];
            if (!is_valid_prof_format(format)) {
                if (error_out) {
                    *error_out = ":prof: --format must be one of "
                                 "pretty|json|speedscope|eta-prof|chrome|pprof";
                }
                return std::nullopt;
            }
            options.format = format;
            continue;
        }
        if (current.rfind("--format=", 0) == 0) {
            const auto format = current.substr(9);
            if (!is_valid_prof_format(format)) {
                if (error_out) {
                    *error_out = ":prof: --format must be one of "
                                 "pretty|json|speedscope|eta-prof|chrome|pprof";
                }
                return std::nullopt;
            }
            options.format = format;
            continue;
        }

        if (error_out) *error_out = ":prof: unknown argument '" + current + "'";
        return std::nullopt;
    }
    return options;
}

static constexpr const char* BANNER =
    "eta REPL - type an expression and press Enter.\n"
    "Use Ctrl+C or (exit) to quit.\n"
    "Use :prof [sample|trace] [--hz N] [--format FMT] to profile the next submission.\n";

int main(int argc, char* argv[]) {
    std::string cli_path;
    bool strict_shadows = false;

    /// Parse CLI arguments
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];

        if (arg == "--help" || arg == "-h") {
            print_usage(argv[0]);
            return 0;
        }

        if (arg == "--path") {
            if (i + 1 >= argc) {
                std::cerr << "error: --path requires a value\n";
                return 1;
            }
            cli_path = argv[++i];
            continue;
        }

        if (arg == "--strict-shadows") {
            strict_shadows = true;
            continue;
        }

        std::cerr << "error: unexpected argument: " << arg << "\n";
        print_usage(argv[0]);
        return 1;
    }

    /// Build module path resolver
    auto resolver = eta::interpreter::ModulePathResolver::from_args_or_env(
        cli_path, strict_shadows);

    /// Warn when the module path is empty and no explicit --path was given.
    if (cli_path.empty()) {
        const char* env = std::getenv("ETA_MODULE_PATH");
        if (!env || env[0] == '\0') {
            auto bundled = eta::interpreter::ModulePathResolver::bundled_stdlib_dir();
            if (!bundled) {
                std::cerr << "warning: ETA_MODULE_PATH is not set and no "
                             "bundled stdlib found next to the executable.\n"
                             "         Use --path or set ETA_MODULE_PATH to "
                             "the directory containing std/prelude.eta.\n";
            }
        }
    }

    /// Create driver
    const std::size_t heap_bytes =
        eta::session::Driver::parse_heap_env_var("ETA_HEAP_SOFT_LIMIT");
    eta::session::Driver driver(std::move(resolver), heap_bytes);
    auto resolve = driver.file_resolver();

    /// Load prelude (if available in module path)
    bool prelude_available = false;
    {
        auto pr = driver.load_prelude();
        if (pr.found) {
            if (pr.loaded) {
                prelude_available = driver.has_module("std.prelude");
                std::cerr << "Loaded " << pr.path.string() << "\n";
            } else {
                std::cerr << "error: failed to load prelude from "
                          << pr.path.string() << "\n";
                driver.diagnostics().print_all(std::cerr, /*use_color=*/true, resolve);
            }
        } else {
            std::cerr << "warning: std.prelude not found in module search path.\n";
            const auto& dirs = driver.resolver().dirs();
            if (dirs.empty()) {
                std::cerr << "         (search path is empty)\n";
            } else {
                std::cerr << "         searched:\n";
                for (const auto& d : dirs) {
                    std::cerr << "           " << d.string() << "\n";
                }
            }
        }
    }

    /// REPL loop
    std::cout << BANNER;

    std::string buffer;
    bool continuation = false;

    /// Track prior REPL modules and their exported names.
    std::vector<eta::interpreter::PriorModule> prior_modules;
    std::optional<ReplProfileOptions> pending_profile;

    while (true) {
        /// Prompt
        if (continuation) {
            std::cout << "... ";
        } else {
            std::cout << "eta> ";
        }
        std::cout.flush();

        std::string line;
        if (!std::getline(std::cin, line)) {
            /// EOF (Ctrl+D / Ctrl+Z)
            std::cout << "\n";
            break;
        }

        /// Accumulate multi-line input
        if (continuation) {
            buffer += "\n" + line;
        } else {
            buffer = line;
        }

        /// Check for balanced parentheses before submitting
        if (!is_balanced(buffer)) {
            continuation = true;
            continue;
        }
        continuation = false;

        const auto trimmed = trim_copy(buffer);

        /// Skip empty input.
        if (trimmed.empty()) {
            continue;
        }

        /// Handle (exit) and (quit) commands.
        if (trimmed == "(exit)" || trimmed == "(quit)") {
            break;
        }

        if (trimmed.rfind(":prof", 0) == 0
            && (trimmed.size() == 5u
                || std::isspace(static_cast<unsigned char>(trimmed[5])))) {
            const auto args = trim_copy(trimmed.substr(5));
            if (args == "off" || args == "disable") {
                pending_profile.reset();
                std::cout << "[prof] cleared pending profile request\n";
                continue;
            }

            std::string option_error;
            auto parsed = parse_prof_options(args, &option_error);
            if (!parsed) {
                std::cerr << "error: "
                          << (option_error.empty() ? ":prof: invalid arguments" : option_error)
                          << "\n";
                continue;
            }

            pending_profile = std::move(*parsed);
            std::cout << "[prof] next submission mode=" << pending_profile->mode
                      << " hz=" << pending_profile->hz
                      << " format=" << pending_profile->format
                      << "\n";
            continue;
        }

        if (!trimmed.empty() && trimmed.front() == ':') {
            std::cerr << "error: unknown REPL command: " << trimmed << "\n";
            continue;
        }

        /**
         * Split input into individual top-level forms (supports multiple
         * forms per input, e.g. "(define x 10) x")
         */
        auto forms = split_toplevel_forms(buffer);
        if (forms.empty()) continue;

        static int repl_counter = 0;
        int this_id = repl_counter++;
        auto wrapped = eta::interpreter::wrap_repl_submission(
            forms, this_id, prelude_available, prior_modules);

        auto active_profile = pending_profile;
        pending_profile.reset();

        std::optional<std::uint64_t> profiler_session;
        if (active_profile.has_value()) {
            auto& profiler = eta::runtime::prof::runtime_profiler();
            auto session = (active_profile->mode == "sample")
                ? profiler.start_sample_session(active_profile->hz)
                : profiler.start_trace_session();
            if (!session) {
                std::cerr << "error: :prof: failed to start profiler session\n";
                active_profile.reset();
            } else {
                profiler_session = *session;
            }
        }

        eta::runtime::nanbox::LispVal result{};
        bool ok;
        if (wrapped.last_is_expr) {
            ok = driver.run_source(wrapped.source, &result, wrapped.result_name);
        } else {
            ok = driver.run_source(wrapped.source);
        }

        if (active_profile.has_value() && profiler_session.has_value()) {
            auto& profiler = eta::runtime::prof::runtime_profiler();
            const bool stopped = (active_profile->mode == "sample")
                ? profiler.stop_sample_session(*profiler_session)
                : profiler.stop_trace_session(*profiler_session);
            if (!stopped) {
                std::cerr << "error: :prof: failed to stop profiler session\n";
            } else {
                std::optional<std::string> report;
                const auto& format = active_profile->format;
                if (format == "pretty") {
                    report = profiler.render_pretty_report_for_session(*profiler_session);
                } else if (format == "json") {
                    report = profiler.render_json_report_for_session(*profiler_session);
                } else if (format == "speedscope") {
                    report = profiler.render_speedscope_report_for_session(*profiler_session);
                } else if (format == "eta-prof") {
                    report = profiler.render_archive_report_for_session(*profiler_session);
                } else if (format == "chrome") {
                    report = profiler.render_chrome_report_for_session(*profiler_session);
                } else if (format == "pprof") {
                    auto archive = profiler.render_archive_report_for_session(*profiler_session);
                    if (!archive) {
                        std::cerr << "error: :prof: failed to render profiler archive for pprof\n";
                    } else {
                        auto parsed_archive = eta::runtime::prof::parse_eta_prof_archive(*archive);
                        if (!parsed_archive) {
                            std::cerr << "error: :prof: failed to parse profiler archive for pprof: "
                                      << parsed_archive.error() << "\n";
                        } else {
                            auto pprof = eta::runtime::prof::write_pprof_profile(*parsed_archive);
                            if (!pprof) {
                                std::cerr << "error: :prof: " << pprof.error() << "\n";
                            } else {
                                report = std::move(*pprof);
                            }
                        }
                    }
                }

                if (report.has_value()) {
                    std::cout << *report;
                    if (report->empty() || report->back() != '\n') {
                        std::cout << "\n";
                    }
                } else if (format != "pprof") {
                    std::cerr << "error: :prof: failed to render profiler report\n";
                }
            }
        }

        if (ok) {
            prior_modules.push_back(eta::interpreter::PriorModule{
                wrapped.module_name,
                wrapped.user_defines,
                wrapped.user_imports});

            /// Print the result unless it's the void/unspecified value (Nil)
            if (wrapped.last_is_expr && result != eta::runtime::nanbox::Nil) {
                std::cout << "=> " << driver.format_value(result) << "\n";
            }
        } else {
            /// Print diagnostics
            driver.diagnostics().print_all(std::cerr, /*use_color=*/true, resolve);
        }
    }

    return 0;
}


