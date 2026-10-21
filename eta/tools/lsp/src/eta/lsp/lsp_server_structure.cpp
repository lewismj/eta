/// LSP folding, selection ranges, semantic tokens and formatting.
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

Value LspServer::handle_folding_range(const Value& params) {
    auto uri = params["textDocument"].get_string("uri").value_or("");
    auto it = documents_.find(uri);
    if (it == documents_.end()) return Value(Array{});

    const auto& source = it->second.content;

    /// Split into lines
    std::vector<std::string> lines;
    {
        std::istringstream iss(source);
        std::string ln;
        while (std::getline(iss, ln)) {
            if (!ln.empty() && ln.back() == '\r') ln.pop_back();
            lines.push_back(std::move(ln));
        }
    }

    Array result;

    /// Track paren-based folding: stack of (line, col) for each opening '('
    struct OpenParen { int64_t line; int64_t col; };
    std::vector<OpenParen> paren_stack;
    bool in_string = false;
    bool in_block_comment = false;
    int64_t block_comment_start = -1;

    for (int64_t row = 0; row < static_cast<int64_t>(lines.size()); ++row) {
        const auto& ln = lines[static_cast<std::size_t>(row)];
        for (std::size_t col = 0; col < ln.size(); ++col) {
            char c = ln[col];

            /// Block comment tracking
            if (in_block_comment) {
                if (c == '|' && col + 1 < ln.size() && ln[col + 1] == '#') {
                    in_block_comment = false;
                    if (block_comment_start >= 0 && row > block_comment_start) {
                        result.push_back(json::object({
                            {"startLine",      Value(block_comment_start)},
                            {"endLine",        Value(row)},
                            {"kind",           "comment"},
                        }));
                    }
                    ++col; ///< skip '#'
                }
                continue;
            }

            if (in_string) {
                if (c == '\\') { ++col; continue; }
                if (c == '"') in_string = false;
                continue;
            }

            /// Block comment start
            if (c == '#' && col + 1 < ln.size() && ln[col + 1] == '|') {
                in_block_comment = true;
                block_comment_start = row;
                ++col;
                continue;
            }

            if (c == '"') { in_string = true; continue; }
            if (c == ';') break; ///< rest of line is a comment

            if (c == '(') {
                paren_stack.push_back({row, static_cast<int64_t>(col)});
            } else if (c == ')') {
                if (!paren_stack.empty()) {
                    auto open = paren_stack.back();
                    paren_stack.pop_back();
                    /// Only fold if the form spans multiple lines
                    if (row > open.line) {
                        result.push_back(json::object({
                            {"startLine",      Value(open.line)},
                            {"startCharacter", Value(open.col)},
                            {"endLine",        Value(row)},
                            {"endCharacter",   Value(static_cast<int64_t>(col) + 1)},
                            {"kind",           "region"},
                        }));
                    }
                }
            }
        }
    }

    /// Fold consecutive line-comment blocks
    {
        int64_t comment_start = -1;
        for (int64_t row = 0; row < static_cast<int64_t>(lines.size()); ++row) {
            const auto& ln = lines[static_cast<std::size_t>(row)];
            auto first = ln.find_first_not_of(" \t");
            bool is_comment = (first != std::string::npos && ln[first] == ';');
            if (is_comment) {
                if (comment_start < 0) comment_start = row;
            } else {
                if (comment_start >= 0 && row - 1 > comment_start) {
                    result.push_back(json::object({
                        {"startLine", Value(comment_start)},
                        {"endLine",   Value(row - 1)},
                        {"kind",      "comment"},
                    }));
                }
                comment_start = -1;
            }
        }
        /// Handle trailing comment block
        if (comment_start >= 0 && static_cast<int64_t>(lines.size()) - 1 > comment_start) {
            result.push_back(json::object({
                {"startLine", Value(comment_start)},
                {"endLine",   Value(static_cast<int64_t>(lines.size()) - 1)},
                {"kind",      "comment"},
            }));
        }
    }

    return Value(std::move(result));
}

/**
 */

