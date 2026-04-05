#include "lsp_server.h"

#include <algorithm>
#include <cassert>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <unordered_set>
#include <variant>

#ifdef _WIN32
#include <windows.h>
#endif

// Eta compiler includes
#include "eta/reader/lexer.h"
#include "eta/reader/parser.h"
#include "eta/reader/expander.h"
#include "eta/reader/module_linker.h"
#include "eta/reader/sexpr_utils.h"
#include "eta/semantics/semantic_analyzer.h"
#include "eta/runtime/builtin_env.h"
#include "eta/runtime/builtin_names.h"

namespace eta::lsp {

using namespace json;

// ============================================================================
// Construction
// ============================================================================

LspServer::LspServer() {
    init_module_path();
}

// ============================================================================
// Module path resolution
// ============================================================================

void LspServer::init_module_path() {
    namespace fs = std::filesystem;

    // 1. Parse ETA_MODULE_PATH env var (colon/semicolon-delimited)
#ifdef _WIN32
#pragma warning(suppress: 4996)  // getenv is safe here; we copy immediately
#endif
    const char* env = std::getenv("ETA_MODULE_PATH");
    if (env && env[0] != '\0') {
        std::string_view path_str(env);
#ifdef _WIN32
        constexpr char sep = ';';
#else
        constexpr char sep = ':';
#endif
        std::size_t start = 0;
        while (start < path_str.size()) {
            auto pos = path_str.find(sep, start);
            if (pos == std::string_view::npos) pos = path_str.size();
            auto part = std::string(path_str.substr(start, pos - start));
            if (!part.empty()) module_search_dirs_.emplace_back(part);
            start = pos + 1;
        }
    }

    // 2. Append stdlib bundled alongside the executable (<prefix>/bin/ → <prefix>/stdlib/)
    std::error_code ec;
#ifdef _WIN32
    wchar_t buf[4096];
    DWORD len = GetModuleFileNameW(nullptr, buf, static_cast<DWORD>(std::size(buf)));
    if (len > 0 && len < static_cast<DWORD>(std::size(buf))) {
        auto stdlib = fs::path(buf).parent_path().parent_path() / "stdlib";
        if (fs::is_directory(stdlib, ec)) module_search_dirs_.push_back(std::move(stdlib));
    }
#else
    fs::path exe = fs::read_symlink("/proc/self/exe", ec);
    if (!ec) {
        auto stdlib = exe.parent_path().parent_path() / "stdlib";
        if (fs::is_directory(stdlib, ec)) module_search_dirs_.push_back(std::move(stdlib));
    }
#endif
}

std::optional<std::string> LspServer::resolve_module_source(const std::string& module_name) {
    namespace fs = std::filesystem;
    // "std.core" → "std/core.eta"
    std::string rel = module_name;
    for (auto& c : rel) { if (c == '.') c = '/'; }
    rel += ".eta";

    for (const auto& dir : module_search_dirs_) {
        auto candidate = dir / rel;
        std::error_code ec;
        if (fs::is_regular_file(candidate, ec)) {
            std::ifstream f(candidate);
            if (!f.is_open()) continue;
            return std::string(std::istreambuf_iterator<char>(f),
                               std::istreambuf_iterator<char>());
        }
    }
    return std::nullopt;
}

void LspServer::preload_module_deps(
        std::vector<eta::reader::parser::SExprPtr>& all_forms,
        std::unordered_set<std::string>& seen_modules) {
    using namespace reader::utils;

    // Collect imported module names from a form list
    auto collect_imports = [&](const std::vector<eta::reader::parser::SExprPtr>& forms,
                                std::vector<std::string>& out) {
        for (const auto& f : forms) {
            const auto* mod = as_list(f);
            if (!mod || mod->elems.size() < 2) continue;
            if (!is_symbol_named(mod->elems[0], "module")) continue;

            for (std::size_t i = 2; i < mod->elems.size(); ++i) {
                const auto* inner = as_list(mod->elems[i]);
                if (!inner || inner->elems.empty()) continue;
                if (!is_symbol_named(inner->elems[0], "import")) continue;

                for (std::size_t j = 1; j < inner->elems.size(); ++j) {
                    const auto& clause = inner->elems[j];
                    std::string mod_name;
                    if (const auto* s = as_symbol(clause)) {
                        mod_name = s->name;
                    } else if (const auto* lst = as_list(clause)) {
                        // (only mod ...) / (except mod ...) / (rename mod ...) / (prefix mod pfx)
                        if (lst->elems.size() >= 2) {
                            if (const auto* mod_sym = as_symbol(lst->elems[1])) mod_name = mod_sym->name;
                        }
                    }
                    if (!mod_name.empty() && !seen_modules.count(mod_name))
                        out.push_back(mod_name);
                }
            }
        }
    };

    std::vector<std::string> to_load;
    collect_imports(all_forms, to_load);

    while (!to_load.empty()) {
        std::vector<std::string> next_load;
        for (const auto& mod_name : to_load) {
            if (seen_modules.count(mod_name)) continue;
            seen_modules.insert(mod_name);

            auto src = resolve_module_source(mod_name);
            if (!src) continue; // not on disk — let linker emit the diagnostic

            reader::lexer::Lexer lex(0, *src);
            reader::parser::Parser parser(lex);
            auto parsed = parser.parse_toplevel();
            if (!parsed) continue;

            reader::expander::Expander expander;
            auto expanded = expander.expand_many(*parsed);
            if (!expanded) continue;

            // Collect transitive imports before moving forms
            collect_imports(*expanded, next_load);

            for (auto& f : *expanded) all_forms.push_back(std::move(f));
        }
        to_load = std::move(next_load);
    }
}

// ============================================================================
// Transport: Header-Content framing over stdio
// ============================================================================

std::optional<std::string> LspServer::read_message() {
    // Read headers until blank line
    std::size_t content_length = 0;
    std::string header_line;
    while (std::getline(std::cin, header_line)) {
        // Strip \r if present
        if (!header_line.empty() && header_line.back() == '\r')
            header_line.pop_back();
        if (header_line.empty()) break; // end of headers

        // Parse Content-Length
        const std::string prefix = "Content-Length: ";
        if (header_line.substr(0, prefix.size()) == prefix) {
            content_length = std::stoull(header_line.substr(prefix.size()));
        }
        // Ignore other headers (Content-Type, etc.)
    }

    if (std::cin.eof() || std::cin.fail()) return std::nullopt;
    if (content_length == 0) return std::nullopt;

    // Read exactly content_length bytes
    std::string body(content_length, '\0');
    std::cin.read(body.data(), static_cast<std::streamsize>(content_length));
    if (std::cin.fail()) return std::nullopt;

    return body;
}

void LspServer::send_message(const Value& msg) {
    std::string body = json::to_string(msg);
    std::cout << "Content-Length: " << body.size() << "\r\n\r\n" << body;
    std::cout.flush();
}

void LspServer::send_response(const Value& id, const Value& result) {
    send_message(json::object({
        {"jsonrpc", "2.0"},
        {"id", id},
        {"result", result},
    }));
}

void LspServer::send_error(const Value& id, int code, const std::string& message) {
    send_message(json::object({
        {"jsonrpc", "2.0"},
        {"id", id},
        {"error", json::object({
            {"code", code},
            {"message", message},
        })},
    }));
}

void LspServer::send_notification(const std::string& method, const Value& params) {
    send_message(json::object({
        {"jsonrpc", "2.0"},
        {"method", method},
        {"params", params},
    }));
}

// ============================================================================
// Main loop
// ============================================================================

void LspServer::run() {
    while (running_) {
        auto msg = read_message();
        if (!msg) {
            break;
        }

        try {
            auto parsed = json::parse(*msg);
            dispatch(parsed);
        } catch (const json::ParseError& e) {
            std::cerr << "[eta-lsp] JSON parse error: " << e.what() << "\n";
        } catch (const std::exception& e) {
            std::cerr << "[eta-lsp] Error: " << e.what() << "\n";
        }
    }
}

// ============================================================================
// Dispatch
// ============================================================================

void LspServer::dispatch(const Value& msg) {
    auto method_opt = msg.get_string("method");
    if (!method_opt) {
        return; // Possibly a response — ignore
    }
    const auto& method = *method_opt;
    const auto& params = msg["params"];
    const auto& id = msg["id"];
    bool has_id = msg.has("id");

    if (method == "initialize") {
        auto result = handle_initialize(params);
        send_response(id, result);
    } else if (method == "initialized") {
        handle_initialized(params);
    } else if (method == "shutdown") {
        handle_shutdown();
        send_response(id, Value(nullptr));
    } else if (method == "exit") {
        handle_exit();
    } else if (method == "textDocument/didOpen") {
        handle_did_open(params);
    } else if (method == "textDocument/didChange") {
        handle_did_change(params);
    } else if (method == "textDocument/didClose") {
        handle_did_close(params);
    } else if (method == "textDocument/didSave") {
        handle_did_save(params);
    } else if (method == "textDocument/hover") {
        auto result = handle_hover(params);
        if (has_id) send_response(id, result);
    } else if (method == "textDocument/definition") {
        auto result = handle_definition(params);
        if (has_id) send_response(id, result);
    } else if (method == "textDocument/completion") {
        auto result = handle_completion(params);
        if (has_id) send_response(id, result);
    } else {
        if (has_id) {
            send_error(id, -32601, "Method not found: " + method);
        }
    }
}

// ============================================================================
// LSP: initialize / shutdown
// ============================================================================

Value LspServer::handle_initialize(const Value& /*params*/) {
    initialized_ = true;

    return json::object({
        {"capabilities", json::object({
            {"textDocumentSync", json::object({
                {"openClose", true},
                {"change", 1},  // 1 = Full sync
                {"save", json::object({{"includeText", true}})},
            })},
            {"hoverProvider", true},
            {"definitionProvider", true},
            {"completionProvider", json::object({
                {"triggerCharacters", json::array({"(", " "})},
            })},
        })},
        {"serverInfo", json::object({
            {"name", "eta-lsp"},
            {"version", "0.1.0"},
        })},
    });
}

void LspServer::handle_initialized(const Value& /*params*/) {
    // Client is ready
}

void LspServer::handle_shutdown() {
    shutdown_requested_ = true;
}

void LspServer::handle_exit() {
    running_ = false;
}

// ============================================================================
// Text document synchronization
// ============================================================================

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

