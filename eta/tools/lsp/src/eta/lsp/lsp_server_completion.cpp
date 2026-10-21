/// LSP completion, signature help and inlay hints.
#include "lsp_server.h"

#include <algorithm>
#include <cassert>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_set>
#include <variant>


/// Eta compiler includes
#include "eta/reader/lexer.h"
#include "eta/reader/parser.h"
#include "eta/reader/expander.h"
#include "eta/reader/module_linker.h"
#include "eta/reader/special_form_docs.h"
#include "eta/reader/sexpr_utils.h"
#include "eta/semantics/semantic_analyzer.h"
#include "eta/docs/markdown.h"
#include "eta/docs/stdlib_docs.h"
#include "eta/runtime/builtin_catalog.h"
#include "eta/runtime/builtin_env.h"
#include "eta/runtime/builtin_metadata.h"
#include "eta/runtime/memory/heap.h"
#include "eta/runtime/memory/intern_table.h"
#include "eta/runtime/vm/bytecode_serializer.h"
#include "eta/interpreter/repl_complete.h"
#include "eta/package/discovery.h"
#include "eta/package/lockfile.h"
#include "eta/package/manifest.h"
#include "eta/package/resolver.h"

/// Shared tooling libraries (§8.2): the LSP links the formatter and linter
/// libraries directly and never shells out to eta_fmt / eta_lint.
#include "eta/cst/parse.h"
#include "eta/format/format_source.h"
#include "eta/linter/core/engine.h"
#include "eta/linter/core/registry.h"
#include "eta/linter/lint.h"
#include "eta/linter/rules/rules.h"
#include "eta/tooling/framing.h"
#include "lsp_server_internal.h"

