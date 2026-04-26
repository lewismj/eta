#include "eta/interpreter/repl_wrap.h"

#include <cctype>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

namespace eta::interpreter {

namespace {

static void skip_ws_and_comments(std::string_view input, std::size_t& pos) {
    while (pos < input.size()) {
        const char c = input[pos];
        if (std::isspace(static_cast<unsigned char>(c))) {
            ++pos;
            continue;
        }
        if (c == ';') {
            while (pos < input.size() && input[pos] != '\n') ++pos;
            continue;
        }
        break;
    }
}

static std::string read_symbol(std::string_view input, std::size_t& pos) {
    skip_ws_and_comments(input, pos);
    if (pos >= input.size()) return {};

    const char c = input[pos];
    if (c == '(' || c == ')' || c == '"' || c == ';') return {};

    const std::size_t start = pos;
    while (pos < input.size()) {
        const char ch = input[pos];
        if (std::isspace(static_cast<unsigned char>(ch)) || ch == '(' || ch == ')' || ch == ';') {
            break;
        }
        ++pos;
    }
    return std::string(input.substr(start, pos - start));
}

/// Skip a balanced list where `pos` points at '('.
static void skip_list(std::string_view input, std::size_t& pos) {
    if (pos >= input.size() || input[pos] != '(') return;

    int depth = 0;
    bool in_string = false;
    bool escape = false;

    while (pos < input.size()) {
        const char c = input[pos++];
        if (in_string) {
            if (escape) {
                escape = false;
                continue;
            }
            if (c == '\\') {
                escape = true;
                continue;
            }
            if (c == '"') {
                in_string = false;
            }
            continue;
        }

        if (c == '"') {
            in_string = true;
            continue;
        }

        if (c == ';') {
            while (pos < input.size() && input[pos] != '\n') ++pos;
            continue;
        }

        if (c == '(') {
            ++depth;
            continue;
        }
        if (c == ')') {
            --depth;
            if (depth == 0) return;
        }
    }
}

static std::string form_head(const std::string& input) {
    std::size_t pos = 0;
    skip_ws_and_comments(input, pos);
    if (pos >= input.size() || input[pos] != '(') return {};
    ++pos;
    return read_symbol(input, pos);
}

/// Detect whether a (trimmed) input line starts with a definition form.
static bool is_definition(const std::string& input) {
    const auto head = form_head(input);
    return head == "define" ||
           head == "def" ||
           head == "defun" ||
           head == "define-syntax" ||
           head == "define-record-type";
}

static void push_unique(std::vector<std::string>& out,
                        std::unordered_set<std::string>& seen,
                        std::string name) {
    if (name.empty()) return;
    if (seen.insert(name).second) {
        out.push_back(std::move(name));
    }
}

/**
 * Extract names introduced by top-level definition forms.
 *
 * For `define-record-type`, this returns constructor/predicate/accessor/mutator
 * names so they can be exported from synthesized REPL modules.
 */
static std::vector<std::string> extract_defined_names(const std::string& input) {
    std::vector<std::string> out;
    std::unordered_set<std::string> seen;

    std::size_t pos = 0;
    skip_ws_and_comments(input, pos);
    if (pos >= input.size() || input[pos] != '(') return out;
    ++pos;

    const auto head = read_symbol(input, pos);
    if (head.empty()) return out;

    if (head == "define" || head == "def") {
        skip_ws_and_comments(input, pos);
        if (pos < input.size() && input[pos] == '(') {
            ++pos;
            push_unique(out, seen, read_symbol(input, pos));
        } else {
            push_unique(out, seen, read_symbol(input, pos));
        }
        return out;
    }

    if (head == "defun" || head == "define-syntax") {
        push_unique(out, seen, read_symbol(input, pos));
        return out;
    }

    if (head == "define-record-type") {
        /// Type name (ignored): `(define-record-type <type> ...)`.
        (void)read_symbol(input, pos);

        /// Constructor spec: `(<ctor> field...)`.
        skip_ws_and_comments(input, pos);
        if (pos < input.size() && input[pos] == '(') {
            ++pos;
            push_unique(out, seen, read_symbol(input, pos));
            while (pos < input.size() && input[pos] != ')') {
                if (input[pos] == '(') {
                    skip_list(input, pos);
                } else {
                    ++pos;
                }
            }
            if (pos < input.size() && input[pos] == ')') ++pos;
        }

        /// Predicate symbol.
        push_unique(out, seen, read_symbol(input, pos));

        /// Field specs: `(field accessor)` or `(field accessor mutator)`.
        while (true) {
            skip_ws_and_comments(input, pos);
            if (pos >= input.size() || input[pos] == ')') break;

            if (input[pos] != '(') {
                ++pos;
                continue;
            }

            ++pos; ///< Skip '(' of field spec.
            (void)read_symbol(input, pos); ///< field name (not a definition)
            push_unique(out, seen, read_symbol(input, pos)); ///< accessor
            push_unique(out, seen, read_symbol(input, pos)); ///< optional mutator

            while (pos < input.size() && input[pos] != ')') {
                if (input[pos] == '(') {
                    skip_list(input, pos);
                } else {
                    ++pos;
                }
            }
            if (pos < input.size() && input[pos] == ')') ++pos;
        }
    }

    return out;
}

/// Detect whether a form is an `(import ...)` directive.
static bool is_import(const std::string& input) {
    auto pos = input.find_first_not_of(" \t\n\r");
    if (pos == std::string::npos || input[pos] != '(') return false;
    pos++; ///< Skip '('.
    pos = input.find_first_not_of(" \t\n\r", pos);
    if (pos == std::string::npos) return false;

    std::string_view rest(input.data() + pos, input.size() - pos);
    for (const char* kw : {"import ", "import\t", "import\n", "import)"}) {
        if (rest.starts_with(kw)) return true;
    }
    return false;
}

} ///< namespace

ReplWrapResult wrap_repl_submission(const std::vector<std::string>& forms,
                                    int repl_id,
                                    bool prelude_available,
                                    const std::vector<PriorModule>& prior_modules) {
    ReplWrapResult result;
    result.module_name = "__repl_" + std::to_string(repl_id);
    result.result_name = "__repl_r_" + std::to_string(repl_id);

    if (forms.empty()) return result;

    std::string body;
    std::string user_imports; ///< Explicit user import forms.

    for (std::size_t i = 0; i < forms.size(); ++i) {
        const bool is_last = (i == forms.size() - 1);
        if (is_import(forms[i])) {
            user_imports += "  " + forms[i] + "\n";
        } else if (is_definition(forms[i])) {
            auto names = extract_defined_names(forms[i]);
            result.user_defines.insert(
                result.user_defines.end(), names.begin(), names.end());
            body += "    " + forms[i] + "\n";
        } else if (is_last) {
            body += "    (define " + result.result_name + " " + forms[i] + ")\n";
            result.last_is_expr = true;
        } else {
            body += "    " + forms[i] + "\n";
        }
    }

    if (!result.last_is_expr) {
        result.result_name.clear();
    }

    std::string imports;
    if (prelude_available) {
        imports += "  (import std.prelude)\n";
    }

    std::unordered_set<std::string> shadowed(result.user_defines.begin(), result.user_defines.end());
    std::unordered_map<std::string, std::vector<std::string>> visible_from;
    std::vector<std::string> emit_order; ///< Newest-first module names.

    for (auto it = prior_modules.rbegin(); it != prior_modules.rend(); ++it) {
        std::vector<std::string> live;
        for (const auto& name : it->exports) {
            if (shadowed.insert(name).second) {
                live.push_back(name);
            }
        }
        if (!live.empty()) {
            visible_from.emplace(it->name, std::move(live));
            emit_order.push_back(it->name);
        }
    }

    /// Emit oldest-still-visible first for deterministic ordering.
    for (auto it = emit_order.rbegin(); it != emit_order.rend(); ++it) {
        const auto& mod = *it;
        const auto& live = visible_from.at(mod);
        imports += "  (import (only " + mod;
        for (const auto& n : live) imports += " " + n;
        imports += "))\n";
    }

    std::string exports;
    if (!result.user_defines.empty()) {
        exports = "  (export";
        for (const auto& name : result.user_defines) exports += " " + name;
        exports += ")\n";
    }

    result.source = "(module " + result.module_name + "\n"
                    + exports
                    + imports
                    + user_imports
                    + "  (begin\n"
                    + body
                    + "  ))";
    return result;
}

} ///< namespace eta::interpreter