Value LspServer::handle_selection_range(const Value& params) {
    auto uri = params["textDocument"].get_string("uri").value_or("");
    auto it = documents_.find(uri);
    if (it == documents_.end()) return Value(Array{});

    const auto& source = it->second.content;
    const auto& positions = params["positions"];
    if (!positions.is_array()) return Value(Array{});

    Array result;
    for (const auto& pos : positions.as_array()) {
        auto line = pos.get_int("line").value_or(0);
        auto character = pos.get_int("character").value_or(0);

        auto ranges = enclosing_sexp_ranges(source, line, character);

        if (ranges.empty()) {
            auto word = word_at_position(source, line, character);
            Range wr{Position{line, character},
                     Position{line, character + static_cast<int64_t>(word.size())}};
            result.push_back(json::object({
                {"range", range_to_json(wr)},
            }));
            continue;
        }

        /**
         * Build nested selectionRange from innermost to outermost
         * The protocol wants innermost.parent = next outer, etc.
         */
        Value current(nullptr);
        for (auto rit = ranges.rbegin(); rit != ranges.rend(); ++rit) {
            if (current.is_null()) {
                current = json::object({{"range", range_to_json(*rit)}});
            } else {
                current = json::object({
                    {"range",  range_to_json(*rit)},
                    {"parent", std::move(current)},
                });
            }
        }
        result.push_back(std::move(current));
    }

    return Value(std::move(result));
}

/**
 */

std::vector<Range> LspServer::enclosing_sexp_ranges(
        const std::string& source, int64_t target_line, int64_t target_char) {
    /// Convert target position to a flat offset
    std::size_t target_offset = 0;
    {
        std::istringstream iss(source);
        std::string ln;
        int64_t row = 0;
        while (std::getline(iss, ln)) {
            if (!ln.empty() && ln.back() == '\r') ln.pop_back();
            if (row == target_line) {
                target_offset += static_cast<std::size_t>(std::min(target_char,
                    static_cast<int64_t>(ln.size())));
                break;
            }
            target_offset += ln.size() + 1; ///< +1 for newline
            ++row;
        }
    }

    /**
     * Scan source tracking parentheses; record open-paren offsets on a stack.
     * When we find a close-paren, if the range includes target_offset, record it.
     */
    struct OpenInfo { std::size_t offset; int64_t line; int64_t col; };
    std::vector<OpenInfo> stack;
    std::vector<Range> result; ///< innermost first

    bool in_string = false;
    int64_t row = 0;
    int64_t col = 0;

    for (std::size_t i = 0; i < source.size(); ++i) {
        char c = source[i];

        if (c == '\n') { ++row; col = 0; continue; }
        if (c == '\r') { continue; }

        if (in_string) {
            if (c == '\\') { ++i; ++col; ++col; continue; }
            if (c == '"') in_string = false;
            ++col;
            continue;
        }

        if (c == '"') { in_string = true; ++col; continue; }
        if (c == ';') {
            /// Skip to end of line
            while (i + 1 < source.size() && source[i + 1] != '\n') ++i;
            ++col;
            continue;
        }

        if (c == '(') {
            stack.push_back({i, row, col});
        } else if (c == ')') {
            if (!stack.empty()) {
                auto open = stack.back();
                stack.pop_back();
                /// Check if target_offset falls within [open.offset, i]
                if (target_offset >= open.offset && target_offset <= i) {
                    result.push_back(Range{
                        Position{open.line, open.col},
                        Position{row, col + 1},
                    });
                }
            }
        }
        ++col;
    }

    /// Sort innermost first (smallest range first)
    std::sort(result.begin(), result.end(), [](const Range& a, const Range& b) {
        auto a_size = (a.end.line - a.start.line) * 10000 + (a.end.character - a.start.character);
        auto b_size = (b.end.line - b.start.line) * 10000 + (b.end.character - b.start.character);
        return a_size < b_size;
    });

    return result;
}

/**
 */