namespace eta::lsp {

using namespace eta::json;

/**
 */

Value LspServer::handle_completion(const Value& params) {
    auto uri = params["textDocument"].get_string("uri").value_or("");
    if (!uri.empty()) {
        ensure_workspace_for_uri(uri);
    }
    auto it = documents_.find(uri);

    /// Lazily load the completion cache on first completion request
    if (!completion_cache_loaded_) {
        load_completion_cache();
    }

    Array items;
    std::unordered_set<std::string> seen; ///< dedup across all tiers

    /// Tier 0: language-level special forms
    for (const auto& entry : eta::reader::special_form_docs()) {
        const std::string name(entry.name);
        if (!seen.insert(name).second) continue;
        const std::string detail = entry.category.empty()
            ? "Keyword"
            : std::string(entry.category);
        items.push_back(json::object({
            {"label", name},
            {"kind", 14}, ///< Keyword
            {"detail", detail},
            {"documentation", json::object({
                {"kind", "markdown"},
                {"value", eta::docs::render_markdown(entry)},
            })},
        }));
    }

    /// Tier 1: configured builtin metadata
    for (const auto& builtin : eta::runtime::builtin_metadata()) {
        if (!seen.insert(builtin.name).second) continue;
        std::string detail = builtin.category.empty() ? "Builtin" : builtin.category;
        detail += " (arity " + std::to_string(builtin.arity);
        if (builtin.has_rest) detail += "+";
        detail += ")";
        items.push_back(json::object({
            {"label", builtin.name},
            {"kind", 3}, ///< Function
            {"detail", detail},
            {"documentation", json::object({
                {"kind", "markdown"},
                {"value", eta::docs::render_builtin_markdown(builtin)},
            })},
        }));
    }

    /// Tier 2: Prelude symbols (std.core, std.math, etc.)
    for (const auto& sym : prelude_symbols_) {
        if (!seen.insert(sym.name).second) continue;
        int kind = 3; ///< Function
        if (sym.kind == "macro") kind = 15;
        if (sym.kind == "module") kind = 9;
        std::string detail = sym.module_name.empty() ? sym.kind : (sym.module_name + "  -  " + sym.kind);
        if (!sym.signature.empty()) detail += " " + sym.signature;
        items.push_back(json::object({
            {"label", sym.name},
            {"kind", kind},
            {"detail", detail},
        }));
    }

    /// Tier 3: Module-path symbols
    for (const auto& sym : module_path_symbols_) {
        if (!seen.insert(sym.name).second) continue;
        int kind = 3;
        if (sym.kind == "macro") kind = 15;
        if (sym.kind == "module") kind = 9;
        std::string detail = sym.module_name.empty() ? sym.kind : (sym.module_name + "  -  " + sym.kind);
        if (!sym.signature.empty()) detail += " " + sym.signature;
        items.push_back(json::object({
            {"label", sym.name},
            {"kind", kind},
            {"detail", detail},
        }));
    }

    /// Tier 4: Document-local symbols
    if (it != documents_.end()) {
        auto symbols = collect_symbols(it->second.content, true);
        for (const auto& sym : symbols) {
            if (!seen.insert(sym.name).second) continue;
            int kind = 6; ///< Variable
            if (sym.kind == "defun" || sym.kind == "function") kind = 3;
            if (sym.kind == "macro") kind = 15;
            if (sym.kind == "module") kind = 9;
            std::string detail = sym.kind;
            if (!sym.signature.empty()) detail += " " + sym.signature;
            items.push_back(json::object({
                {"label", sym.name},
                {"kind", kind},
                {"detail", detail},
            }));
        }
    }

    return json::object({
        {"isIncomplete", false},
        {"items", Value(std::move(items))},
    });
}

/**
 */

void LspServer::load_completion_cache() {
    completion_cache_loaded_ = true;
    prelude_symbols_.clear();
    module_path_symbols_.clear();

    /// Scan module path
    scan_module_path_symbols();
}

void LspServer::scan_module_path_symbols() {
    for (const auto& path : discover_workspace_eta_files(resolver_.dirs())) {
        std::ifstream f(path);
        if (!f.is_open()) continue;
        std::string src(std::istreambuf_iterator<char>(f),
                        std::istreambuf_iterator<char>{});

        auto syms = collect_symbols(src, /*capture_signature=*/true);
        auto stem = path.stem().string();
        auto file_str = path.string();
        for (auto& sym : syms) {
            if (sym.kind == "module") continue;
            if (sym.module_name.empty()) sym.module_name = stem;
            sym.file_path = file_str;
            module_path_symbols_.push_back(std::move(sym));
        }
    }
}

/**
 * textDocument/signatureHelp
 */

Value LspServer::handle_signature_help(const Value& params) {
    auto uri = params["textDocument"].get_string("uri").value_or("");
    auto it = documents_.find(uri);
    if (it == documents_.end()) return Value(nullptr);

    auto line_num = params["position"].get_int("line").value_or(0);
    auto col_num  = params["position"].get_int("character").value_or(0);

    const auto& source = it->second.content;

    /// Build a flat string of all text up to the cursor
    std::string prefix;
    {
        std::istringstream iss(source);
        std::string ln;
        int64_t row = 0;
        while (std::getline(iss, ln)) {
            if (!ln.empty() && ln.back() == '\r') ln.pop_back();
            if (row < line_num) {
                prefix += ln + '\n';
            } else if (row == line_num) {
                auto clip = static_cast<std::size_t>(std::max<int64_t>(col_num, 0));
                prefix += ln.substr(0, std::min(clip, ln.size()));
                break;
            }
            ++row;
        }
    }

    /**
     * We track paren depth; skip strings; ignore line comments (scanning backwards
     * so we just skip everything after a ';' on each line when going forward).
     */
    int paren_depth = 0;
    int func_start  = -1;

    for (int i = static_cast<int>(prefix.size()) - 1; i >= 0; --i) {
        char c = prefix[static_cast<std::size_t>(i)];
        if (c == ')')      { ++paren_depth; }
        else if (c == '(') {
            if (paren_depth == 0) { func_start = i + 1; break; }
            --paren_depth;
        }
        /**
         * Note: string / comment handling when scanning backwards is complex;
         * for the common case this simple scan is sufficient.
         */
    }

    if (func_start < 0) return Value(nullptr);

    /// Skip whitespace between '(' and function name
    while (func_start < static_cast<int>(prefix.size()) &&
           (prefix[static_cast<std::size_t>(func_start)] == ' ' ||
            prefix[static_cast<std::size_t>(func_start)] == '\t' ||
            prefix[static_cast<std::size_t>(func_start)] == '\n'))
        ++func_start;

    /// Extract function name (up to whitespace / paren)
    int func_end = func_start;
    while (func_end < static_cast<int>(prefix.size())) {
        char c = prefix[static_cast<std::size_t>(func_end)];
        if (c == ' ' || c == '\t' || c == '\n' || c == '(' || c == ')') break;
        ++func_end;
    }
    if (func_end <= func_start) return Value(nullptr);

    std::string func_name = prefix.substr(
        static_cast<std::size_t>(func_start),
        static_cast<std::size_t>(func_end - func_start));

    /// Count complete top-level arguments after the function name (= active param index)
    int active_param = 0;
    {
        int pos      = func_end;
        bool in_tok  = false;
        int depth2   = 0;
        while (pos < static_cast<int>(prefix.size())) {
            char c = prefix[static_cast<std::size_t>(pos)];
            if (c == '(') { ++depth2; in_tok = true; }
            else if (c == ')') {
                if (depth2 == 0) break;
                --depth2;
                if (depth2 == 0) in_tok = true; ///< closing a nested sexp counts as a token
            } else if (depth2 == 0) {
                if (c == ' ' || c == '\t' || c == '\n') {
                    if (in_tok) { ++active_param; in_tok = false; }
                } else {
                    in_tok = true;
                }
            }
            ++pos;
        }
    }

    std::string label;
    if (auto builtin = eta::runtime::lookup_builtin_metadata(func_name)) {
        label = eta::runtime::format_builtin_signature(*builtin);
    }
    if (label.empty()) {
        if (auto entry = eta::reader::lookup_special_form_doc(func_name)) {
            label = std::string(entry->signature);
        }
    }
    if (label.empty()) {
        if (auto stdlib_doc = eta::docs::lookup_stdlib_doc(func_name)) {
            label = std::string(stdlib_doc->signature);
        }
    }

    /// Check document-local symbols
    if (label.empty()) {
        auto syms = collect_symbols(source, true);
        for (const auto& sym : syms) {
            if (sym.name == func_name && !sym.signature.empty()) {
                label = "(" + sym.name + " " +
                        sym.signature.substr(1, sym.signature.size() > 2 ? sym.signature.size() - 2 : 0) + ")";
                break;
            }
        }
    }

    /// Check cached prelude / module-path symbols
    if (label.empty() && completion_cache_loaded_) {
        for (const auto& sym : prelude_symbols_) {
            if (sym.name == func_name && !sym.signature.empty()) {
                label = "(" + sym.name + " " +
                        sym.signature.substr(1, sym.signature.size() > 2 ? sym.signature.size() - 2 : 0) + ")";
                break;
            }
        }
        if (label.empty()) {
            for (const auto& sym : module_path_symbols_) {
                if (sym.name == func_name && !sym.signature.empty()) {
                    label = "(" + sym.name + " " +
                            sym.signature.substr(1, sym.signature.size() > 2 ? sym.signature.size() - 2 : 0) + ")";
                    break;
                }
            }
        }
    }

    if (label.empty()) return Value(nullptr);

    return json::object({
        {"signatures", json::array({
            json::object({
                {"label", label},
            }),
        })},
        {"activeSignature", 0},
        {"activeParameter", active_param},
    });
}

/**
 * textDocument/inlayHint
 *
 * For every call site `(callee arg0 arg1 ...)` whose callee resolves to a
 * known defun in the current document, prelude or a module-path file,
 * emit a "name:" hint immediately before each positional argument.  Hints
 * are filtered to the requested range.
 */
namespace {

/**
 * Extract param names from a captured signature like "(f g)" or "(x . rest)".
 * Skips outer parens, splits on whitespace, ignores dotted-rest markers and
 * any token starting with '&' (Common-Lisp keyword params).
 */
std::vector<std::string> parse_inlay_param_names(const std::string& signature) {
    std::vector<std::string> out;
    if (signature.size() < 2) return out;
    std::size_t i   = (signature.front() == '(') ? 1u : 0u;
    std::size_t end = signature.size();
    if (end > 0 && signature[end - 1] == ')') --end;

    std::string tok;
    auto flush = [&]() {
        if (tok.empty()) return;
        if (tok != "." && tok[0] != '&') out.push_back(tok);
        tok.clear();
    };
    int depth = 0;
    for (; i < end; ++i) {
        char c = signature[i];
        if (c == '(') { ++depth; flush(); continue; }
        if (c == ')') { --depth; flush(); continue; }
        if (depth > 0) continue;
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
            flush();
        } else {
            tok += c;
        }
    }
    flush();
    return out;
}

struct InlayCallSiteArg {
    int64_t line{0};
    int64_t character{0};
};

struct InlayCallSite {
    std::string                   callee;
    std::vector<InlayCallSiteArg> args;
};

/**
 * Forward-scan source emitting one InlayCallSite per `(callee a b ...)` form.
 * Tracks line/column, double-quoted strings, ';' line comments and paren
 * depth.  The position recorded for each argument is the first character of
 * that argument (the outermost '(' if the argument is itself a sexp).
 */
std::vector<InlayCallSite> find_inlay_call_sites(const std::string& source) {
    std::vector<InlayCallSite> out;

    int64_t line = 0;
    int64_t col  = 0;
    bool in_string       = false;
    bool in_line_comment = false;

    struct Frame {
        bool          seen_callee{false};
        bool          in_arg_token{false};
        int           inner_depth{0}; ///< nesting depth of the *current* argument
        InlayCallSite site;
    };
    std::vector<Frame> stack;

    auto end_token = [&](Frame& f) {
        if (f.in_arg_token) {
            if (!f.seen_callee) f.seen_callee = true;
            f.in_arg_token = false;
        }
    };

    for (std::size_t i = 0; i < source.size(); ++i) {
        char c = source[i];

        if (c == '\n') {
            in_line_comment = false;
            in_string = false;
            for (auto& f : stack) end_token(f);
            ++line; col = 0;
            continue;
        }
        if (in_line_comment) { ++col; continue; }
        if (in_string) {
            if (c == '\\' && i + 1 < source.size()) { ++i; col += 2; continue; }
            if (c == '"') in_string = false;
            ++col; continue;
        }

        if (c == ';') { in_line_comment = true; ++col; continue; }
        if (c == '"') {
            in_string = true;
            for (auto& f : stack) end_token(f);
            ++col; continue;
        }

        if (c == '(') {
            if (!stack.empty()) {
                Frame& parent = stack.back();
                if (parent.seen_callee && !parent.in_arg_token) {
                    parent.site.args.push_back({line, col});
                    parent.in_arg_token = true;
                    ++parent.inner_depth;
                } else if (parent.seen_callee && parent.in_arg_token) {
                    ++parent.inner_depth;
                } else if (!parent.seen_callee) {
                    /// callee position is itself a sexp (e.g. ((f x) y))  -  skip
                    parent.in_arg_token = true; parent.seen_callee = true;
                    ++parent.inner_depth;
                }
            }
            stack.push_back(Frame{});
            ++col; continue;
        }
        if (c == ')') {
            if (!stack.empty()) {
                Frame f = std::move(stack.back());
                stack.pop_back();
                if (!f.site.callee.empty() && !f.site.args.empty()) {
                    out.push_back(std::move(f.site));
                }
                if (!stack.empty()) {
                    Frame& parent = stack.back();
                    if (parent.inner_depth > 0) --parent.inner_depth;
                    if (parent.inner_depth == 0) parent.in_arg_token = false;
                }
            }
            ++col; continue;
        }
        if (c == ' ' || c == '\t' || c == '\r') {
            if (!stack.empty()) {
                Frame& f = stack.back();
                if (f.inner_depth == 0) end_token(f);
            }
            ++col; continue;
        }

        if (!stack.empty()) {
            Frame& f = stack.back();
            if (f.inner_depth == 0) {
                if (!f.seen_callee) {
                    if (!f.in_arg_token) f.in_arg_token = true;
                    f.site.callee += c;
                } else if (!f.in_arg_token) {
                    f.site.args.push_back({line, col});
                    f.in_arg_token = true;
                }
            }
        }
        ++col;
    }
    return out;
}

} ///< anonymous namespace

