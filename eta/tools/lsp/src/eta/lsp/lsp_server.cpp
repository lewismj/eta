#include "lsp_server.h"

#include <algorithm>
#include <cassert>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <unordered_set>
#include <variant>


/// Eta compiler includes
#include "eta/reader/lexer.h"
#include "eta/reader/parser.h"
#include "eta/reader/expander.h"
#include "eta/reader/module_linker.h"
#include "eta/reader/sexpr_utils.h"
#include "eta/semantics/semantic_analyzer.h"
#include "eta/runtime/builtin_env.h"
#include "eta/runtime/builtin_names.h"
#include "eta/interpreter/repl_complete.h"
#include "eta/package/lockfile.h"
#include "eta/package/manifest.h"
#include "eta/package/resolver.h"

namespace eta::lsp {

using namespace eta::json;

namespace {

[[nodiscard]] std::filesystem::path canonicalize_path(const std::filesystem::path& path) {
    std::error_code ec;
    const auto canonical = std::filesystem::weakly_canonical(path, ec);
    if (!ec) return canonical;
    return path.lexically_normal();
}

[[nodiscard]] std::optional<int> parse_hex_nibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
    if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
    return std::nullopt;
}

[[nodiscard]] LspDiagnostic package_error_diagnostic(const std::string& source,
                                                     std::string message,
                                                     std::size_t line) {
    LspDiagnostic d;
    d.source = source;
    d.severity = 1;
    const auto zero_based = (line > 0u) ? static_cast<int64_t>(line - 1u) : 0;
    d.range = Range{
        Position{zero_based, 0},
        Position{zero_based, 1},
    };
    d.message = std::move(message);
    return d;
}

} // namespace

/**
 * Construction
 */

LspServer::LspServer()
    : in_(std::cin), out_(std::cout)
{
    init_module_path();
}

LspServer::LspServer(std::istream& in, std::ostream& out)
    : in_(in), out_(out)
{
    init_module_path();
}

/**
 * Module path resolution
 */

void LspServer::init_module_path() {
    std::error_code ec;
    auto cwd = std::filesystem::current_path(ec);
    if (ec) {
        resolver_ = interpreter::ModulePathResolver::from_args_or_env("");
        return;
    }
    resolver_ = interpreter::ModulePathResolver::from_args_or_env_at("", cwd);
}

std::optional<std::filesystem::path> LspServer::uri_to_path(const std::string& uri) {
    constexpr std::string_view kFilePrefix = "file://";
    if (uri.rfind(kFilePrefix, 0) != 0) return std::nullopt;

    std::string_view encoded = std::string_view(uri).substr(kFilePrefix.size());
#if defined(_WIN32)
    if (encoded.size() >= 3u
        && encoded[0] == '/'
        && std::isalpha(static_cast<unsigned char>(encoded[1]))
        && encoded[2] == ':') {
        encoded.remove_prefix(1u);
    }
#endif

    std::string decoded;
    decoded.reserve(encoded.size());
    for (std::size_t i = 0; i < encoded.size(); ++i) {
        const char c = encoded[i];
        if (c == '%' && (i + 2u) < encoded.size()) {
            const auto hi = parse_hex_nibble(encoded[i + 1u]);
            const auto lo = parse_hex_nibble(encoded[i + 2u]);
            if (hi && lo) {
                decoded.push_back(static_cast<char>((*hi << 4) | *lo));
                i += 2u;
                continue;
            }
        }
        decoded.push_back(c);
    }
    if (decoded.empty()) return std::nullopt;

#if defined(_WIN32)
    std::replace(decoded.begin(), decoded.end(), '/', '\\');
#endif
    return std::filesystem::path(decoded);
}

std::optional<std::filesystem::path>
LspServer::find_manifest_path(std::filesystem::path start_dir) {
    if (start_dir.empty()) return std::nullopt;
    start_dir = canonicalize_path(start_dir);
    while (!start_dir.empty()) {
        const auto manifest = start_dir / "eta.toml";
        std::error_code ec;
        if (std::filesystem::is_regular_file(manifest, ec) && !ec) {
            return canonicalize_path(manifest);
        }
        const auto parent = start_dir.parent_path();
        if (parent.empty() || parent == start_dir) break;
        start_dir = parent;
    }
    return std::nullopt;
}

void LspServer::ensure_workspace_for_uri(const std::string& uri) {
    std::filesystem::path start_dir;
    if (auto doc_path = uri_to_path(uri)) {
        std::error_code ec;
        if (std::filesystem::is_directory(*doc_path, ec) && !ec) {
            start_dir = *doc_path;
        } else {
            start_dir = doc_path->parent_path();
        }
    }
    if (start_dir.empty()) {
        std::error_code ec;
        start_dir = std::filesystem::current_path(ec);
        if (ec) start_dir.clear();
    }

    std::optional<std::filesystem::path> manifest_path;
    if (!start_dir.empty()) {
        manifest_path = find_manifest_path(start_dir);
    }
    const std::optional<std::filesystem::path> root_path =
        manifest_path ? std::optional<std::filesystem::path>(manifest_path->parent_path())
                      : std::nullopt;

    if (workspace_manifest_path_ == manifest_path
        && workspace_root_path_ == root_path) {
        return;
    }

    if (manifest_path) {
        resolver_ = interpreter::ModulePathResolver::from_args_or_env_at(
            "", manifest_path->parent_path());
    } else {
        resolver_ = interpreter::ModulePathResolver::from_args_or_env("");
    }
    workspace_manifest_path_ = std::move(manifest_path);
    workspace_root_path_ = root_path;

    completion_cache_loaded_ = false;
    prelude_symbols_.clear();
    module_path_symbols_.clear();
}