    // Full sync: replace content with last change
    const auto& changes = params["contentChanges"];
    if (changes.is_array() && !changes.as_array().empty()) {
        const auto& last = changes.as_array().back();
        auto text = last.get_string("text");
        if (text) {
            it->second.content = *text;
        }
    }

    validate_document(uri);
}

void LspServer::handle_did_close(const Value& params) {
    auto uri = params["textDocument"].get_string("uri").value_or("");
    documents_.erase(uri);
    // Clear diagnostics for closed document
    publish_diagnostics(uri, {});
}

void LspServer::handle_did_save(const Value& params) {
    auto uri = params["textDocument"].get_string("uri").value_or("");
    // Re-validate on save
    auto text = params.get_string("text");
    if (text) {
        auto it = documents_.find(uri);
        if (it != documents_.end()) {
            it->second.content = *text;
        }
    }
    validate_document(uri);
}

// ============================================================================
// Diagnostics: Run the eta pipeline and collect errors
// ============================================================================

void LspServer::validate_document(const std::string& uri) {
    auto it = documents_.find(uri);
    if (it == documents_.end()) return;

    const auto& source = it->second.content;
    std::vector<LspDiagnostic> diags;

    // ── Phase 1: Lex + Parse ──────────────────────────────────────────
    reader::lexer::Lexer lex(0, source);
    reader::parser::Parser parser(lex);

    auto parsed_res = parser.parse_toplevel();
    if (!parsed_res) {
        auto& err = parsed_res.error();
        std::visit([&](auto&& e) {
            using T = std::decay_t<decltype(e)>;
            LspDiagnostic d;
            d.severity = 1; // Error
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
        publish_diagnostics(uri, diags);
        return;
    }

    auto& parsed = *parsed_res;
    if (parsed.empty()) {
        publish_diagnostics(uri, {});
        return;
    }

    // ── Phase 2: Expand ───────────────────────────────────────────────
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
        publish_diagnostics(uri, diags);
        return;
    }

    // ── Phase 3: Link ─────────────────────────────────────────────────
    // Move expanded forms out (SExprPtr = unique_ptr, not copyable).
    // Load any external modules required by (import ...) forms so the
    // linker can resolve cross-module references without false diagnostics.
    auto all_forms = std::move(*expanded_res);
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
        preload_module_deps(all_forms, seen_modules);
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
        publish_diagnostics(uri, diags);
        return;
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
        publish_diagnostics(uri, diags);
        return;
    }

    // ── Phase 4: Semantic Analysis ────────────────────────────────────
    semantics::SemanticAnalyzer sa;
    runtime::BuiltinEnvironment builtins;
    runtime::register_builtin_names(builtins);
    auto sem_res = sa.analyze_all(all_forms, linker, builtins);
    if (!sem_res) {
        const auto& err = sem_res.error();
        LspDiagnostic d;
        d.severity = 1;
        d.range = span_to_range(err.span.start.line, err.span.start.column,
                                err.span.end.line, err.span.end.column);
        d.message = err.message;
        diags.push_back(std::move(d));
        publish_diagnostics(uri, diags);
        return;
    }

    // All phases passed — clear diagnostics
    publish_diagnostics(uri, {});
}