Value LspServer::handle_inlay_hint(const Value& params) {
    auto uri = params["textDocument"].get_string("uri").value_or("");
    if (!uri.empty()) {
        ensure_workspace_for_uri(uri);
    }
    auto it = documents_.find(uri);
    if (it == documents_.end()) return Value(Array{});

    int64_t r_start_line = params["range"]["start"].get_int("line").value_or(0);
    int64_t r_start_char = params["range"]["start"].get_int("character").value_or(0);
    int64_t r_end_line   = params["range"]["end"].get_int("line").value_or(INT64_MAX);
    int64_t r_end_char   = params["range"]["end"].get_int("character").value_or(INT64_MAX);

    const auto& source = it->second.content;

    /// Build callee -> param-names index.  Document-local definitions take
    /// precedence over prelude / module-path so user shadowing wins.
    std::unordered_map<std::string, std::vector<std::string>> param_index;
    auto add_syms = [&](const std::vector<SymbolInfo>& syms) {
        for (const auto& s : syms) {
            if (s.signature.empty()) continue;
            if (param_index.count(s.name)) continue;
            auto names = parse_inlay_param_names(s.signature);
            if (!names.empty()) param_index[s.name] = std::move(names);
        }
    };

    auto local = collect_symbols(source, /*capture_signature=*/true);
    add_syms(local);
    if (!completion_cache_loaded_) load_completion_cache();
    add_syms(prelude_symbols_);
    add_syms(module_path_symbols_);

    auto in_range = [&](int64_t l, int64_t c) {
        if (l < r_start_line) return false;
        if (l == r_start_line && c < r_start_char) return false;
        if (l > r_end_line) return false;
        if (l == r_end_line && c > r_end_char) return false;
        return true;
    };

    Array result;
    auto sites = find_inlay_call_sites(source);
    for (const auto& site : sites) {
        auto pit = param_index.find(site.callee);
        if (pit == param_index.end()) continue;
        const auto& names = pit->second;
        if (names.empty()) continue;

        const std::size_t n = std::min(site.args.size(), names.size());
        for (std::size_t i = 0; i < n; ++i) {
            const auto& a = site.args[i];
            if (!in_range(a.line, a.character)) continue;
            result.push_back(json::object({
                {"position", json::object({
                    {"line",      a.line},
                    {"character", a.character},
                })},
                {"label",        names[i] + ":"},
                {"kind",         2},     ///< Parameter
                {"paddingRight", true},
            }));
        }
    }

    return Value(std::move(result));
}

} ///< namespace eta::lsp
