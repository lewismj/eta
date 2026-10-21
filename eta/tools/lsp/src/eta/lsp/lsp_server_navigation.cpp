/// LSP hover, definition, references, rename, highlights and document/workspace symbols.
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

Value LspServer::handle_hover(const Value& params) {
    auto uri = params["textDocument"].get_string("uri").value_or("");
    auto it = documents_.find(uri);
    if (it == documents_.end()) return Value(nullptr);

    auto line = params["position"].get_int("line").value_or(0);
    auto character = params["position"].get_int("character").value_or(0);

    const auto& source = it->second.content;
    auto word = word_at_position(source, line, character);
    if (word.empty()) return Value(nullptr);

    if (auto entry = eta::reader::lookup_special_form_doc(word)) {
        return json::object({
            {"contents", json::object({
                {"kind", "markdown"},
                {"value", eta::docs::render_markdown(*entry)},
            })},
        });
    }
    if (auto builtin = eta::runtime::lookup_builtin_metadata(word)) {
        return json::object({
            {"contents", json::object({
                {"kind", "markdown"},
                {"value", eta::docs::render_builtin_markdown(*builtin)},
            })},
        });
    }
    if (auto stdlib_doc = eta::docs::lookup_stdlib_doc(word)) {
        return json::object({
            {"contents", json::object({
                {"kind", "markdown"},
                {"value", eta::docs::render_markdown(*stdlib_doc)},
            })},
        });
    }

    /// Check document-local definitions
    auto symbols = collect_symbols(source);
    for (const auto& sym : symbols) {
        if (sym.name == word) {
            std::string doc = "**" + sym.name + "**  -  " + sym.kind +
                              " (line " + std::to_string(sym.line + 1) + ")";
            return json::object({
                {"contents", json::object({
                    {"kind", "markdown"},
                    {"value", doc},
                })},
            });
        }
    }

    return Value(nullptr);
}

/**
 */

Value LspServer::handle_definition(const Value& params) {
    auto uri = params["textDocument"].get_string("uri").value_or("");
    if (!uri.empty()) {
        ensure_workspace_for_uri(uri);
    }
    auto it = documents_.find(uri);
    if (it == documents_.end()) return Value(nullptr);

    auto line = params["position"].get_int("line").value_or(0);
    auto character = params["position"].get_int("character").value_or(0);

    const auto& source = it->second.content;
    auto word = word_at_position(source, line, character);
    if (word.empty()) return Value(nullptr);

    /// 1. Search document-local symbols first
    auto symbols = collect_symbols(source);
    for (const auto& sym : symbols) {
        if (sym.name == word) {
            return json::object({
                {"uri", uri},
                {"range", range_to_json(Range{
                    Position{sym.line, sym.character},
                    Position{sym.line, sym.character + static_cast<int64_t>(sym.name.size())},
                })},
            });
        }
    }

    /// 2. Lazily load the completion cache so prelude/module symbols are available
    if (!completion_cache_loaded_) {
        load_completion_cache();
    }

    /// 3. Search prelude symbols
    for (const auto& sym : prelude_symbols_) {
        if (sym.name == word && !sym.file_path.empty()) {
            return json::object({
                {"uri", path_to_uri(sym.file_path)},
                {"range", range_to_json(Range{
                    Position{sym.line, sym.character},
                    Position{sym.line, sym.character + static_cast<int64_t>(sym.name.size())},
                })},
            });
        }
    }

    /// 4. Search module-path symbols
    for (const auto& sym : module_path_symbols_) {
        if (sym.name == word && !sym.file_path.empty()) {
            return json::object({
                {"uri", path_to_uri(sym.file_path)},
                {"range", range_to_json(Range{
                    Position{sym.line, sym.character},
                    Position{sym.line, sym.character + static_cast<int64_t>(sym.name.size())},
                })},
            });
        }
    }

    return Value(nullptr);
}

/**
 * textDocument/documentSymbol
 */

