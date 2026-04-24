#include "eta/interpreter/repl_wrap.h"

#include <cctype>
#include <cstring>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

namespace eta::interpreter {

/// Detect whether a (trimmed) input line starts with a definition form.
static bool is_definition(const std::string& input) {
    auto pos = input.find_first_not_of(" \t\n\r");
    if (pos == std::string::npos || input[pos] != '(') return false;
    pos++; ///< Skip '('.
    pos = input.find_first_not_of(" \t\n\r", pos);
    if (pos == std::string::npos) return false;

    for (const char* kw : {"define ", "define\t", "define\n",
                           "defun ", "defun\t", "defun\n",
                           "def ", "def\t", "def\n",
                           "define-syntax ", "define-syntax\t", "define-syntax\n"}) {
        std::string_view rest(input.data() + pos, input.size() - pos);
        if (rest.starts_with(kw)) return true;
    }
    return false;
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

/**
 * Extract the defined name from a `(define name ...)` or `(defun name ...)`
 * form. Returns an empty string for non-definition forms.
 */
static std::string extract_define_name(const std::string& input) {
    auto pos = input.find_first_not_of(" \t\n\r");
    if (pos == std::string::npos || input[pos] != '(') return {};
    pos++;
    pos = input.find_first_not_of(" \t\n\r", pos);
    if (pos == std::string::npos) return {};

    std::string_view rest(input.data() + pos, input.size() - pos);
    for (const char* kw : {"define", "defun", "def"}) {
        if (rest.starts_with(kw)) {
            pos += std::strlen(kw);
            break;
        }
    }

    pos = input.find_first_not_of(" \t\n\r", pos);
    if (pos == std::string::npos) return {};

    /// The name might be a function shorthand form: `(f x y)`.
    if (input[pos] == '(') {
        pos++;
        pos = input.find_first_not_of(" \t\n\r", pos);
        if (pos == std::string::npos) return {};
    }

    auto end = pos;
    while (end < input.size() && !std::isspace(static_cast<unsigned char>(input[end]))
           && input[end] != ')' && input[end] != '(') {
        ++end;
    }
    return input.substr(pos, end - pos);
}

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
            auto name = extract_define_name(forms[i]);
            if (!name.empty()) result.user_defines.push_back(name);
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