Value LspServer::handle_semantic_tokens_full(const Value& params) {
    auto uri = params["textDocument"].get_string("uri").value_or("");
    auto it = documents_.find(uri);
    if (it == documents_.end()) return json::object({{"data", json::array({})}});

    const auto& source = it->second.content;

    /**
     * Token type indices (must match the legend in handle_initialize):
     * 0=keyword, 1=function, 2=variable, 3=string, 4=number,
     * 5=comment, 6=operator, 7=type, 8=macro, 9=parameter
     */
    constexpr int TT_KEYWORD  = 0;
    constexpr int TT_FUNCTION = 1;
    constexpr int TT_VARIABLE = 2;
    constexpr int TT_STRING   = 3;
    constexpr int TT_NUMBER   = 4;
    constexpr int TT_COMMENT  = 5;
    constexpr int TT_OPERATOR = 6;
    constexpr int TT_TYPE     = 7;
    constexpr int TT_MACRO    = 8;

    /// Token modifier bits: 0x1=declaration, 0x2=definition
    constexpr int TM_DEFINITION = 2;

    /// Collect defined symbols for classification
    auto symbols = collect_symbols(source, true);
    std::unordered_set<std::string> defun_names;
    std::unordered_set<std::string> macro_names;
    std::unordered_set<std::string> record_names;
    std::unordered_set<std::string> defined_names;
    for (const auto& sym : symbols) {
        if (sym.kind == "defun" || sym.kind == "function") defun_names.insert(sym.name);
        else if (sym.kind == "macro") macro_names.insert(sym.name);
        else if (sym.kind == "record") record_names.insert(sym.name);
        defined_names.insert(sym.name);
    }

    /// Also include prelude/module-path symbols
    if (!completion_cache_loaded_) load_completion_cache();
    for (const auto& sym : prelude_symbols_) {
        if (sym.kind == "defun" || sym.kind == "function") defun_names.insert(sym.name);
        else if (sym.kind == "macro") macro_names.insert(sym.name);
        else if (sym.kind == "record") record_names.insert(sym.name);
    }
    for (const auto& sym : module_path_symbols_) {
        if (sym.kind == "defun" || sym.kind == "function") defun_names.insert(sym.name);
        else if (sym.kind == "macro") macro_names.insert(sym.name);
        else if (sym.kind == "record") record_names.insert(sym.name);
    }

    static const std::unordered_set<std::string> keywords = {
        "define", "lambda", "if", "begin", "set!", "quote", "let", "let*",
        "letrec", "letrec*", "cond", "case", "and", "or", "when", "unless",
        "do", "module", "import", "export", "define-syntax", "syntax-rules",
        "define-record-type", "def", "defun", "progn", "quasiquote",
        "call/cc", "dynamic-wind", "values", "call-with-values", "apply",
        "raise", "catch", "logic-var", "unify", "deref-lvar", "trail-mark",
        "unwind-trail", "copy-term",
    };

    static const std::unordered_set<std::string> operators = {
        "+", "-", "*", "/", "=", "<", ">", "<=", ">=",
        "eq?", "eqv?", "equal?", "not",
    };

    /**
     * Process source line-by-line to handle comments, then use lexer for code
     * We'll scan manually to also capture comments (lexer skips them)
     */

    /// First pass: collect comment token positions
    struct SemanticToken {
        int64_t line;     ///< 0-based
        int64_t col;      ///< 0-based
        int64_t length;
        int type;
        int modifiers;
    };
    std::vector<SemanticToken> tokens;

    /// Scan for comments
    {
        std::istringstream iss(source);
        std::string ln;
        int64_t row = 0;
        bool in_block_comment = false;

        while (std::getline(iss, ln)) {
            if (!ln.empty() && ln.back() == '\r') ln.pop_back();

            for (std::size_t col = 0; col < ln.size(); ++col) {
                if (in_block_comment) {
                    if (ln[col] == '|' && col + 1 < ln.size() && ln[col + 1] == '#') {
                        in_block_comment = false;
                        ++col; ///< skip '#'
                    }
                    continue;
                }

                char c = ln[col];
                if (c == '#' && col + 1 < ln.size() && ln[col + 1] == '|') {
                    in_block_comment = true;
                    ++col;
                    continue;
                }
                if (c == '"') {
                    /// Skip strings
                    ++col;
                    while (col < ln.size()) {
                        if (ln[col] == '\\') { ++col; }
                        else if (ln[col] == '"') break;
                        ++col;
                    }
                    continue;
                }
                if (c == ';') {
                    /// Line comment from ';' to end of line
                    tokens.push_back({row, static_cast<int64_t>(col),
                                      static_cast<int64_t>(ln.size() - col),
                                      TT_COMMENT, 0});
                    break;
                }
            }
            ++row;
        }
    }

    /// Lexer pass for code tokens
    {
        reader::lexer::Lexer lex(0, source);
        while (true) {
            auto result = lex.next_token();
            if (!result) break;
            auto& tok = *result;
            if (tok.kind == reader::lexer::Token::Kind::EOF_) break;

            /// Lexer uses 1-based lines/columns; LSP uses 0-based
            int64_t tok_line = static_cast<int64_t>(tok.span.start.line) - 1;
            int64_t tok_col  = static_cast<int64_t>(tok.span.start.column) - 1;
            int64_t tok_end_col = static_cast<int64_t>(tok.span.end.column) - 1;
            int64_t tok_len = tok_end_col - tok_col;
            if (tok_len <= 0) tok_len = 1;

            int type = -1;
            int modifiers = 0;

            switch (tok.kind) {
                case reader::lexer::Token::Kind::String:
                    type = TT_STRING;
                    break;
                case reader::lexer::Token::Kind::Number:
                    type = TT_NUMBER;
                    break;
                case reader::lexer::Token::Kind::Boolean:
                    type = TT_KEYWORD;
                    break;
                case reader::lexer::Token::Kind::Symbol: {
                    auto* sv = std::get_if<std::string>(&tok.value);
                    if (!sv) break;
                    const auto& name = *sv;

                    if (keywords.count(name)) {
                        type = TT_KEYWORD;
                    } else if (operators.count(name)) {
                        type = TT_OPERATOR;
                    } else if (macro_names.count(name)) {
                        type = TT_MACRO;
                    } else if (record_names.count(name)) {
                        type = TT_TYPE;
                    } else if (defun_names.count(name)) {
                        type = TT_FUNCTION;
                    } else {
                        type = TT_VARIABLE;
                    }

                    /// Mark definitions
                    for (const auto& sym : symbols) {
                        if (sym.name == name && sym.line == tok_line &&
                            sym.character == tok_col) {
                            modifiers |= TM_DEFINITION;
                            break;
                        }
                    }
                    break;
                }
                default:
                    break;
            }

            if (type >= 0) {
                tokens.push_back({tok_line, tok_col, tok_len, type, modifiers});
            }
        }
    }

    /// Sort by position
    std::sort(tokens.begin(), tokens.end(), [](const SemanticToken& a, const SemanticToken& b) {
        return a.line < b.line || (a.line == b.line && a.col < b.col);
    });

    /// Encode as delta format
    Array data;
    int64_t prev_line = 0;
    int64_t prev_col = 0;
    for (const auto& t : tokens) {
        int64_t delta_line = t.line - prev_line;
        int64_t delta_col = (delta_line == 0) ? (t.col - prev_col) : t.col;
        data.push_back(Value(delta_line));
        data.push_back(Value(delta_col));
        data.push_back(Value(t.length));
        data.push_back(Value(static_cast<int64_t>(t.type)));
        data.push_back(Value(static_cast<int64_t>(t.modifiers)));
        prev_line = t.line;
        prev_col = t.col;
    }

    return json::object({{"data", Value(std::move(data))}});
}