Value LspServer::handle_document_symbol(const Value& params) {
    auto uri = params["textDocument"].get_string("uri").value_or("");
    auto it = documents_.find(uri);
    if (it == documents_.end()) return Value(Array{});

    const auto& source = it->second.content;
    auto symbols = collect_symbols(source, /*capture_signature=*/true);

    /// Pre-split source into lines to find the opening '(' of each form.
    std::vector<std::string> lines;
    {
        std::istringstream iss(source);
        std::string ln;
        while (std::getline(iss, ln)) {
            if (!ln.empty() && ln.back() == '\r') ln.pop_back();
            lines.push_back(std::move(ln));
        }
    }

    /// LSP SymbolKind values
    constexpr int SK_Module   = 2;
    constexpr int SK_Function = 12;
    constexpr int SK_Variable = 13;
    constexpr int SK_Class    = 5;  ///< for record types

    Array result;
    for (const auto& sym : symbols) {
        int kind = SK_Variable;
        if (sym.kind == "defun" || sym.kind == "function") kind = SK_Function;
        else if (sym.kind == "module") kind = SK_Module;
        else if (sym.kind == "macro") kind = SK_Function;
        else if (sym.kind == "record") kind = SK_Class;

        std::string detail = sym.kind;
        if (!sym.signature.empty()) detail += " " + sym.signature;

        /// Selection range = just the symbol name
        auto name_end_char = sym.character + static_cast<int64_t>(sym.name.size());
        auto sel_range = Range{
            Position{sym.line, sym.character},
            Position{sym.line, name_end_char},
        };

        /**
         * Full range: scan backward from the symbol name to find the '(' that opens
         * this form, then use sexp_end to find the matching ')'.
         */
        int64_t form_col = sym.character;
        if (sym.line < static_cast<int64_t>(lines.size())) {
            const auto& ln = lines[static_cast<std::size_t>(sym.line)];
            for (int64_t c = sym.character - 1; c >= 0; --c) {
                if (ln[static_cast<std::size_t>(c)] == '(') { form_col = c; break; }
            }
        }
        auto [end_line, end_col] = sexp_end(source, sym.line, form_col);
        auto full_range = Range{
            Position{sym.line, form_col},
            Position{end_line, end_col},
        };
        /// Fallback: if sexp_end didn't advance, use the selection range
        if (end_line == sym.line && end_col == form_col) {
            full_range = sel_range;
        }

        result.push_back(json::object({
            {"name", sym.name},
            {"detail", detail},
            {"kind", kind},
            {"range", range_to_json(full_range)},
            {"selectionRange", range_to_json(sel_range)},
        }));
    }

    return Value(std::move(result));
}

/**
 * textDocument/references
 */

std::vector<Range> LspServer::find_all_occurrences(
        const std::string& source, const std::string& symbol) {
    std::vector<Range> result;
    if (symbol.empty()) return result;

    auto is_sym_char = [](char c) -> bool {
        if (c <= ' ') return false;
        if (c == '(' || c == ')' || c == '[' || c == ']' ||
            c == '"' || c == ';' || c == '#') return false;
        return true;
    };

    std::istringstream iss(source);
    std::string line;
    int64_t line_num = 0;

    while (std::getline(iss, line)) {
        std::size_t pos = 0;
        while (pos < line.size()) {
            auto found = line.find(symbol, pos);
            if (found == std::string::npos) break;

            /// Check word boundaries
            bool left_ok = (found == 0) || !is_sym_char(line[found - 1]);
            auto end_pos = found + symbol.size();
            bool right_ok = (end_pos >= line.size()) || !is_sym_char(line[end_pos]);

            if (left_ok && right_ok) {
                result.push_back(Range{
                    Position{line_num, static_cast<int64_t>(found)},
                    Position{line_num, static_cast<int64_t>(end_pos)},
                });
            }
            pos = found + 1;
        }
        ++line_num;
    }
    return result;
}

