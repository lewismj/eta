/// LSP text document sync (didOpen/didChange/didClose/didSave) and diagnostics.
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
 * Text document synchronization
 */

void LspServer::handle_did_open(const Value& params) {
    const auto& td = params["textDocument"];
    auto uri = td.get_string("uri").value_or("");
    auto lang = td.get_string("languageId").value_or("eta");
    auto version = td.get_int("version").value_or(0);
    auto text = td.get_string("text").value_or("");

    documents_[uri] = TextDocument{uri, lang, version, text};
    validate_document(uri);
}

void LspServer::handle_did_change(const Value& params) {
    const auto& td = params["textDocument"];
    auto uri = td.get_string("uri").value_or("");

    auto it = documents_.find(uri);
    if (it == documents_.end()) return;

    it->second.version = td.get_int("version").value_or(it->second.version);

    /**
     * Apply ordered content changes sequentially against the preceding version
     * (§8.2). A change with a `range` is an incremental edit resolved through the
     * negotiated position encoding; a change without one is a full replacement.
     * An unresolvable position desynchronises the document, so we clear stale
     * diagnostics and wait for the next full snapshot rather than guessing.
     */
    const auto& changes = params["contentChanges"];
    if (!changes.is_array()) return;
    for (const auto& change : changes.as_array()) {
        if (!change.has("range")) {
            if (auto text = change.get_string("text")) it->second.content = *text;
            continue;
        }
        const auto& range = change["range"];
        const auto& start = range["start"];
        const auto& end = range["end"];
        cst::Source snapshot(0, uri, it->second.content);
        const cst::SourcePosition start_pos{
            static_cast<std::size_t>(start.get_int("line").value_or(0)),
            static_cast<std::size_t>(start.get_int("character").value_or(0))};
        const cst::SourcePosition end_pos{
            static_cast<std::size_t>(end.get_int("line").value_or(0)),
            static_cast<std::size_t>(end.get_int("character").value_or(0))};
        auto begin = snapshot.offset(start_pos, position_encoding_);
        auto finish = snapshot.offset(end_pos, position_encoding_);
        if (!begin || !finish || *begin > *finish || *finish > it->second.content.size()) {
            last_validated_content_.erase(uri);
            publish_diagnostics(uri, {});
            return;
        }
        it->second.content.replace(*begin, *finish - *begin, change.get_string("text").value_or(""));
    }

    validate_document(uri);
}

void LspServer::handle_did_close(const Value& params) {
    auto uri = params["textDocument"].get_string("uri").value_or("");
    documents_.erase(uri);
    last_validated_content_.erase(uri); ///< clean up cache entry
    /// Clear diagnostics for closed document
    publish_diagnostics(uri, {});
}

void LspServer::handle_did_save(const Value& params) {
    auto uri = params["textDocument"].get_string("uri").value_or("");
    /// Re-validate on save; update stored content if the server sent it back
    auto text = params.get_string("text");
    if (text) {
        auto it = documents_.find(uri);
        if (it != documents_.end()) {
            it->second.content = *text;
        }
    }
    /// Invalidate validation and completion caches so file changes are reflected
    last_validated_content_.erase(uri);
    completion_cache_loaded_ = false;
    validate_document(uri);
}

/**
 * Diagnostics: Run the eta pipeline and collect errors
 */

void LspServer::validate_document(const std::string& uri) {
    auto it = documents_.find(uri);
    if (it == documents_.end()) return;

    ensure_workspace_for_uri(uri);
    publish_workspace_package_diagnostics(uri);

    const auto& source = it->second.content;

    /**
     * Skip if the content hasn't changed since the last successful validation run
     * (avoids re-running the full 4-phase pipeline on every keystroke for no-op edits).
     */
    auto& cached = last_validated_content_[uri];
    if (cached == source) return;
    cached = source;

    const auto document_path = uri_to_path(uri);
    auto diags = compute_document_diagnostics(
        source, document_path ? document_path->parent_path() : std::filesystem::path{});
    publish_diagnostics(uri, diags);
}