/**
 */

Value LspServer::handle_formatting(const Value& params) {
    auto uri = params["textDocument"].get_string("uri").value_or("");
    auto it = documents_.find(uri);
    if (it == documents_.end()) return Value(Array{});

    /// The canonical style uses spaces; a tab request is rejected explicitly
    /// (§8.2) by returning no edits rather than emitting an unsupported layout.
    const auto& opts = params["options"];
    if (opts.is_object() && opts.has("insertSpaces") && opts["insertSpaces"].is_bool()
        && !opts["insertSpaces"].as_bool()) {
        return Value(Array{});
    }

    format::Options options;
    options.width = format_max_width_;
    options.indent = format_indent_;
    /// `tabSize` maps to the canonical indent; document-format options take
    /// precedence over the editor default where supplied (§8.2).
    if (auto tab = opts.get_int("tabSize"); tab && *tab > 0) {
        options.indent = static_cast<std::size_t>(*tab);
    }

    const auto& source = it->second.content;
    auto result = format::format_source(source, options);
    /// Malformed, unsupported or unsafe input yields no edits; the diagnostic
    /// channel still reports the underlying reason (§8.2).
    if (result.outcome == format::Outcome::Refused || !result.output) return Value(Array{});
    if (*result.output == source) return Value(Array{});

    cst::Source map(0, uri, source);
    auto end_pos = map.position(source.size(), position_encoding_);
    if (!end_pos) return Value(Array{});
    return json::array({
        json::object({
            {"range", range_to_json(Range{
                Position{0, 0},
                Position{static_cast<int64_t>(end_pos->line), static_cast<int64_t>(end_pos->character)},
            })},
            {"newText", *result.output},
        }),
    });
}