Value LspServer::handle_references(const Value& params) {
    auto uri = params["textDocument"].get_string("uri").value_or("");
    auto it = documents_.find(uri);
    if (it == documents_.end()) return Value(Array{});

    auto line = params["position"].get_int("line").value_or(0);
    auto character = params["position"].get_int("character").value_or(0);

    const auto& source = it->second.content;
    auto word = word_at_position(source, line, character);
    if (word.empty()) return Value(Array{});

    Array result;

    /// 1. Current document
    auto occurrences = find_all_occurrences(source, word);
    for (const auto& r : occurrences) {
        result.push_back(json::object({
            {"uri", uri},
            {"range", range_to_json(r)},
        }));
    }

    /// 2. Other open documents
    for (const auto& [doc_uri, doc] : documents_) {
        if (doc_uri == uri) continue;
        auto occ = find_all_occurrences(doc.content, word);
        for (const auto& r : occ) {
            result.push_back(json::object({
                {"uri", doc_uri},
                {"range", range_to_json(r)},
            }));
        }
    }

    /// 3. Module-path files (prelude + module path)
    if (!completion_cache_loaded_) {
        load_completion_cache();
    }

    /// Collect unique file paths from cached symbols that match the word
    std::unordered_set<std::string> scanned_files;
    /// Track URIs we've already added results for
    scanned_files.insert(uri);
    for (const auto& [doc_uri, doc] : documents_) {
        scanned_files.insert(doc_uri);
    }

    auto scan_cached = [&](const std::vector<SymbolInfo>& syms) {
        /**
         * prevents false-positive matches in prelude/stdlib files for symbols
         * (e.g. a local variable "x") that are not defined there.
         */
        std::unordered_set<std::string> files_with_symbol;
        for (const auto& sym : syms) {
            if (sym.name == word && !sym.file_path.empty())
                files_with_symbol.insert(sym.file_path);
        }
        if (files_with_symbol.empty()) return;

        for (const auto& sym : syms) {
            if (sym.file_path.empty()) continue;
            if (!files_with_symbol.count(sym.file_path)) continue;
            auto file_uri = path_to_uri(sym.file_path);
            if (!scanned_files.insert(file_uri).second) continue;
            /// Read and scan the file
            std::ifstream f(sym.file_path);
            if (!f.is_open()) continue;
            std::string src(std::istreambuf_iterator<char>(f),
                            std::istreambuf_iterator<char>{});
            auto occ = find_all_occurrences(src, word);
            for (const auto& r : occ) {
                result.push_back(json::object({
                    {"uri", file_uri},
                    {"range", range_to_json(r)},
                }));
            }
        }
    };

    scan_cached(prelude_symbols_);
    scan_cached(module_path_symbols_);

    return Value(std::move(result));
}

/**
 * textDocument/rename
 */

Value LspServer::handle_rename(const Value& params) {
    auto uri = params["textDocument"].get_string("uri").value_or("");
    auto it = documents_.find(uri);
    if (it == documents_.end()) return Value(nullptr);

    auto line = params["position"].get_int("line").value_or(0);
    auto character = params["position"].get_int("character").value_or(0);
    auto new_name = params.get_string("newName").value_or("");
    if (new_name.empty()) return Value(nullptr);

    const auto& source = it->second.content;
    auto word = word_at_position(source, line, character);
    if (word.empty()) return Value(nullptr);

    auto occurrences = find_all_occurrences(source, word);
    if (occurrences.empty()) return Value(nullptr);

    Array edits;
    for (const auto& r : occurrences) {
        edits.push_back(json::object({
            {"range", range_to_json(r)},
            {"newText", new_name},
        }));
    }

    /// WorkspaceEdit with changes map
    return json::object({
        {"changes", json::object({
            {uri, Value(std::move(edits))},
        })},
    });
}

/**
 */

