#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

#include "eta/session/driver.h"
#include "eta/interpreter/module_path.h"
#include "eta/interpreter/repl_wrap.h"
#include "eta/runtime/nanbox.h"

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

static constexpr const char* BANNER =
    "eta REPL - type an expression and press Enter.\n"
    "Use Ctrl+C or (exit) to quit.\n";

int main(int argc, char* argv[]) {
    std::string cli_path;

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

        std::cerr << "error: unexpected argument: " << arg << "\n";
        print_usage(argv[0]);
        return 1;
    }

    /// Build module path resolver
    auto resolver = eta::interpreter::ModulePathResolver::from_args_or_env(cli_path);

    /// Warn when the module path is empty and no explicit --path was given.
    if (cli_path.empty()) {
        const char* env = std::getenv("ETA_MODULE_PATH");
        if (!env || env[0] == '\0') {
            auto bundled = eta::interpreter::ModulePathResolver::bundled_stdlib_dir();
            if (!bundled) {
                std::cerr << "warning: ETA_MODULE_PATH is not set and no "
                             "bundled stdlib found next to the executable.\n"
                             "         Use --path or set ETA_MODULE_PATH to "
                             "the directory containing prelude.eta.\n";
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
            std::cerr << "warning: prelude.eta not found in module search path.\n";
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

        /// Skip empty input
        if (buffer.empty() || buffer.find_first_not_of(" \t\n\r") == std::string::npos) {
            continue;
        }

        /// Handle (exit) and (quit) commands
        {
            auto trimmed = buffer;
            auto s = trimmed.find_first_not_of(" \t\n\r");
            auto e = trimmed.find_last_not_of(" \t\n\r");
            if (s != std::string::npos) {
                trimmed = trimmed.substr(s, e - s + 1);
            }
            if (trimmed == "(exit)" || trimmed == "(quit)") {
                break;
            }
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

        eta::runtime::nanbox::LispVal result{};
        bool ok;
        if (wrapped.last_is_expr) {
            ok = driver.run_source(wrapped.source, &result, wrapped.result_name);
        } else {
            ok = driver.run_source(wrapped.source);
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