Value LspServer::handle_range_formatting(const Value& params) {
    auto uri = params["textDocument"].get_string("uri").value_or("");
    auto it = documents_.find(uri);
    if (it == documents_.end()) return Value(Array{});

    const auto& opts = params["options"];
    if (opts.is_object() && opts.has("insertSpaces") && opts["insertSpaces"].is_bool()
        && !opts["insertSpaces"].as_bool()) {
        return Value(Array{});
    }

    format::Options options;
    options.width = format_max_width_;
    options.indent = format_indent_;
    if (auto tab = opts.get_int("tabSize"); tab && *tab > 0) {
        options.indent = static_cast<std::size_t>(*tab);
    }

    const auto& source = it->second.content;

    /// Parse the whole document once. Range formatting never reparses a
    /// state-sensitive slice with fresh defaults (§8.2.1); malformed input is
    /// refused with no edits.
    try {
        auto parsed = cst::parse(source, cst::ReaderConfig{});
        if (!parsed || parsed->status != cst::Status::Valid || !parsed->file) return Value(Array{});
        const auto& file = *parsed->file;

        const auto& range = params["range"];
        const cst::SourcePosition start_pos{
            static_cast<std::size_t>(range["start"].get_int("line").value_or(0)),
            static_cast<std::size_t>(range["start"].get_int("character").value_or(0))};
        const cst::SourcePosition end_pos{
            static_cast<std::size_t>(range["end"].get_int("line").value_or(0)),
            static_cast<std::size_t>(range["end"].get_int("character").value_or(0))};
        auto range_begin = file.source->offset(start_pos, position_encoding_);
        auto range_end = file.source->offset(end_pos, position_encoding_);
        if (!range_begin || !range_end || *range_begin > *range_end) return Value(Array{});

        Array edits;
        /// Only whole top-level reader units that sit entirely inside the request
        /// and begin at column zero qualify. Each is formatted in isolation and
        /// replaces exactly its own concrete-token extent, leaving every
        /// surrounding byte (indentation, trailing comments, separators)
        /// untouched (§8.2.1).
        for (const auto root : file.roots) {
            const auto extent = linter::rules::node_extent(file, file.nodes[root]);
            if (extent.begin < *range_begin || extent.end > *range_end) continue;
            if (extent.begin != 0 && source[extent.begin - 1] != '\n') continue;

            std::string unit(source.substr(extent.begin, extent.end - extent.begin));
            auto formatted = format::format_source(unit, options);
            if (formatted.outcome == format::Outcome::Refused || !formatted.output) continue;
            if (*formatted.output == unit) continue;

            auto unit_start = file.source->position(extent.begin, position_encoding_);
            auto unit_end = file.source->position(extent.end, position_encoding_);
            if (!unit_start || !unit_end) continue;
            edits.push_back(json::object({
                {"range", range_to_json(Range{
                    Position{static_cast<int64_t>(unit_start->line), static_cast<int64_t>(unit_start->character)},
                    Position{static_cast<int64_t>(unit_end->line), static_cast<int64_t>(unit_end->character)},
                })},
                {"newText", *formatted.output},
            }));
        }
        return Value(std::move(edits));
    } catch (const std::exception&) {
        return Value(Array{});
    }
}
} ///< namespace eta::lsp