void LspServer::publish_workspace_package_diagnostics(const std::string& /*uri*/) {
    if (!workspace_manifest_path_) {
        if (manifest_diagnostics_uri_) {
            publish_diagnostics(*manifest_diagnostics_uri_, {});
            manifest_diagnostics_uri_.reset();
        }
        if (lockfile_diagnostics_uri_) {
            publish_diagnostics(*lockfile_diagnostics_uri_, {});
            lockfile_diagnostics_uri_.reset();
        }
        return;
    }

    const auto manifest_path = *workspace_manifest_path_;
    const auto manifest_uri = path_to_uri(manifest_path.string());
    if (manifest_diagnostics_uri_ && *manifest_diagnostics_uri_ != manifest_uri) {
        publish_diagnostics(*manifest_diagnostics_uri_, {});
    }
    manifest_diagnostics_uri_ = manifest_uri;

    std::vector<LspDiagnostic> manifest_diags;
    auto manifest = eta::package::read_manifest(manifest_path);
    if (!manifest) {
        manifest_diags.push_back(package_error_diagnostic(
            "eta-manifest", manifest.error().message, manifest.error().line));
    }

    const auto lockfile_path = manifest_path.parent_path() / "eta.lock";
    std::optional<std::string> lockfile_uri;
    std::optional<eta::package::Lockfile> lockfile;
    {
        std::error_code ec;
        if (std::filesystem::is_regular_file(lockfile_path, ec) && !ec) {
            lockfile_uri = path_to_uri(lockfile_path.string());
        }
    }

    std::vector<LspDiagnostic> lockfile_diags;
    if (lockfile_uri) {
        auto lock = eta::package::read_lockfile(lockfile_path);
        if (!lock) {
            lockfile_diags.push_back(package_error_diagnostic(
                "eta-lockfile", lock.error().message, lock.error().line));
        } else {
            lockfile = std::move(*lock);
        }
    }

    if (manifest && manifest_diags.empty()) {
        eta::package::ResolveOptions options;
        options.modules_root = manifest_path.parent_path() / ".eta" / "modules";
        if (lockfile) options.lockfile = &*lockfile;
        auto resolved = eta::package::resolve_dependencies(manifest_path, options);
        if (!resolved) {
            manifest_diags.push_back(package_error_diagnostic(
                "eta-manifest", resolved.error().message, 0));
        }
    }

    if (lockfile_diagnostics_uri_) {
        if (!lockfile_uri || *lockfile_diagnostics_uri_ != *lockfile_uri) {
            publish_diagnostics(*lockfile_diagnostics_uri_, {});
            lockfile_diagnostics_uri_.reset();
        }
    }

    publish_diagnostics(manifest_uri, manifest_diags);
    if (lockfile_uri) {
        lockfile_diagnostics_uri_ = *lockfile_uri;
        publish_diagnostics(*lockfile_uri, lockfile_diags);
    }
}

std::optional<std::string> LspServer::resolve_module_source(const std::string& module_name) {
    auto path = resolver_.resolve(module_name);
    if (!path) return std::nullopt;

    auto source_path = *path;
    if (source_path.extension() == ".etac") {
        auto sibling = source_path;
        sibling.replace_extension(".eta");
        std::error_code ec;
        if (!std::filesystem::is_regular_file(sibling, ec) || ec) {
            return std::nullopt;
        }
        source_path = std::move(sibling);
    }
    if (source_path.extension() != ".eta") return std::nullopt;

    std::ifstream f(source_path);
    if (!f.is_open()) return std::nullopt;
    return std::string(std::istreambuf_iterator<char>(f),
                       std::istreambuf_iterator<char>());
}

void LspServer::preload_prelude(
        std::vector<eta::reader::parser::SExprPtr>& all_forms,
        std::unordered_set<std::string>& seen_modules) {
    using namespace reader::utils;

    /**
     * std.* modules inline (std.core, std.math, std.io, std.collections,
     * std.test, std.prelude).  We must load it before preload_module_deps so
     * that (import std.prelude) in user code resolves to a known module.
     */
    static const std::string prelude_uri = "eta://stdlib/prelude.eta";

    auto prelude_path = resolver_.find_file("prelude.eta");
    if (!prelude_path) return;

    std::ifstream f(*prelude_path);
    if (!f.is_open()) return;
    std::string src(std::istreambuf_iterator<char>(f),
                    std::istreambuf_iterator<char>{});

    reader::lexer::Lexer lex(0, src);
    reader::parser::Parser parser(lex);
    auto parsed = parser.parse_toplevel();
    if (!parsed) {
        /// Report parse error via publishDiagnostics so the user knows the stdlib is broken.
        std::vector<LspDiagnostic> diags;
        std::visit([&](auto&& e) {
            using T = std::decay_t<decltype(e)>;
            LspDiagnostic d;
            d.severity = 1;
            d.source   = "eta-prelude";
            if constexpr (std::is_same_v<T, reader::lexer::LexError>) {
                d.range   = span_to_range(e.span.start.line, e.span.start.column,
                                          e.span.end.line,   e.span.end.column);
                d.message = "[prelude.eta] lex error: " + (e.message.empty()
                    ? std::string(reader::lexer::to_string(e.kind)) : e.message);
            } else {
                d.range   = span_to_range(e.span.start.line, e.span.start.column,
                                          e.span.end.line,   e.span.end.column);
                d.message = "[prelude.eta] parse error: "
                    + std::string(reader::parser::to_string(e.kind));
            }
            diags.push_back(std::move(d));
        }, parsed.error());
        publish_diagnostics(prelude_uri, diags);
        return;
    }

    reader::expander::Expander expander;
    auto expanded = expander.expand_many(*parsed);
    if (!expanded) {
        /// Report expansion error.
        const auto& err = expanded.error();
        LspDiagnostic d;
        d.severity = 1;
        d.source   = "eta-prelude";
        d.range    = span_to_range(err.span.start.line, err.span.start.column,
                                   err.span.end.line,   err.span.end.column);
        d.message  = "[prelude.eta] expand error: " + (err.message.empty()
            ? reader::expander::to_string(err.kind)
            : err.message);
        publish_diagnostics(prelude_uri, {d});
        return;
    }

    for (auto& form : *expanded) {
        /// Track module names so preload_module_deps won't try to re-load them
        const auto* mod = as_list(form);
        if (mod && mod->elems.size() >= 2 &&
            is_symbol_named(mod->elems[0], "module")) {
            if (const auto* name_sym = as_symbol(mod->elems[1])) {
                if (seen_modules.count(name_sym->name)) continue; ///< already present
                seen_modules.insert(name_sym->name);
            }
        }
        all_forms.push_back(std::move(form));
    }
}

void LspServer::preload_module_deps(
        std::vector<eta::reader::parser::SExprPtr>& all_forms,
        std::unordered_set<std::string>& seen_modules) {
    using namespace reader::utils;

    /// Collect imported module names from a form list
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
                        /// (only mod ...) / (except mod ...) / (rename mod ...) / (prefix mod pfx)
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
            if (!src) continue;

            reader::lexer::Lexer lex(0, *src);
            reader::parser::Parser parser(lex);
            auto parsed = parser.parse_toplevel();
            if (!parsed) continue;

            reader::expander::Expander expander;
            auto expanded = expander.expand_many(*parsed);
            if (!expanded) continue;

            /// Collect transitive imports before moving forms
            collect_imports(*expanded, next_load);

            for (auto& f : *expanded) all_forms.push_back(std::move(f));
        }
        to_load = std::move(next_load);
    }
}

/**
 * Transport: Header-Content framing over stdio
 */

std::optional<std::string> LspServer::read_message() {
    static constexpr std::size_t MAX_MESSAGE_SIZE = 64u * 1024u * 1024u;

    std::size_t content_length = 0;
    bool got_content_length = false;
    std::string header_line;
    while (std::getline(in_, header_line)) {
        if (!header_line.empty() && header_line.back() == '\r')
            header_line.pop_back();
        if (header_line.empty()) break;

        const std::string prefix = "Content-Length: ";
        if (header_line.substr(0, prefix.size()) == prefix) {
            try {
                content_length = std::stoull(header_line.substr(prefix.size()));
                got_content_length = true;
            } catch (const std::exception&) {
                std::cerr << "[eta-lsp] warning: malformed Content-Length header: "
                          << header_line << "\n";
            }
        }
        /// Ignore other headers (Content-Type, etc.)
    }

    if (in_.eof() || in_.fail()) return std::nullopt;
    if (!got_content_length || content_length == 0) return std::nullopt;

    /// Guard against unbounded allocation from a crafted Content-Length value.
    if (content_length > MAX_MESSAGE_SIZE) {
        std::cerr << "[eta-lsp] warning: Content-Length " << content_length
                  << " exceeds maximum (" << MAX_MESSAGE_SIZE << " bytes); dropping message\n";
        return std::nullopt;
    }

    std::string body(content_length, '\0');
    in_.read(body.data(), static_cast<std::streamsize>(content_length));
    if (in_.fail()) return std::nullopt;

    return body;
}