std::vector<LspDiagnostic> LspServer::compute_document_diagnostics(
    const std::string& source,
    const std::filesystem::path& importer_dir) {
    std::vector<LspDiagnostic> diags;

    /// Shared linter findings (§5.1) are merged with compiler diagnostics by the
    /// single publisher below. Lint rules never run on reader-invalid input, so
    /// the compiler pipeline remains the owner of syntax/semantic errors.
    collect_lint_diagnostics(source, diags);

    /// Lex + Parse
    reader::lexer::Lexer lex(0, source);
    reader::parser::Parser parser(lex);

    auto parsed_res = parser.parse_toplevel();
    if (!parsed_res) {
        auto& err = parsed_res.error();
        std::visit([&](auto&& e) {
            using T = std::decay_t<decltype(e)>;
            LspDiagnostic d;
            d.severity = 1; ///< Error
            if constexpr (std::is_same_v<T, reader::lexer::LexError>) {
                d.range = span_to_range(e.span.start.line, e.span.start.column,
                                        e.span.end.line, e.span.end.column);
                d.message = e.message.empty()
                    ? std::string("lex error: ") + reader::lexer::to_string(e.kind)
                    : e.message;
            } else {
                d.range = span_to_range(e.span.start.line, e.span.start.column,
                                        e.span.end.line, e.span.end.column);
                d.message = std::string("parse error: ") + reader::parser::to_string(e.kind);
            }
            diags.push_back(std::move(d));
        }, err);
        return diags;
    }

    auto& parsed = *parsed_res;
    if (parsed.empty()) {
        return diags;
    }

    /// Expand
    reader::expander::Expander expander;
    auto expanded_res = expander.expand_many(parsed);
    if (!expanded_res) {
        const auto& err = expanded_res.error();
        LspDiagnostic d;
        d.severity = 1;
        d.range = span_to_range(err.span.start.line, err.span.start.column,
                                err.span.end.line, err.span.end.column);
        d.message = err.message.empty()
            ? std::string("expand error: ") + reader::expander::to_string(err.kind)
            : err.message;
        diags.push_back(std::move(d));
        return diags;
    }

    /**
     * Link
     * Move expanded forms out (SExprPtr = unique_ptr, not copyable).
     * Load any external modules required by (import ...) forms so the
     * linker can resolve cross-module references without false diagnostics.
     */
    auto all_forms = std::move(*expanded_res);
    std::unordered_map<std::string, std::vector<std::string>> compiled_module_exports;
    {
        using namespace reader::utils;
        std::unordered_set<std::string> seen_modules;
        for (const auto& f : all_forms) {
            const auto* mod = as_list(f);
            if (!mod || mod->elems.size() < 2) continue;
            if (!is_symbol_named(mod->elems[0], "module")) continue;
            if (const auto* name_sym = as_symbol(mod->elems[1]))
                seen_modules.insert(name_sym->name);
        }
        preload_module_deps(
            all_forms, seen_modules, compiled_module_exports, importer_dir);
    }

    reader::ModuleLinker linker;
    auto idx_res = linker.index_modules(all_forms);
    if (!idx_res) {
        const auto& err = idx_res.error();
        LspDiagnostic d;
        d.severity = 1;
        d.range = span_to_range(err.span.start.line, err.span.start.column,
                                err.span.end.line, err.span.end.column);
        d.message = err.message;
        diags.push_back(std::move(d));
        return diags;
    }

    for (const auto& [module_name, exports] : compiled_module_exports) {
        auto compiled_idx_res = linker.index_compiled_module_exports(
            module_name,
            std::span<const std::string>(exports.data(), exports.size()));
        if (!compiled_idx_res) {
            const auto& err = compiled_idx_res.error();
            LspDiagnostic d;
            d.severity = 1;
            d.range = span_to_range(err.span.start.line, err.span.start.column,
                                    err.span.end.line, err.span.end.column);
            d.message = err.message;
            diags.push_back(std::move(d));
            return diags;
        }
    }

    auto link_res = linker.link();
    if (!link_res) {
        const auto& err = link_res.error();
        LspDiagnostic d;
        d.severity = 1;
        d.range = span_to_range(err.span.start.line, err.span.start.column,
                                err.span.end.line, err.span.end.column);
        d.message = err.message;
        diags.push_back(std::move(d));
        return diags;
    }

    /// Semantic Analysis
    semantics::SemanticAnalyzer sa;
    runtime::BuiltinEnvironment builtins;
    runtime::register_builtin_specs(builtins);
    auto sem_res = sa.analyze_all(all_forms, linker, builtins);
    if (!sem_res) {
        const auto& err = sem_res.error();
        LspDiagnostic d;
        d.severity = 1;
        d.range = span_to_range(err.span.start.line, err.span.start.column,
                                err.span.end.line, err.span.end.column);
        d.message = err.message;
        diags.push_back(std::move(d));
        return diags;
    }

    return diags;
}

void LspServer::collect_lint_diagnostics(const std::string& source, std::vector<LspDiagnostic>& diags) {
    if (!lint_enabled_) return;

    linter::LintOptions options;
    options.preset = "recommended";
    auto rules = linter::resolve_rules(options);
    if (!rules) return;

    auto report = linter::lint_source(source, "<document>", *rules, options);
    if (report.diagnostics.empty()) return;

    cst::Source map(0, "<document>", source);
    for (const auto& finding : report.diagnostics) {
        /// Reader/parse failures are owned by the compiler pipeline; skip them so
        /// a single syntax error is not reported twice (§5.1).
        if (finding.owner != "eta_lint" || !finding.located) continue;
        auto start = map.position(finding.span.begin, position_encoding_);
        auto end = map.position(finding.span.end, position_encoding_);
        if (!start || !end) continue;
        LspDiagnostic d;
        d.range = Range{
            Position{static_cast<int64_t>(start->line), static_cast<int64_t>(start->character)},
            Position{static_cast<int64_t>(end->line), static_cast<int64_t>(end->character)}};
        switch (finding.severity) {
            case linter::Severity::Error: d.severity = 1; break;
            case linter::Severity::Warning: d.severity = 2; break;
            case linter::Severity::Info: d.severity = 3; break;
        }
        d.source = "eta_lint";
        d.message = finding.code + ": " + finding.message;
        diags.push_back(std::move(d));
    }
}

void LspServer::publish_diagnostics(const std::string& uri,
                                     const std::vector<LspDiagnostic>& diags) {
    Array diag_array;
    diag_array.reserve(diags.size());
    for (const auto& d : diags) {
        diag_array.push_back(diagnostic_to_json(d));
    }

    /// Tag the set with the document version so the client can discard results
    /// that race a newer edit (§8.2).
    Object payload{
        {"uri", uri},
        {"diagnostics", Value(std::move(diag_array))},
    };
    if (auto it = documents_.find(uri); it != documents_.end()) {
        payload.insert_or_assign("version", Value(it->second.version));
    }
    send_notification("textDocument/publishDiagnostics", Value(std::move(payload)));
}
} ///< namespace eta::lsp