void LspServer::publish_diagnostics(const std::string& uri,
                                     const std::vector<LspDiagnostic>& diags) {
    Array diag_array;
    diag_array.reserve(diags.size());
    for (const auto& d : diags) {
        diag_array.push_back(diagnostic_to_json(d));
    }

    send_notification("textDocument/publishDiagnostics", json::object({
        {"uri", uri},
        {"diagnostics", Value(std::move(diag_array))},
    }));
}

// ============================================================================
// Step 6: Semantic features — Hover
// ============================================================================

Value LspServer::handle_hover(const Value& params) {
    auto uri = params["textDocument"].get_string("uri").value_or("");
    auto it = documents_.find(uri);
    if (it == documents_.end()) return Value(nullptr);

    auto line = params["position"].get_int("line").value_or(0);
    auto character = params["position"].get_int("character").value_or(0);

    const auto& source = it->second.content;
    auto word = word_at_position(source, line, character);
    if (word.empty()) return Value(nullptr);

    // Known keywords/special forms
    static const std::unordered_map<std::string, std::string> keyword_docs = {
        {"define",       "**define** — Define a variable or function.\n\n`(define name expr)`\n\n`(define (name args...) body...)`"},
        {"lambda",       "**lambda** — Create an anonymous function.\n\n`(lambda (args...) body...)`"},
        {"if",           "**if** — Conditional expression.\n\n`(if test consequent alternate)`"},
        {"begin",        "**begin** — Sequence expressions.\n\n`(begin expr...)`"},
        {"set!",         "**set!** — Mutate a variable binding.\n\n`(set! name expr)`"},
        {"quote",        "**quote** — Return datum without evaluation.\n\n`(quote datum)` or `'datum`"},
        {"let",          "**let** — Parallel local bindings.\n\n`(let ((var init)...) body...)`"},
        {"let*",         "**let*** — Sequential local bindings.\n\n`(let* ((var init)...) body...)`"},
        {"letrec",       "**letrec** — Recursive local bindings.\n\n`(letrec ((var init)...) body...)`"},
        {"letrec*",      "**letrec*** — Sequential recursive bindings.\n\n`(letrec* ((var init)...) body...)`"},
        {"cond",         "**cond** — Multi-way conditional.\n\n`(cond (test expr...)... (else expr...))`"},
        {"case",         "**case** — Dispatch on datum equality.\n\n`(case key ((datum...) expr...)... (else expr...))`"},
        {"and",          "**and** — Short-circuit logical and.\n\n`(and expr...)` → last truthy or `#f`"},
        {"or",           "**or** — Short-circuit logical or.\n\n`(or expr...)` → first truthy or `#f`"},
        {"when",         "**when** — One-armed conditional.\n\n`(when test body...)`"},
        {"unless",       "**unless** — Negated one-armed conditional.\n\n`(unless test body...)`"},
        {"do",           "**do** — Iteration construct.\n\n`(do ((var init step)...) (test result...) body...)`"},
        {"module",       "**module** — Declare a module.\n\n`(module name body...)`"},
        {"import",       "**import** — Import bindings from a module.\n\n`(import module-name)`"},
        {"export",       "**export** — Export bindings.\n\n`(export name...)`"},
        {"define-syntax","**define-syntax** — Define a hygienic macro.\n\n`(define-syntax name (syntax-rules (literals...) clause...))`"},
        {"syntax-rules", "**syntax-rules** — Hygienic macro transformer.\n\n`(syntax-rules (literals...) (pattern template)...)`"},
        {"define-record-type", "**define-record-type** — Define a record type.\n\n`(define-record-type name (ctor field...) pred (field accessor [mutator])...)`"},
        {"def",          "**def** — Sugar for `define`.\n\n`(def name expr)` or `(def (name args...) body...)`"},
        {"defun",        "**defun** — Sugar for function definition.\n\n`(defun name (args...) body...)`"},
        {"progn",        "**progn** — Alias for `begin`.\n\n`(progn expr...)`"},
        {"quasiquote",   "**quasiquote** — Template with unquote.\n\n`` `(datum ,expr ,@splice) ``"},
        {"call/cc",      "**call/cc** — Call with current continuation.\n\n`(call/cc proc)`"},
        {"call-with-current-continuation", "**call-with-current-continuation** — Full name for `call/cc`.\n\n`(call-with-current-continuation proc)`"},
        {"dynamic-wind", "**dynamic-wind** — Guard entry/exit of a continuation.\n\n`(dynamic-wind before thunk after)`"},
        {"values",       "**values** — Return multiple values.\n\n`(values expr...)`"},
        {"call-with-values", "**call-with-values** — Receive multiple values.\n\n`(call-with-values producer consumer)`"},
        {"apply",        "**apply** — Apply procedure to argument list.\n\n`(apply proc arg... arg-list)`"},
    };

    auto kit = keyword_docs.find(word);
    if (kit != keyword_docs.end()) {
        return json::object({
            {"contents", json::object({
                {"kind", "markdown"},
                {"value", kit->second},
            })},
        });
    }

    // Check document-local definitions
    auto symbols = collect_symbols(source);
    for (const auto& sym : symbols) {
        if (sym.name == word) {
            std::string doc = "**" + sym.name + "** — " + sym.kind +
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

// ============================================================================
// Step 6: Semantic features — Go to Definition
// ============================================================================

Value LspServer::handle_definition(const Value& params) {
    auto uri = params["textDocument"].get_string("uri").value_or("");
    auto it = documents_.find(uri);
    if (it == documents_.end()) return Value(nullptr);

    auto line = params["position"].get_int("line").value_or(0);
    auto character = params["position"].get_int("character").value_or(0);

    const auto& source = it->second.content;
    auto word = word_at_position(source, line, character);
    if (word.empty()) return Value(nullptr);

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

    return Value(nullptr);
}

// ============================================================================
// Step 6: Semantic features — Completion
// ============================================================================

Value LspServer::handle_completion(const Value& params) {
    auto uri = params["textDocument"].get_string("uri").value_or("");
    auto it = documents_.find(uri);

    Array items;

    // Always suggest keywords
    static const std::vector<std::pair<std::string, std::string>> keywords = {
        {"define",       "Core: define a variable or function"},
        {"lambda",       "Core: anonymous function"},
        {"if",           "Core: conditional"},
        {"begin",        "Core: sequence"},
        {"set!",         "Core: mutation"},
        {"quote",        "Core: literal datum"},
        {"let",          "Binding: parallel local bindings"},
        {"let*",         "Binding: sequential local bindings"},
        {"letrec",       "Binding: recursive local bindings"},
        {"letrec*",      "Binding: sequential recursive bindings"},
        {"cond",         "Control: multi-way conditional"},
        {"case",         "Control: datum dispatch"},
        {"and",          "Control: short-circuit and"},
        {"or",           "Control: short-circuit or"},
        {"when",         "Control: one-armed if"},
        {"unless",       "Control: negated one-armed if"},
        {"do",           "Control: iteration"},
        {"module",       "Module: declare a module"},
        {"import",       "Module: import bindings"},
        {"export",       "Module: export bindings"},
        {"define-syntax","Macro: define a syntax transformer"},
        {"syntax-rules", "Macro: hygienic macro rules"},
        {"define-record-type", "Record: define a record type"},
        {"def",          "Sugar: alias for define"},
        {"defun",        "Sugar: function definition"},
        {"progn",        "Sugar: alias for begin"},
        {"quasiquote",   "Core: template with unquote"},
        {"call/cc",      "Advanced: call with current continuation"},
        {"dynamic-wind", "Advanced: continuation guard"},
        {"values",       "Advanced: multiple return values"},
        {"call-with-values", "Advanced: receive multiple values"},
        {"apply",        "Core: apply procedure to args"},
    };

    for (const auto& [kw, detail] : keywords) {
        items.push_back(json::object({
            {"label", kw},
            {"kind", 14}, // Keyword
            {"detail", detail},
        }));
    }

    // Add document-local symbols
    if (it != documents_.end()) {
        auto symbols = collect_symbols(it->second.content);
        std::unordered_set<std::string> seen;
        for (const auto& sym : symbols) {
            if (seen.insert(sym.name).second) {
                int kind = 6; // Variable
                if (sym.kind == "defun" || sym.kind == "function") kind = 3; // Function
                if (sym.kind == "macro") kind = 15; // Snippet
                if (sym.kind == "module") kind = 9; // Module

                items.push_back(json::object({
                    {"label", sym.name},
                    {"kind", kind},
                    {"detail", sym.kind},
                }));
            }
        }
    }

    return json::object({
        {"isIncomplete", false},
        {"items", Value(std::move(items))},
    });
}

// ============================================================================
// Helpers
// ============================================================================

Value LspServer::position_to_json(const Position& p) {
    return json::object({
        {"line", p.line},
        {"character", p.character},
    });
}

Value LspServer::range_to_json(const Range& r) {
    return json::object({
        {"start", position_to_json(r.start)},
        {"end", position_to_json(r.end)},
    });
}

Value LspServer::diagnostic_to_json(const LspDiagnostic& d) {
    return json::object({
        {"range", range_to_json(d.range)},
        {"severity", d.severity},
        {"source", d.source},
        {"message", d.message},
    });
}

Range LspServer::span_to_range(uint32_t start_line, uint32_t start_col,
                                            uint32_t end_line, uint32_t end_col) {
    // eta uses 1-based line/column; LSP uses 0-based
    return Range{
        Position{
            static_cast<int64_t>(start_line > 0 ? start_line - 1 : 0),
            static_cast<int64_t>(start_col > 0 ? start_col - 1 : 0),
        },
        Position{
            static_cast<int64_t>(end_line > 0 ? end_line - 1 : 0),
            static_cast<int64_t>(end_col > 0 ? end_col - 1 : 0),
        },
    };
}

std::string LspServer::word_at_position(const std::string& text,
                                         int64_t line, int64_t character) {
    std::istringstream iss(text);
    std::string current_line;
    for (int64_t i = 0; i <= line; ++i) {
        if (!std::getline(iss, current_line)) return {};
    }

    if (character < 0 || character >= static_cast<int64_t>(current_line.size()))
        return {};

    auto is_sym_char = [](char c) -> bool {
        if (c <= ' ') return false;
        if (c == '(' || c == ')' || c == '[' || c == ']' ||
            c == '"' || c == ';' || c == '#') return false;
        return true;
    };

    auto col = static_cast<std::size_t>(character);
    if (!is_sym_char(current_line[col])) return {};

    std::size_t start = col;
    while (start > 0 && is_sym_char(current_line[start - 1])) --start;
    std::size_t end = col;
    while (end < current_line.size() && is_sym_char(current_line[end])) ++end;

    return current_line.substr(start, end - start);
}

std::vector<LspServer::SymbolInfo> LspServer::collect_symbols(const std::string& source) {
    std::vector<SymbolInfo> result;

    // Quick scan for definition forms
    std::istringstream iss(source);
    std::string line;
    int64_t line_num = 0;

    while (std::getline(iss, line)) {
        // Skip comment-only lines
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

            auto name_start = pos + 1 + keyword.size() + 1; // skip '(' + keyword + space
            // Skip whitespace
            while (name_start < line.size() &&
                   (line[name_start] == ' ' || line[name_start] == '\t'))
                ++name_start;
            if (name_start >= line.size()) return;

            // If the name starts with '(', extract first symbol inside
            auto actual_start = name_start;
            if (line[actual_start] == '(') {
                ++actual_start;
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
                result.push_back(SymbolInfo{
                    line.substr(actual_start, name_end - actual_start),
                    kind,
                    line_num,
                    static_cast<int64_t>(actual_start),
                });
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

} // namespace eta::lsp