void LspServer::send_message(const Value& msg) {
    std::string body = json::to_string(msg);
    out_ << "Content-Length: " << body.size() << "\r\n\r\n" << body;
    out_.flush();
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

/**
 * Main loop
 */

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

/**
 * Dispatch
 */

void LspServer::dispatch(const Value& msg) {
    auto method_opt = msg.get_string("method");
    if (!method_opt) {
        return;
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
    } else if (method == "textDocument/documentSymbol") {
        auto result = handle_document_symbol(params);
        if (has_id) send_response(id, result);
    } else if (method == "textDocument/references") {
        auto result = handle_references(params);
        if (has_id) send_response(id, result);
    } else if (method == "textDocument/rename") {
        auto result = handle_rename(params);
        if (has_id) send_response(id, result);
    } else if (method == "textDocument/signatureHelp") {
        auto result = handle_signature_help(params);
        if (has_id) send_response(id, result);
    } else if (method == "textDocument/foldingRange") {
        auto result = handle_folding_range(params);
        if (has_id) send_response(id, result);
    } else if (method == "textDocument/selectionRange") {
        auto result = handle_selection_range(params);
        if (has_id) send_response(id, result);
    } else if (method == "workspace/symbol") {
        auto result = handle_workspace_symbol(params);
        if (has_id) send_response(id, result);
    } else if (method == "textDocument/semanticTokens/full") {
        auto result = handle_semantic_tokens_full(params);
        if (has_id) send_response(id, result);
    } else if (method == "textDocument/formatting") {
        auto result = handle_formatting(params);
        if (has_id) send_response(id, result);
    } else if (method == "textDocument/documentHighlight") {
        auto result = handle_document_highlight(params);
        if (has_id) send_response(id, result);
    } else if (method == "textDocument/inlayHint") {
        auto result = handle_inlay_hint(params);
        if (has_id) send_response(id, result);
    } else if (method == "eta/lockfile/explain") {
        auto result = handle_lockfile_explain(params);
        if (has_id) send_response(id, result);
    } else {
        if (has_id) {
            send_error(id, -32601, "Method not found: " + method);
        }
    }
}

/**
 * LSP: initialize / shutdown
 */

Value LspServer::handle_initialize(const Value& /*params*/) {
    initialized_ = true;

    return json::object({
        {"capabilities", json::object({
            {"textDocumentSync", json::object({
                {"openClose", true},
                {"change", 1},  ///< 1 = Full sync
                {"save", json::object({{"includeText", true}})},
            })},
            {"hoverProvider", true},
            {"definitionProvider", true},
            {"documentSymbolProvider", true},
            {"referencesProvider", true},
            {"renameProvider", true},
            {"completionProvider", json::object({
                {"triggerCharacters", json::array({"(", " "})},
            })},
            {"signatureHelpProvider", json::object({
                {"triggerCharacters", json::array({"(", ","})},
                {"retriggerCharacters", json::array({","})},
            })},
            {"foldingRangeProvider", true},
            {"selectionRangeProvider", true},
            {"workspaceSymbolProvider", true},
            {"documentFormattingProvider", true},
            {"documentHighlightProvider", true},
            {"inlayHintProvider", json::object({
                {"resolveProvider", false},
            })},
            {"semanticTokensProvider", json::object({
                {"legend", json::object({
                    {"tokenTypes", json::array({
                        "keyword", "function", "variable", "string",
                        "number", "comment", "operator", "type",
                        "macro", "parameter"
                    })},
                    {"tokenModifiers", json::array({
                        "declaration", "definition"
                    })},
                })},
                {"full", true},
            })},
        })},
        {"serverInfo", json::object({
            {"name", "eta-lsp"},
            {"version", "0.1.0"},
        })},
    });
}

void LspServer::handle_initialized(const Value& /*params*/) {
    /// Client is ready
}

void LspServer::handle_shutdown() {
    shutdown_requested_ = true;
}

void LspServer::handle_exit() {
    running_ = false;
}

Value LspServer::handle_lockfile_explain(const Value& params) {
    std::string uri = params.get_string("uri").value_or("");
    if (uri.empty()) {
        uri = params["textDocument"].get_string("uri").value_or("");
    }
    if (!uri.empty()) {
        ensure_workspace_for_uri(uri);
    } else if (!documents_.empty()) {
        ensure_workspace_for_uri(documents_.begin()->first);
    }

    const std::string module_name =
        params.get_string("module").value_or(
            params.get_string("moduleName").value_or(""));

    Array roots;
    roots.reserve(resolver_.dirs().size());
    for (const auto& dir : resolver_.dirs()) {
        roots.push_back(dir.string());
    }

    Array candidates;
    if (!module_name.empty()) {
        for (const auto& candidate : resolver_.resolve_all(module_name)) {
            candidates.push_back(candidate.string());
        }
    }

    Object response{
        {"module", module_name},
        {"roots", Value(std::move(roots))},
        {"candidates", Value(std::move(candidates))},
    };
    if (module_name.empty()) {
        response.insert_or_assign("selected", Value(nullptr));
        response.insert_or_assign("error", Value("missing 'module' parameter"));
    } else if (auto selected = resolver_.resolve(module_name)) {
        response.insert_or_assign("selected", selected->string());
    } else {
        response.insert_or_assign("selected", Value(nullptr));
    }

    if (workspace_root_path_) {
        response.insert_or_assign("workspaceRoot", workspace_root_path_->string());
    }
    if (workspace_manifest_path_) {
        response.insert_or_assign("manifestPath", workspace_manifest_path_->string());
        const auto lockfile = workspace_manifest_path_->parent_path() / "eta.lock";
        std::error_code ec;
        if (std::filesystem::is_regular_file(lockfile, ec) && !ec) {
            response.insert_or_assign("lockfilePath", lockfile.string());
        }
    }
    return Value(std::move(response));
}

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

    /// Full sync: replace content with last change
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
    std::vector<LspDiagnostic> diags;

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
        publish_diagnostics(uri, diags);
        return;
    }

    auto& parsed = *parsed_res;
    if (parsed.empty()) {
        publish_diagnostics(uri, {});
        return;
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
        publish_diagnostics(uri, diags);
        return;
    }

    /**
     * Link
     * Move expanded forms out (SExprPtr = unique_ptr, not copyable).
     * Load any external modules required by (import ...) forms so the
     * linker can resolve cross-module references without false diagnostics.
     */
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
        preload_prelude(all_forms, seen_modules);
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

    /// Semantic Analysis
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

    /// Known keywords/special forms
    static const std::unordered_map<std::string, std::string> keyword_docs = {
        {"define",       "**define**  -  Define a variable or function.\n\n`(define name expr)`\n\n`(define (name args...) body...)`"},
        {"lambda",       "**lambda**  -  Create an anonymous function.\n\n`(lambda (args...) body...)`"},
        {"if",           "**if**  -  Conditional expression.\n\n`(if test consequent alternate)`"},
        {"begin",        "**begin**  -  Sequence expressions.\n\n`(begin expr...)`"},
        {"set!",         "**set!**  -  Mutate a variable binding.\n\n`(set! name expr)`"},
        {"quote",        "**quote**  -  Return datum without evaluation.\n\n`(quote datum)` or `'datum`"},
        {"let",          "**let**  -  Parallel local bindings.\n\n`(let ((var init)...) body...)`"},
        {"let*",         "**let***  -  Sequential local bindings.\n\n`(let* ((var init)...) body...)`"},
        {"letrec",       "**letrec**  -  Recursive local bindings.\n\n`(letrec ((var init)...) body...)`"},
        {"letrec*",      "**letrec***  -  Sequential recursive bindings.\n\n`(letrec* ((var init)...) body...)`"},
        {"cond",         "**cond**  -  Multi-way conditional.\n\n`(cond (test expr...)... (else expr...))`"},
        {"case",         "**case**  -  Dispatch on datum equality.\n\n`(case key ((datum...) expr...)... (else expr...))`"},
        {"and",          "**and**  -  Short-circuit logical and.\n\n`(and expr...)` -> last truthy or `#f`"},
        {"or",           "**or**  -  Short-circuit logical or.\n\n`(or expr...)` -> first truthy or `#f`"},
        {"when",         "**when**  -  One-armed conditional.\n\n`(when test body...)`"},
        {"unless",       "**unless**  -  Negated one-armed conditional.\n\n`(unless test body...)`"},
        {"do",           "**do**  -  Iteration construct.\n\n`(do ((var init step)...) (test result...) body...)`"},
        {"module",       "**module**  -  Declare a module.\n\n`(module name body...)`"},
        {"import",       "**import**  -  Import bindings from a module.\n\n`(import module-name)`"},
        {"export",       "**export**  -  Export bindings.\n\n`(export name...)`"},
        {"define-syntax","**define-syntax**  -  Define a hygienic macro.\n\n`(define-syntax name (syntax-rules (literals...) clause...))`"},
        {"syntax-rules", "**syntax-rules**  -  Hygienic macro transformer.\n\n`(syntax-rules (literals...) (pattern template)...)`"},
        {"define-record-type", "**define-record-type**  -  Define a record type.\n\n`(define-record-type name (ctor field...) pred (field accessor [mutator])...)`"},
        {"def",          "**def**  -  Alias for `define`.\n\n`(def name expr)` or `(def (name args...) body...)`"},
        {"defun",        "**defun**  -  Alias for function definition.\n\n`(defun name (args...) body...)`"},
        {"progn",        "**progn**  -  Alias for `begin`.\n\n`(progn expr...)`"},
        {"quasiquote",   "**quasiquote**  -  Template with unquote.\n\n`` `(datum ,expr ,@splice) ``"},
        {"call/cc",      "**call/cc**  -  Call with current continuation.\n\n`(call/cc proc)`"},
        {"call-with-current-continuation", "**call-with-current-continuation**  -  Full name for `call/cc`.\n\n`(call-with-current-continuation proc)`"},
        {"dynamic-wind", "**dynamic-wind**  -  Guard entry/exit of a continuation.\n\n`(dynamic-wind before thunk after)`"},
        {"values",       "**values**  -  Return multiple values.\n\n`(values expr...)`"},
        {"call-with-values", "**call-with-values**  -  Receive multiple values.\n\n`(call-with-values producer consumer)`"},
        {"apply",        "**apply**  -  Apply procedure to argument list.\n\n`(apply proc arg... arg-list)`"},
        /// Exception handling
        {"raise",        "**raise**  -  Raise an exception.\n\n`(raise tag value)`"},
        {"catch",        "**catch**  -  Catch an exception by tag.\n\n`(catch 'tag body ...)`"},
        /// Logic / unification
        {"logic-var",    "**logic-var**  -  Create a fresh logic variable.\n\n`(logic-var)`"},
        {"unify",        "**unify**  -  Unify two terms, extending the substitution.\n\n`(unify term1 term2)`"},
        {"deref-lvar",   "**deref-lvar**  -  Walk the substitution chain for a logic variable.\n\n`(deref-lvar lvar)`"},
        {"trail-mark",   "**trail-mark**  -  Record the current trail position.\n\n`(trail-mark)`"},
        {"unwind-trail", "**unwind-trail**  -  Undo bindings back to a saved trail mark.\n\n`(unwind-trail mark)`"},
        {"copy-term",    "**copy-term**  -  Deep-copy a term, freshening logic variables.\n\n`(copy-term term)`"},
        /// AD / tape
        {"make-dual",        "**make-dual**  -  Construct a dual number for forward-mode AD.\n\n`(make-dual primal tangent)`"},
        {"dual?",            "**dual?**  -  Predicate: is the value a dual number?\n\n`(dual? val)`"},
        {"dual-primal",      "**dual-primal**  -  Extract the primal from a dual number.\n\n`(dual-primal dual)`"},
        {"dual-backprop",    "**dual-backprop**  -  Extract the backprop closure from a dual.\n\n`(dual-backprop dual)`"},
        {"tape-new",         "**tape-new**  -  Create a new AD tape.\n\n`(tape-new)`"},
        {"tape-start!",      "**tape-start!**  -  Activate a tape for recording.\n\n`(tape-start! tape)`"},
        {"tape-stop!",       "**tape-stop!**  -  Deactivate the current tape (pops the most recent).\n\n`(tape-stop!)`"},
        {"tape-var",         "**tape-var**  -  Create a tracked tape variable.\n\n`(tape-var tape value)`"},
        {"tape-backward!",   "**tape-backward!**  -  Run reverse-mode backpropagation.\n\n`(tape-backward! tape root-ref)`"},
        {"tape-adjoint",     "**tape-adjoint**  -  Read the adjoint of a tape ref.\n\n`(tape-adjoint tape ref)`"},
        {"tape-primal",      "**tape-primal**  -  Read the primal value of a tape ref.\n\n`(tape-primal tape ref)`"},
        {"tape-ref?",        "**tape-ref?**  -  Predicate: is the value a tape reference?\n\n`(tape-ref? val)`"},
        {"tape-ref-index",   "**tape-ref-index**  -  Get the integer index of a tape ref.\n\n`(tape-ref-index ref)`"},
        {"tape-size",        "**tape-size**  -  Number of recorded nodes on the tape.\n\n`(tape-size tape)`"},
        {"tape-ref-value",   "**tape-ref-value**  -  Get the primal value stored in a tape ref.\n\n`(tape-ref-value ref)`"},
        /// CLP
        {"%clp-domain-z!",  "**%clp-domain-z!**  -  Set an integer domain constraint.\n\n`(%clp-domain-z! lvar lo hi)`"},
        {"%clp-domain-fd!",  "**%clp-domain-fd!**  -  Set a finite-domain constraint.\n\n`(%clp-domain-fd! lvar domain)`"},
        {"%clp-get-domain",  "**%clp-get-domain**  -  Query the current domain of a constrained variable.\n\n`(%clp-get-domain lvar)`"},
#ifdef ETA_HAS_NNG
        /// nng / message-passing builtins
        {"nng-socket",     "**nng-socket**  -  Create an nng socket.\n\n`(nng-socket type-sym)` where type-sym is one of: `'pair` `'pub` `'sub` `'push` `'pull` `'req` `'rep` `'surveyor` `'respondent` `'bus`"},
        {"nng-listen",     "**nng-listen**  -  Listen on an endpoint.\n\n`(nng-listen sock endpoint)`  -  e.g. `\"tcp:///<*:5555\"`, `\"ipc:///tmp/eta.sock\"`, `\"inproc://workers\"`"},
        {"nng-dial",       "**nng-dial**  -  Dial (connect to) an endpoint.\n\n`(nng-dial sock endpoint)`"},
        {"nng-close",      "**nng-close**  -  Close the socket (idempotent).\n\n`(nng-close sock)`"},
        {"nng-socket?",    "**nng-socket?**  -  Socket predicate.\n\n`(nng-socket? x)` -> `#t` if x is an nng socket"},
        {"send!",          "**send!**  -  Serialize a value and send it over a socket.\n\n`(send! sock value [flag])`  -  flag: `'noblock` or `'wait`"},
        {"recv!",          "**recv!**  -  Receive a value from a socket.\n\n`(recv! sock [flag])`  -  returns value or `#f` on timeout; flag: `'noblock` or `'wait`"},
        {"nng-poll",       "**nng-poll**  -  Poll multiple sockets for readiness.\n\n`(nng-poll items timeout-ms)`  -  items is a list of `(socket . events)` pairs; returns list of ready sockets"},
        {"nng-subscribe",  "**nng-subscribe**  -  Set SUB topic filter.\n\n`(nng-subscribe sock topic)`  -  topic is a string prefix"},
        {"nng-set-option", "**nng-set-option**  -  Set a socket option.\n\n`(nng-set-option sock option value)`  -  option: `'recv-timeout` `'send-timeout` `'recv-buf-size` `'survey-time`"},
        /// actor model
        {"spawn",           "**spawn**  -  Spawn a child Eta process.\n\n`(spawn module-path)`  -  launches `etai <module-path>` as a child process and returns the parent-side PAIR socket for communication"},
        {"spawn-kill",      "**spawn-kill**  -  Forcibly terminate a spawned child.\n\n`(spawn-kill sock)`  -  sends SIGTERM; returns `#t` on success"},
        {"spawn-wait",      "**spawn-wait**  -  Wait for a spawned child to exit.\n\n`(spawn-wait sock)`  -  blocks until child exits; returns the exit code as a fixnum"},
        {"current-mailbox", "**current-mailbox**  -  The PAIR socket to the parent process.\n\n`(current-mailbox)`  -  returns the socket established by `--mailbox` at startup, or `()` if not a spawned child"},
        /// in-process actor threads
        {"spawn-thread-with", "**spawn-thread-with**  -  Spawn an in-process actor thread.\n\n`(spawn-thread-with module-path func-name args...)`  -  launches a new OS thread with its own VM, loads the module, calls `(func-name args...)`, communicates via `inproc://` PAIR socket"},
        {"spawn-thread",      "**spawn-thread**  -  Spawn an actor thread from a closure.\n\n`(spawn-thread thunk)`  -  use `spawn-thread-with` instead for named functions"},
        {"thread-join",       "**thread-join**  -  Wait for an actor thread to complete.\n\n`(thread-join sock)`  -  blocks until the thread exits; returns `0` on success, `#f` if not found"},
        {"thread-alive?",     "**thread-alive?**  -  Check if an actor thread is still running.\n\n`(thread-alive? sock)`  -  returns `#t` while the thread is executing, `#f` after it exits"},
#endif
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

    /// Tier 0: Keywords (special forms)
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
        {"def",          "Alias: for define"},
        {"defun",        "Alias: function definition"},
        {"progn",        "Alias: for begin"},
        {"quasiquote",   "Core: template with unquote"},
        {"call/cc",      "Advanced: call with current continuation"},
        {"dynamic-wind", "Advanced: continuation guard"},
        {"values",       "Advanced: multiple return values"},
        {"call-with-values", "Advanced: receive multiple values"},
        {"apply",        "Core: apply procedure to args"},
        {"raise",        "Exception: raise an exception"},
        {"catch",        "Exception: catch an exception"},
        {"logic-var",    "Logic: create a logic variable"},
        {"unify",        "Logic: unify two terms"},
        {"deref-lvar",   "Logic: dereference a logic variable"},
        {"trail-mark",   "Logic: mark the trail"},
        {"unwind-trail", "Logic: unwind the trail"},
        {"copy-term",    "Logic: deep-copy a term"},
    };

    for (const auto& [kw, detail] : keywords) {
        seen.insert(kw);
        items.push_back(json::object({
            {"label", kw},
            {"kind", 14}, ///< Keyword
            {"detail", detail},
        }));
    }

    /**
     * Tier 1: Static builtins (from builtin_names.h)
     * name, arity, has_rest, category
     */
    struct BuiltinDesc { const char* name; int arity; bool has_rest; const char* category; };
    static const std::vector<BuiltinDesc> builtins = {
        /// Arithmetic
        {"+", 0, true, "Arithmetic"}, {"-", 1, true, "Arithmetic"},
        {"*", 0, true, "Arithmetic"}, {"/", 1, true, "Arithmetic"},
        /// Numeric comparison
        {"=", 2, true, "Comparison"}, {"<", 2, true, "Comparison"},
        {">", 2, true, "Comparison"}, {"<=", 2, true, "Comparison"},
        {">=", 2, true, "Comparison"},
        /// Equivalence
        {"eq?", 2, false, "Equivalence"}, {"eqv?", 2, false, "Equivalence"},
        {"not", 1, false, "Equivalence"},
        /// Pairs / lists
        {"cons", 2, false, "List"}, {"car", 1, false, "List"},
        {"cdr", 1, false, "List"}, {"pair?", 1, false, "List"},
        {"null?", 1, false, "List"}, {"list", 0, true, "List"},
        /// Type predicates
        {"number?", 1, false, "Predicate"}, {"boolean?", 1, false, "Predicate"},
        {"string?", 1, false, "Predicate"}, {"char?", 1, false, "Predicate"},
        {"symbol?", 1, false, "Predicate"}, {"procedure?", 1, false, "Predicate"},
        {"integer?", 1, false, "Predicate"},
        /// Numeric predicates
        {"zero?", 1, false, "Predicate"}, {"positive?", 1, false, "Predicate"},
        {"negative?", 1, false, "Predicate"},
        /// Numeric operations
        {"abs", 1, false, "Math"}, {"min", 2, true, "Math"},
        {"max", 2, true, "Math"}, {"modulo", 2, false, "Math"},
        {"remainder", 2, false, "Math"},
        /// Transcendentals
        {"sin", 1, false, "Math"}, {"cos", 1, false, "Math"},
        {"tan", 1, false, "Math"}, {"asin", 1, false, "Math"},
        {"acos", 1, false, "Math"}, {"atan", 1, true, "Math"},
        {"exp", 1, false, "Math"}, {"log", 1, false, "Math"},
        {"sqrt", 1, false, "Math"},
        /// List operations
        {"length", 1, false, "List"}, {"append", 0, true, "List"},
        {"reverse", 1, false, "List"}, {"list-ref", 2, false, "List"},
        /// Higher-order
        {"map", 2, false, "Higher-order"}, {"for-each", 2, false, "Higher-order"},
        /// Deep equality
        {"equal?", 2, false, "Equivalence"},
        /// String operations
        {"string-length", 1, false, "String"}, {"string-append", 0, true, "String"},
        {"number->string", 1, false, "String"}, {"string->number", 1, false, "String"},
        /// Vector operations
        {"vector", 0, true, "Vector"}, {"vector-length", 1, false, "Vector"},
        {"vector-ref", 2, false, "Vector"}, {"vector-set!", 3, false, "Vector"},
        {"vector?", 1, false, "Vector"}, {"make-vector", 2, false, "Vector"},
        /// Misc
        {"error", 1, true, "Misc"}, {"platform", 0, false, "Misc"},
        {"logic-var?", 1, false, "Logic"}, {"ground?", 1, false, "Logic"},
        /// AD Dual
        {"dual?", 1, false, "AD"}, {"dual-primal", 1, false, "AD"},
        {"dual-backprop", 1, false, "AD"}, {"make-dual", 2, false, "AD"},
        /// CLP
        {"%clp-domain-z!", 3, false, "CLP"}, {"%clp-domain-fd!", 2, false, "CLP"},
        {"%clp-get-domain", 1, false, "CLP"},
        /// AD Tape
        {"tape-new", 0, false, "AD"}, {"tape-start!", 1, false, "AD"},
        {"tape-stop!", 0, false, "AD"}, {"tape-var", 2, false, "AD"},
        {"tape-backward!", 2, false, "AD"}, {"tape-adjoint", 2, false, "AD"},
        {"tape-primal", 2, false, "AD"}, {"tape-ref?", 1, false, "AD"},
        {"tape-ref-index", 1, false, "AD"}, {"tape-size", 1, false, "AD"},
        {"tape-ref-value", 1, false, "AD"},
        /// Port primitives
        {"current-input-port", 0, false, "Port"}, {"current-output-port", 0, false, "Port"},
        {"current-error-port", 0, false, "Port"},
        {"set-current-input-port!", 1, false, "Port"}, {"set-current-output-port!", 1, false, "Port"},
        {"set-current-error-port!", 1, false, "Port"},
        {"open-output-string", 0, false, "Port"}, {"get-output-string", 1, false, "Port"},
        {"open-input-string", 1, false, "Port"}, {"write-string", 1, true, "Port"},
        {"read-char", 0, true, "Port"}, {"port?", 1, false, "Port"},
        {"input-port?", 1, false, "Port"}, {"output-port?", 1, false, "Port"},
        {"close-port", 1, false, "Port"}, {"close-input-port", 1, false, "Port"},
        {"close-output-port", 1, false, "Port"}, {"write-char", 1, true, "Port"},
        {"open-input-file", 1, false, "Port"}, {"open-output-file", 1, false, "Port"},
        {"open-output-bytevector", 0, false, "Port"}, {"open-input-bytevector", 1, false, "Port"},
        {"get-output-bytevector", 1, false, "Port"}, {"read-u8", 0, true, "Port"},
        {"write-u8", 1, true, "Port"}, {"binary-port?", 1, false, "Port"},
        /// I/O
        {"display", 1, true, "I/O"}, {"write", 1, true, "I/O"},
        {"newline", 0, true, "I/O"},
#ifdef ETA_HAS_NNG
        /// nng / message-passing
        {"nng-socket",     1, false, "NNG"}, {"nng-listen",     2, false, "NNG"},
        {"nng-dial",       2, false, "NNG"}, {"nng-close",      1, false, "NNG"},
        {"nng-socket?",    1, false, "NNG"}, {"send!",          2, true,  "NNG"},
        {"recv!",          1, true,  "NNG"}, {"nng-poll",       2, false, "NNG"},
        {"nng-subscribe",  2, false, "NNG"}, {"nng-set-option", 3, false, "NNG"},
        /// actor model
        {"spawn",           1, true,  "NNG"}, {"spawn-kill",      1, false, "NNG"},
        {"spawn-wait",      1, false, "NNG"}, {"current-mailbox", 0, false, "NNG"},
        /// in-process actor threads
        {"spawn-thread-with", 2, true,  "NNG"}, {"spawn-thread",  1, false, "NNG"},
        {"thread-join",       1, false, "NNG"}, {"thread-alive?", 1, false, "NNG"},
#endif
    };

    for (const auto& b : builtins) {
        if (!seen.insert(b.name).second) continue; ///< already added (e.g. apply is a keyword)
        std::string detail = std::string(b.category) + " (arity " + std::to_string(b.arity);
        if (b.has_rest) detail += "+";
        detail += ")";
        items.push_back(json::object({
            {"label", b.name},
            {"kind", 3}, ///< Function
            {"detail", detail},
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

void LspServer::load_completion_cache() {
    completion_cache_loaded_ = true;
    prelude_symbols_.clear();
    module_path_symbols_.clear();

    /// Scan prelude
    auto prelude_path = resolver_.find_file("prelude.eta");
    if (prelude_path) {
        std::ifstream f(*prelude_path);
        if (f.is_open()) {
            std::string src(std::istreambuf_iterator<char>(f),
                            std::istreambuf_iterator<char>{});
            auto syms = collect_symbols(src, /*capture_signature=*/true);

            /**
             * Determine which module each symbol belongs to by tracking
             * (module <name> ...) boundaries in the source.
             */
            std::string current_module;
            std::istringstream lines(src);
            std::string line;
            int64_t line_num = 0;
            std::vector<std::pair<int64_t, std::string>> module_starts;
            while (std::getline(lines, line)) {
                auto pos = line.find("(module ");
                if (pos != std::string::npos) {
                    auto name_start = pos + 8;
                    while (name_start < line.size() && line[name_start] == ' ') ++name_start;
                    auto name_end = name_start;
                    while (name_end < line.size() && line[name_end] != ' ' && line[name_end] != ')' && line[name_end] != '\n')
                        ++name_end;
                    if (name_end > name_start)
                        module_starts.push_back({line_num, line.substr(name_start, name_end - name_start)});
                }
                ++line_num;
            }

            for (auto& sym : syms) {
                if (sym.kind == "module") continue; ///< skip module declarations
                sym.file_path = prelude_path->string();
                /// Find which module this symbol belongs to
                for (auto rit = module_starts.rbegin(); rit != module_starts.rend(); ++rit) {
                    if (sym.line >= rit->first) {
                        sym.module_name = rit->second;
                        break;
                    }
                }
                prelude_symbols_.push_back(std::move(sym));
            }
        }
    }

    /// Scan module path
    scan_module_path_symbols();
}

void LspServer::scan_module_path_symbols() {
    namespace fs = std::filesystem;
    std::unordered_set<std::string> scanned_files;

    for (const auto& dir : resolver_.dirs()) {
        std::error_code ec;
        for (auto& entry : fs::recursive_directory_iterator(dir, fs::directory_options::skip_permission_denied, ec)) {
            if (!entry.is_regular_file()) continue;
            if (entry.path().extension() != ".eta") continue;

            auto canonical = entry.path().string();
            if (!scanned_files.insert(canonical).second) continue;

            if (entry.path().filename() == "prelude.eta") continue;

            std::ifstream f(entry.path());
            if (!f.is_open()) continue;
            std::string src(std::istreambuf_iterator<char>(f),
                            std::istreambuf_iterator<char>{});

            auto syms = collect_symbols(src, /*capture_signature=*/true);
            auto stem = entry.path().stem().string();
            auto file_str = entry.path().string();
            for (auto& sym : syms) {
                if (sym.kind == "module") continue;
                if (sym.module_name.empty()) sym.module_name = stem;
                sym.file_path = file_str;
                module_path_symbols_.push_back(std::move(sym));
            }
        }
    }
}

/**
 * Helpers
 */

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
    /// eta uses 1-based line/column; LSP uses 0-based
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
    if (line < 0 || character < 0) return {};

    std::size_t offset = 0;
    int64_t current_line = 0;
    while (current_line < line) {
        const auto nl = text.find('\n', offset);
        if (nl == std::string::npos) return {};
        offset = nl + 1;
        ++current_line;
    }

    const auto line_end = text.find('\n', offset);
    const std::size_t line_len = (line_end == std::string::npos)
        ? (text.size() - offset)
        : (line_end - offset);

    if (static_cast<std::size_t>(character) > line_len) return {};
    offset += static_cast<std::size_t>(character);

    const bool at_line_end = static_cast<std::size_t>(character) == line_len;
    const bool on_symbol =
        (offset < text.size()) && interpreter::repl_complete::is_symbol_char(text[offset]);
    const bool left_symbol =
        (offset > 0) && interpreter::repl_complete::is_symbol_char(text[offset - 1]);
    const bool on_space =
        (offset < text.size()) &&
        std::isspace(static_cast<unsigned char>(text[offset]));
    if (!on_symbol && !(left_symbol && (at_line_end || !on_space))) {
        return {};
    }

    const auto tok = interpreter::repl_complete::token_at(text, offset);
    return tok.text;
}

/**
 */

std::pair<int64_t, int64_t> LspServer::sexp_end(
        const std::string& source, int64_t start_line, int64_t start_col) {
    std::istringstream iss(source);
    std::string ln;
    int64_t row = 0;
    int depth = 0;
    bool in_string = false;

    while (std::getline(iss, ln)) {
        if (!ln.empty() && ln.back() == '\r') ln.pop_back();

        std::size_t col_start = (row == start_line)
            ? static_cast<std::size_t>(std::max<int64_t>(start_col, 0))
            : 0;

        bool in_line_comment = false;
        for (std::size_t col = col_start; col < ln.size(); ++col) {
            char c = ln[col];
            if (in_line_comment) break;
            if (in_string) {
                if (c == '\\') { ++col; continue; } ///< skip escape
                if (c == '"')  { in_string = false; }
                continue;
            }
            if (c == '"') { in_string = true;  continue; }
            if (c == ';') { in_line_comment = true; break; }
            if (c == '(') { ++depth; }
            else if (c == ')') {
                --depth;
                if (depth == 0) {
                    /// Return position just after the closing ')'
                    return {row, static_cast<int64_t>(col) + 1};
                }
            }
        }
        ++row;
    }
    return {start_line, start_col};
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

    /// Signature lookup

    /// Static table of builtin signatures
    static const std::vector<std::pair<std::string, std::string>> builtin_sigs = {
        {"+",              "(+ z ...)"}, {"-",  "(- z1 z2 ...)"}, {"*", "(* z ...)"}, {"/", "(/ z1 z2 ...)"},
        {"=",              "(= z1 z2 ...)"}, {"<",  "(< x1 x2 ...)"}, {">",  "(> x1 x2 ...)"},
        {"<=",             "(<= x1 x2 ...)"}, {">=", "(>= x1 x2 ...)"},
        {"cons",           "(cons obj1 obj2)"}, {"car",  "(car pair)"}, {"cdr", "(cdr pair)"},
        {"list",           "(list obj ...)"}, {"length",  "(length list)"},
        {"append",         "(append list ...)"}, {"reverse", "(reverse list)"},
        {"list-ref",       "(list-ref list k)"},
        {"map",            "(map proc list ...)"}, {"for-each", "(for-each proc list ...)"},
        {"display",        "(display obj [port])"}, {"write",   "(write obj [port])"},
        {"newline",        "(newline [port])"},   {"error",   "(error message irritant ...)"},
        {"apply",          "(apply proc arg ... args)"},
        {"not",            "(not obj)"},    {"eq?",    "(eq? obj1 obj2)"},
        {"eqv?",           "(eqv? obj1 obj2)"}, {"equal?",  "(equal? obj1 obj2)"},
        {"null?",          "(null? obj)"},  {"pair?",  "(pair? obj)"},  {"list?",   "(list? obj)"},
        {"number?",        "(number? obj)"},{"boolean?","(boolean? obj)"},{"string?","(string? obj)"},
        {"symbol?",        "(symbol? obj)"},{"procedure?","(procedure? obj)"},{"integer?","(integer? obj)"},
        {"zero?",          "(zero? z)"},    {"positive?","(positive? x)"},{"negative?","(negative? x)"},
        {"abs",            "(abs x)"},      {"min",  "(min x1 x2 ...)"}, {"max",   "(max x1 x2 ...)"},
        {"modulo",         "(modulo n1 n2)"},{"remainder","(remainder n1 n2)"},
        {"sqrt",           "(sqrt z)"},     {"expt",   "(expt z1 z2)"},
        {"floor",          "(floor x)"},    {"ceiling","(ceiling x)"},
        {"truncate",       "(truncate x)"}, {"round",  "(round x)"},
        {"sin",            "(sin z)"},{"cos","(cos z)"},{"tan","(tan z)"},
        {"asin",           "(asin z)"},{"acos","(acos z)"},{"atan","(atan y [x])"},
        {"exp",            "(exp z)"},{"log","(log z)"},
        {"string-length",  "(string-length string)"},
        {"string-append",  "(string-append string ...)"},
        {"number->string", "(number->string z [radix])"},
        {"string->number", "(string->number string [radix])"},
        {"vector",         "(vector obj ...)"}, {"make-vector", "(make-vector k [fill])"},
        {"vector-ref",     "(vector-ref vector k)"}, {"vector-set!", "(vector-set! vector k obj)"},
        {"vector-length",  "(vector-length vector)"},
        {"values",         "(values obj ...)"}, {"call-with-values", "(call-with-values producer consumer)"},
        {"call/cc",        "(call/cc proc)"}, {"dynamic-wind", "(dynamic-wind before thunk after)"},
        {"raise",          "(raise tag value)"},   {"catch",  "(catch 'tag body ...)"},
        {"logic-var",      "(logic-var)"},    {"unify", "(unify term1 term2)"},
        {"deref-lvar",     "(deref-lvar lvar)"},  {"trail-mark", "(trail-mark)"},
        {"unwind-trail",   "(unwind-trail mark)"}, {"copy-term", "(copy-term term)"},
        {"make-dual",      "(make-dual primal tangent)"},
        {"tape-new",       "(tape-new)"},          {"tape-start!", "(tape-start! tape)"},
        {"tape-stop!",     "(tape-stop! tape)"},   {"tape-var",    "(tape-var tape value)"},
        {"tape-backward!", "(tape-backward! tape root-ref)"},
        {"tape-adjoint",   "(tape-adjoint tape ref)"},
        {"tape-primal",    "(tape-primal tape ref)"},
        {"tape-ref-index", "(tape-ref-index ref)"}, {"tape-size", "(tape-size tape)"},
        {"tape-ref-value", "(tape-ref-value ref)"},
        {"%clp-domain-z!", "(%clp-domain-z! lvar lo hi)"},
        {"%clp-domain-fd!","(%clp-domain-fd! lvar domain)"},
        {"%clp-get-domain","(%clp-get-domain lvar)"},
        {"define",         "(define name value)"}, {"defun",  "(defun name (args...) body...)"},
        {"lambda",         "(lambda (args...) body...)"},
        {"let",            "(let ((var init) ...) body ...)"}, {"let*",  "(let* ((var init) ...) body ...)"},
        {"letrec",         "(letrec ((var init) ...) body ...)"}, {"if", "(if test consequent alternate)"},
        {"when",           "(when test body ...)"}, {"unless", "(unless test body ...)"},
        {"begin",          "(begin expr ...)"}, {"cond",  "(cond (test expr ...) ... (else expr ...))"},
        {"case",           "(case key ((datum ...) expr ...) ... (else expr ...))"},
        {"set!",           "(set! name value)"},
#ifdef ETA_HAS_NNG
        /// nng / message-passing
        {"nng-socket",    "(nng-socket type-symbol)"},
        {"nng-listen",    "(nng-listen sock endpoint)"},
        {"nng-dial",      "(nng-dial sock endpoint)"},
        {"nng-close",     "(nng-close sock)"},
        {"nng-socket?",   "(nng-socket? x)"},
        {"send!",         "(send! sock value [flag])"},
        {"recv!",         "(recv! sock [flag])"},
        {"nng-poll",      "(nng-poll items timeout-ms)"},
        {"nng-subscribe", "(nng-subscribe sock topic)"},
        {"nng-set-option","(nng-set-option sock option value)"},
        /// actor model
        {"spawn",           "(spawn module-path)"},
        {"spawn-kill",      "(spawn-kill sock)"},
        {"spawn-wait",      "(spawn-wait sock)"},
        {"current-mailbox", "(current-mailbox)"},
        /// in-process actor threads
        {"spawn-thread-with", "(spawn-thread-with module-path func-name args...)"},
        {"spawn-thread",      "(spawn-thread thunk)"},
        {"thread-join",       "(thread-join sock)"},
        {"thread-alive?",     "(thread-alive? sock)"},
#endif
    };

    std::string label;
    for (const auto& [name, sig] : builtin_sigs) {
        if (name == func_name) { label = sig; break; }
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

std::string LspServer::path_to_uri(const std::string& path) {
    namespace fs = std::filesystem;
    /// Normalise to absolute path
    std::error_code ec;
    std::string abs_path = fs::absolute(fs::path(path), ec).string();
    if (ec) abs_path = path;

    /// Replace backslashes with forward slashes
    std::string uri = "file:///";
    for (char c : abs_path) {
        if (c == '\\') {
            uri += '/';
        } else if (c == ' ') {
            uri += "%20";
        } else {
            uri += c;
        }
    }
    /**
     * On Unix, absolute paths start with '/', so we'd get file:////
     * Normalise to file:///
     */
    while (uri.size() > 8 && uri[7] == '/' && uri[8] == '/') {
        uri.erase(7, 1);
    }
    return uri;
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

    /// Read tab size (default 2)
    int64_t tab_size = 2;
    if (params.has("options")) {
        tab_size = params["options"].get_int("tabSize").value_or(2);
    }

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

    if (lines.empty()) return Value(Array{});

    /// Rebuild with correct indentation based on paren depth
    std::string formatted;
    int depth = 0;
    bool in_string = false;

    for (std::size_t i = 0; i < lines.size(); ++i) {
        const auto& raw_line = lines[i];

        /// Strip leading whitespace to get content
        auto first = raw_line.find_first_not_of(" \t");
        std::string content = (first == std::string::npos) ? "" : raw_line.substr(first);

        /// Trim trailing whitespace
        auto last = content.find_last_not_of(" \t");
        if (last != std::string::npos) content = content.substr(0, last + 1);

        /// If line starts with ')', decrease depth before indenting
        int pre_adjust = 0;
        if (!content.empty() && !in_string) {
            for (std::size_t c = 0; c < content.size(); ++c) {
                if (content[c] == ')') ++pre_adjust;
                else break;
            }
        }

        int indent_depth = std::max(0, depth - pre_adjust);
        std::string indent(static_cast<std::size_t>(indent_depth * tab_size), ' ');

        if (content.empty()) {
            formatted += "\n";
        } else if (in_string) {
            /// Inside a multi-line string, preserve as-is
            formatted += raw_line + "\n";
        } else {
            formatted += indent + content + "\n";
        }

        /// Update depth for next line by scanning this line
        for (std::size_t c = 0; c < content.size(); ++c) {
            char ch = content[c];
            if (in_string) {
                if (ch == '\\') { ++c; continue; }
                if (ch == '"') in_string = false;
                continue;
            }
            if (ch == '"') { in_string = true; continue; }
            if (ch == ';') break; ///< rest is comment
            if (ch == '(') ++depth;
            else if (ch == ')') --depth;
        }
        if (depth < 0) depth = 0;
    }

    /// Remove trailing newline if original didn't have one
    bool orig_trailing_newline = !source.empty() && source.back() == '\n';
    if (!orig_trailing_newline && !formatted.empty() && formatted.back() == '\n') {
        formatted.pop_back();
    }

    /// Return a single TextEdit replacing the entire document
    return json::array({
        json::object({
            {"range", range_to_json(Range{
                Position{0, 0},
                Position{static_cast<int64_t>(lines.size()), 0},
            })},
            {"newText", formatted},
        }),
    });
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