Value LspServer::handle_workspace_symbol(const Value& params) {
    auto query_opt = params.get_string("query");
    std::string query = query_opt ? *query_opt : "";

    if (!documents_.empty()) {
        ensure_workspace_for_uri(documents_.begin()->first);
    }

    if (!completion_cache_loaded_) {
        load_completion_cache();
    }

    /// LSP SymbolKind values
    constexpr int SK_Module   = 2;
    constexpr int SK_Function = 12;
    constexpr int SK_Variable = 13;
    constexpr int SK_Class    = 5;

    auto kind_for = [&](const std::string& k) -> int {
        if (k == "defun" || k == "function") return SK_Function;
        if (k == "module") return SK_Module;
        if (k == "macro") return SK_Function;
        if (k == "record") return SK_Class;
        return SK_Variable;
    };

    auto matches = [&](const std::string& name) -> bool {
        if (query.empty()) return true;
        /// Case-insensitive substring match
        auto it = std::search(name.begin(), name.end(), query.begin(), query.end(),
            [](char a, char b) {
                return std::tolower(static_cast<unsigned char>(a)) ==
                       std::tolower(static_cast<unsigned char>(b));
            });
        return it != name.end();
    };

    Array result;
    constexpr std::size_t MAX_RESULTS = 200;

    /// Search open documents
    for (const auto& [doc_uri, doc] : documents_) {
        if (result.size() >= MAX_RESULTS) break;
        auto syms = collect_symbols(doc.content, true);
        for (const auto& sym : syms) {
            if (result.size() >= MAX_RESULTS) break;
            if (!matches(sym.name)) continue;
            result.push_back(json::object({
                {"name", sym.name},
                {"kind", kind_for(sym.kind)},
                {"location", json::object({
                    {"uri", doc_uri},
                    {"range", range_to_json(Range{
                        Position{sym.line, sym.character},
                        Position{sym.line, sym.character + static_cast<int64_t>(sym.name.size())},
                    })},
                })},
                {"containerName", sym.module_name.empty() ? Value(nullptr) : Value(sym.module_name)},
            }));
        }
    }

    /// Search prelude symbols
    for (const auto& sym : prelude_symbols_) {
        if (result.size() >= MAX_RESULTS) break;
        if (!matches(sym.name)) continue;
        std::string sym_uri = sym.file_path.empty() ? "" : path_to_uri(sym.file_path);
        result.push_back(json::object({
            {"name", sym.name},
            {"kind", kind_for(sym.kind)},
            {"location", json::object({
                {"uri", sym_uri},
                {"range", range_to_json(Range{
                    Position{sym.line, sym.character},
                    Position{sym.line, sym.character + static_cast<int64_t>(sym.name.size())},
                })},
            })},
            {"containerName", sym.module_name.empty() ? Value(nullptr) : Value(sym.module_name)},
        }));
    }

    /// Search module-path symbols
    for (const auto& sym : module_path_symbols_) {
        if (result.size() >= MAX_RESULTS) break;
        if (!matches(sym.name)) continue;
        std::string sym_uri = sym.file_path.empty() ? "" : path_to_uri(sym.file_path);
        result.push_back(json::object({
            {"name", sym.name},
            {"kind", kind_for(sym.kind)},
            {"location", json::object({
                {"uri", sym_uri},
                {"range", range_to_json(Range{
                    Position{sym.line, sym.character},
                    Position{sym.line, sym.character + static_cast<int64_t>(sym.name.size())},
                })},
            })},
            {"containerName", sym.module_name.empty() ? Value(nullptr) : Value(sym.module_name)},
        }));
    }

    return Value(std::move(result));
}

/**
 * collect_symbols
 */

