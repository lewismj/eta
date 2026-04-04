#include <filesystem>
#include <iostream>
#include <string>

#include "eta/interpreter/driver.h"
#include "eta/interpreter/module_path.h"
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

/// Check whether a line of input has balanced parentheses.
/// Returns true when the expression is complete (or on empty input).
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

/// Detect whether a (trimmed) input line starts with a definition form.
static bool is_definition(const std::string& input) {
    // Find first non-whitespace
    auto pos = input.find_first_not_of(" \t\n\r");
    if (pos == std::string::npos || input[pos] != '(') return false;
    pos++; // skip '('
    // Skip whitespace after '('
    pos = input.find_first_not_of(" \t\n\r", pos);
    if (pos == std::string::npos) return false;

    // Check if it starts with a definition keyword
    for (const char* kw : {"define ", "define\t", "define\n",
                            "defun ", "defun\t", "defun\n",
                            "def ", "def\t", "def\n"}) {
        std::string_view rest(input.data() + pos, input.size() - pos);
        if (rest.starts_with(kw)) return true;
    }
    return false;
}

/// Extract the defined name from a (define name ...) or (defun name ...) form.
/// Returns empty string if not a recognizable definition.
static std::string extract_define_name(const std::string& input) {
    auto pos = input.find_first_not_of(" \t\n\r");
    if (pos == std::string::npos || input[pos] != '(') return {};
    pos++;
    pos = input.find_first_not_of(" \t\n\r", pos);
    if (pos == std::string::npos) return {};

    // Skip keyword
    std::string_view rest(input.data() + pos, input.size() - pos);
    for (const char* kw : {"define", "defun", "def"}) {
        if (rest.starts_with(kw)) {
            pos += std::strlen(kw);
            break;
        }
    }

    pos = input.find_first_not_of(" \t\n\r", pos);
    if (pos == std::string::npos) return {};

    // The name might be bare `x` or a function shorthand `(f x y)`
    if (input[pos] == '(') {
        // (define (f x y) ...) — name is the first symbol inside
        pos++;
        pos = input.find_first_not_of(" \t\n\r", pos);
        if (pos == std::string::npos) return {};
    }

    // Collect the identifier
    auto end = pos;
    while (end < input.size() && !std::isspace(static_cast<unsigned char>(input[end]))
           && input[end] != ')' && input[end] != '(') {
        ++end;
    }
    return input.substr(pos, end - pos);
}

/// Split input into top-level forms (simple paren-balanced splitting).
/// Each returned string is one complete top-level form or bare atom.
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
            // If we're outside parens and have accumulated a bare token, finish it
            if (depth == 0 && form_start != std::string::npos) {
                forms.push_back(input.substr(form_start, i - form_start));
                form_start = std::string::npos;
            }
            continue;
        }

        // Start of comment — skip to end of line
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
    // Trailing bare token
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

    // ── Parse CLI arguments ──────────────────────────────────────────
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

    // ── Build module path resolver ───────────────────────────────────
    auto resolver = eta::interpreter::ModulePathResolver::from_args_or_env(cli_path);

    // ── Create driver ────────────────────────────────────────────────
    eta::interpreter::Driver driver(std::move(resolver));

    // Load prelude (if available in module path)
    if (!driver.load_prelude()) {
        driver.diagnostics().print_all(std::cerr, /*use_color=*/true);
        // Non-fatal for REPL — continue without prelude
    }

    // ── REPL loop ────────────────────────────────────────────────────
    std::cout << BANNER;

    std::string buffer;
    bool continuation = false;

    // Track previous REPL module names so each new module can import from them.
    std::vector<std::string> prior_modules;

    while (true) {
        // Prompt
        if (continuation) {
            std::cout << "... ";
        } else {
            std::cout << "eta> ";
        }
        std::cout.flush();

        std::string line;
        if (!std::getline(std::cin, line)) {
            // EOF (Ctrl+D / Ctrl+Z)
            std::cout << "\n";
            break;
        }

        // Accumulate multi-line input
        if (continuation) {
            buffer += "\n" + line;
        } else {
            buffer = line;
        }

        // Check for balanced parentheses before submitting
        if (!is_balanced(buffer)) {
            continuation = true;
            continue;
        }
        continuation = false;

        // Skip empty input
        if (buffer.empty() || buffer.find_first_not_of(" \t\n\r") == std::string::npos) {
            continue;
        }

        // Split input into individual top-level forms (supports multiple
        // forms per input, e.g. "(define x 10) x")
        auto forms = split_toplevel_forms(buffer);
        if (forms.empty()) continue;

        // Build the module body. Definitions go in directly; the last
        // form, if it is an expression, gets captured in a unique result binding.
        static int repl_counter = 0;
        int this_id = repl_counter++;
        std::string module_name = "__repl_" + std::to_string(this_id);
        // Unique result name per module to avoid import conflicts
        std::string result_name = "__repl_r_" + std::to_string(this_id);

        // Collect user-defined names for auto-export (NOT the result binding)
        std::vector<std::string> user_defines;
        std::string body;
        bool last_is_expr = false;

        for (std::size_t i = 0; i < forms.size(); ++i) {
            bool is_last = (i == forms.size() - 1);
            if (is_definition(forms[i])) {
                auto name = extract_define_name(forms[i]);
                if (!name.empty()) user_defines.push_back(name);
                body += "    " + forms[i] + "\n";
            } else if (is_last) {
                // Last form is an expression — capture its value
                body += "    (define " + result_name + " " + forms[i] + ")\n";
                last_is_expr = true;
            } else {
                // Non-last expression — run for side effects
                body += "    " + forms[i] + "\n";
            }
        }

        // Build import clauses from all prior REPL modules
        std::string imports;
        for (const auto& prev : prior_modules) {
            imports += "  (import " + prev + ")\n";
        }

        // Build export clause for user defines only (not __repl_r_N)
        std::string exports;
        if (!user_defines.empty()) {
            exports = "  (export";
            for (const auto& n : user_defines) exports += " " + n;
            exports += ")\n";
        }

        std::string wrapped = "(module " + module_name + "\n"
                              + exports
                              + imports
                              + "  (begin\n"
                              + body
                              + "  ))";

        eta::runtime::nanbox::LispVal result{};
        bool ok;
        if (last_is_expr) {
            ok = driver.run_source(wrapped, &result, result_name);
        } else {
            ok = driver.run_source(wrapped);
        }

        if (ok) {
            // Success — register this module so future inputs can import from it
            prior_modules.push_back(module_name);

            // Print the result unless it's the void/unspecified value (Nil)
            if (last_is_expr && result != eta::runtime::nanbox::Nil) {
                std::cout << "=> " << driver.format_value(result) << "\n";
            }
        } else {
            // Print diagnostics
            driver.diagnostics().print_all(std::cerr, /*use_color=*/true);
        }
    }

    return 0;
}