std::vector<LspServer::SymbolInfo> LspServer::collect_symbols(const std::string& source, bool capture_signature) {
    std::vector<SymbolInfo> result;

    /// Quick scan for definition forms
    std::istringstream iss(source);
    std::string line;
    int64_t line_num = 0;

    while (std::getline(iss, line)) {
        /// Skip comment-only lines
        auto first_non_ws = line.find_first_not_of(" \t");
        if (first_non_ws != std::string::npos && line[first_non_ws] == ';') {
            ++line_num;
            continue;
        }

        auto try_extract = [&](const std::string& keyword, const std::string& kind) {
            std::string pat1 = "(" + keyword + " ";
            std::string pat2 = "(" + keyword + "\t";
            auto pos = line.find(pat1);
            if (pos == std::string::npos) pos = line.find(pat2);
            if (pos == std::string::npos) return;

            auto name_start = pos + 1 + keyword.size() + 1; ///< skip '(' + keyword + space
            /// Skip whitespace
            while (name_start < line.size() &&
                   (line[name_start] == ' ' || line[name_start] == '\t'))
                ++name_start;
            if (name_start >= line.size()) return;

            /// If the name starts with '(', extract first symbol inside
            auto actual_start = name_start;
            bool had_paren = false;
            if (line[actual_start] == '(') {
                ++actual_start;
                had_paren = true;
                while (actual_start < line.size() &&
                       (line[actual_start] == ' ' || line[actual_start] == '\t'))
                    ++actual_start;
            }

            auto name_end = actual_start;
            while (name_end < line.size()) {
                char c = line[name_end];
                if (c == ' ' || c == '\t' || c == ')' || c == '(' ||
                    c == '\n' || c == '\r')
                    break;
                ++name_end;
            }

            if (name_end > actual_start) {
                SymbolInfo sym;
                sym.name = line.substr(actual_start, name_end - actual_start);
                sym.kind = kind;
                sym.line = line_num;
                sym.character = static_cast<int64_t>(actual_start);

                /// Capture signature: for defun/define with (name args...) form
                if (capture_signature && (kind == "defun" || kind == "define")) {
                    /**
                     * For defun: (defun name (args...) body...)
                     * Signature is the next (...) after name
                     */
                    if (kind == "defun" && !had_paren) {
                        auto sig_start = name_end;
                        while (sig_start < line.size() && line[sig_start] == ' ') ++sig_start;
                        if (sig_start < line.size() && line[sig_start] == '(') {
                            int depth = 0;
                            auto sig_end = sig_start;
                            for (; sig_end < line.size(); ++sig_end) {
                                if (line[sig_end] == '(') ++depth;
                                else if (line[sig_end] == ')') { --depth; if (depth == 0) { ++sig_end; break; } }
                            }
                            sym.signature = line.substr(sig_start, sig_end - sig_start);
                        }
                    }
                    if (kind == "define" && had_paren) {
                        auto sig_start = name_end;
                        /// Capture everything up to the closing ')'
                        auto sig_end = sig_start;
                        while (sig_end < line.size() && line[sig_end] != ')') ++sig_end;
                        if (sig_end > sig_start) {
                            sym.signature = "(" + line.substr(sig_start, sig_end - sig_start) + ")";
                        }
                    }
                }

                result.push_back(std::move(sym));
            }
        };

        try_extract("define-syntax", "macro");
        try_extract("define-record-type", "record");
        try_extract("define", "define");
        try_extract("defun", "defun");
        try_extract("def", "define");
        try_extract("module", "module");

        ++line_num;
    }

    return result;
}

/**
 * textDocument/documentHighlight
 *
 * Returns DocumentHighlight ranges for every occurrence of the symbol at
 * the requested position in the current document.  Kind is advertised as
 * Text (1) since the parser does not currently distinguish read-vs-write
 * usages.
 */
Value LspServer::handle_document_highlight(const Value& params) {
    auto uri = params["textDocument"].get_string("uri").value_or("");
    auto it = documents_.find(uri);
    if (it == documents_.end()) return Value(Array{});

    auto line      = params["position"].get_int("line").value_or(0);
    auto character = params["position"].get_int("character").value_or(0);

    const auto& source = it->second.content;
    auto word = word_at_position(source, line, character);
    if (word.empty()) return Value(Array{});

    auto occurrences = find_all_occurrences(source, word);
    Array result;
    for (const auto& r : occurrences) {
        result.push_back(json::object({
            {"range", range_to_json(r)},
            {"kind",  1}, ///< Text
        }));
    }
    return Value(std::move(result));
}
} ///< namespace eta::lsp
