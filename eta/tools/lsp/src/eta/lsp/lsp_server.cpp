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

namespace {

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

[[nodiscard]] std::string manifest_context_label(eta::package::ManifestContextKind kind) {
    switch (kind) {
        case eta::package::ManifestContextKind::StandalonePackage:
            return "standalone-package";
        case eta::package::ManifestContextKind::WorkspaceRoot:
            return "workspace-root";
        case eta::package::ManifestContextKind::WorkspaceMember:
            return "workspace-member";
        case eta::package::ManifestContextKind::WorkspaceNonMember:
            return "workspace-non-member";
    }
    return "unknown";
}

[[nodiscard]] std::optional<std::vector<std::string>>
load_etac_exports_for_module(const std::filesystem::path& etac_path,
                             std::string_view module_name) {
    std::ifstream in(etac_path, std::ios::in | std::ios::binary);
    if (!in.is_open()) return std::nullopt;

    runtime::memory::heap::Heap heap(8u * 1024u * 1024u);
    runtime::memory::intern::InternTable intern_table;
    runtime::vm::BytecodeSerializer serializer(heap, intern_table);
    auto etac = serializer.deserialize(in, /*expected_builtins=*/0);
    if (!etac) return std::nullopt;

    for (const auto& module : etac->modules) {
        if (module.name != module_name) continue;

        std::vector<std::string> exports;
        exports.reserve(module.export_bindings.size());
        for (const auto& ex : module.export_bindings) {
            if (!ex.name.empty()) exports.push_back(ex.name);
        }

        std::sort(exports.begin(), exports.end());
        exports.erase(std::unique(exports.begin(), exports.end()), exports.end());
        return exports;
    }
    return std::nullopt;
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
    /// Strip the slash before the drive letter only after decoding, because
    /// VS Code encodes the drive colon (file:///c%3A/...).
    if (decoded.size() >= 3u
        && decoded[0] == '/'
        && std::isalpha(static_cast<unsigned char>(decoded[1]))
        && decoded[2] == ':') {
        decoded.erase(0, 1);
    }
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
    const auto resolver_discovery_start =
        start_dir.empty() ? std::filesystem::path{} : canonicalize_path(start_dir);

    std::optional<std::filesystem::path> next_active_manifest_path;
    std::optional<std::filesystem::path> next_package_manifest_path;
    std::optional<std::filesystem::path> next_workspace_manifest_path;
    std::optional<std::filesystem::path> next_root_path;
    std::optional<std::string> next_context;
    if (!start_dir.empty()) {
        if (auto discovered = eta::package::discover_manifest_context(start_dir); discovered) {
            next_active_manifest_path = discovered->active_manifest_path;
            next_package_manifest_path = discovered->package_manifest_path;
            next_workspace_manifest_path = discovered->workspace_manifest_path;
            if (discovered->context.has_value()) {
                next_context = manifest_context_label(*discovered->context);
            }
            if (next_workspace_manifest_path.has_value()) {
                next_root_path = next_workspace_manifest_path->parent_path();
            } else if (next_active_manifest_path.has_value()) {
                next_root_path = next_active_manifest_path->parent_path();
            }
        } else {
            next_active_manifest_path = find_manifest_path(start_dir);
            if (next_active_manifest_path.has_value()) {
                next_package_manifest_path = next_active_manifest_path;
                next_root_path = next_active_manifest_path->parent_path();
            }
        }
    }

    if (active_manifest_path_ == next_active_manifest_path
        && package_manifest_path_ == next_package_manifest_path
        && workspace_manifest_path_ == next_workspace_manifest_path
        && workspace_root_path_ == next_root_path
        && workspace_context_ == next_context
        && resolver_discovery_start_ == resolver_discovery_start) {
        return;
    }

    if (!start_dir.empty()) {
        resolver_ = interpreter::ModulePathResolver::from_args_or_env_at("", start_dir);
    } else {
        resolver_ = interpreter::ModulePathResolver::from_args_or_env("");
    }
    active_manifest_path_ = std::move(next_active_manifest_path);
    package_manifest_path_ = std::move(next_package_manifest_path);
    workspace_manifest_path_ = std::move(next_workspace_manifest_path);
    workspace_root_path_ = std::move(next_root_path);
    workspace_context_ = std::move(next_context);
    resolver_discovery_start_ = resolver_discovery_start;

    completion_cache_loaded_ = false;
    prelude_symbols_.clear();
    module_path_symbols_.clear();
}

void LspServer::publish_workspace_package_diagnostics(const std::string& /*uri*/) {
    const auto manifest_path = workspace_manifest_path_.has_value()
        ? workspace_manifest_path_
        : package_manifest_path_;
    if (!manifest_path.has_value()) {
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

    const auto manifest_uri = path_to_uri(manifest_path->string());
    if (manifest_diagnostics_uri_ && *manifest_diagnostics_uri_ != manifest_uri) {
        publish_diagnostics(*manifest_diagnostics_uri_, {});
    }
    manifest_diagnostics_uri_ = manifest_uri;

    std::vector<LspDiagnostic> manifest_diags;
    auto document = eta::package::read_manifest_document(*manifest_path);
    if (!document) {
        manifest_diags.push_back(package_error_diagnostic(
            "eta-manifest", document.error().message, document.error().line));
    }

    const auto lockfile_root = workspace_manifest_path_.has_value()
        ? workspace_manifest_path_->parent_path()
        : manifest_path->parent_path();
    const auto lockfile_path = lockfile_root / "eta.lock";
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

    if (document && manifest_diags.empty()) {
        if (workspace_manifest_path_.has_value()) {
            auto workspace = eta::package::resolve_workspace_members(*workspace_manifest_path_);
            if (!workspace) {
                manifest_diags.push_back(package_error_diagnostic(
                    "eta-manifest", workspace.error().message, 0));
            } else {
                eta::package::ResolveOptions options;
                options.modules_root = workspace_manifest_path_->parent_path() / ".eta" / "modules";
                if (lockfile) options.lockfile = &*lockfile;
                auto resolved = eta::package::resolve_workspace_dependencies(*workspace, options);
                if (!resolved) {
                    manifest_diags.push_back(package_error_diagnostic(
                        "eta-manifest", resolved.error().message, 0));
                }
            }
        } else if (document->package.has_value()) {
            eta::package::ResolveOptions options;
            options.modules_root = manifest_path->parent_path() / ".eta" / "modules";
            if (lockfile) options.lockfile = &*lockfile;
            auto resolved = eta::package::resolve_dependencies(*manifest_path, options);
            if (!resolved) {
                manifest_diags.push_back(package_error_diagnostic(
                    "eta-manifest", resolved.error().message, 0));
            }
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

std::optional<std::string> LspServer::resolve_module_source(
    const std::string& module_name,
    const std::filesystem::path& importer_dir,
    std::filesystem::path* resolved_source_path) {
    auto path = resolver_.resolve(module_name, importer_dir);
    if (!path) return std::nullopt;

    auto source_path = *path;
    if (source_path.extension() == ".etac") {
        auto sibling = source_path;
        sibling.replace_extension(".eta");
        std::error_code ec;
        if (!std::filesystem::is_regular_file(sibling, ec) || ec) {
            if (resolved_source_path) *resolved_source_path = source_path;
            return std::nullopt;
        }
        source_path = std::move(sibling);
    }
    if (source_path.extension() != ".eta") return std::nullopt;
    if (resolved_source_path) *resolved_source_path = source_path;

    std::ifstream f(source_path);
    if (!f.is_open()) return std::nullopt;
    return std::string(std::istreambuf_iterator<char>(f),
                       std::istreambuf_iterator<char>());
}

void LspServer::preload_module_deps(
        std::vector<eta::reader::parser::SExprPtr>& all_forms,
        std::unordered_set<std::string>& seen_modules,
        std::unordered_map<std::string, std::vector<std::string>>& compiled_module_exports,
        const std::filesystem::path& importer_dir) {
    using namespace reader::utils;

    struct PendingImport {
        std::string name;
        std::filesystem::path importer_dir;
    };

    /// Collect imported module names from a form list
    auto collect_imports = [&](const std::vector<eta::reader::parser::SExprPtr>& forms,
                                const std::filesystem::path& source_dir,
                                std::vector<PendingImport>& out) {
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
                    if (!mod_name.empty() && !seen_modules.count(mod_name)) {
                        out.push_back(PendingImport{mod_name, source_dir});
                    }
                }
            }
        }
    };

    std::vector<PendingImport> to_load;
    collect_imports(all_forms, importer_dir, to_load);

    while (!to_load.empty()) {
        std::vector<PendingImport> next_load;
        for (const auto& pending : to_load) {
            if (seen_modules.count(pending.name)) continue;
            seen_modules.insert(pending.name);

            std::filesystem::path resolved_path;
            auto src = resolve_module_source(
                pending.name, pending.importer_dir, &resolved_path);
            if (!src) {
                auto resolved = resolver_.resolve(pending.name, pending.importer_dir);
                if (resolved && resolved->extension() == ".etac") {
                    auto sibling = *resolved;
                    sibling.replace_extension(".eta");
                    std::error_code ec;
                    if (!std::filesystem::is_regular_file(sibling, ec) || ec) {
                        auto exports = load_etac_exports_for_module(*resolved, pending.name);
                        if (exports) {
                            compiled_module_exports[pending.name] = std::move(*exports);
                        }
                    }
                }
                continue;
            }

            reader::lexer::Lexer lex(0, *src);
            reader::parser::Parser parser(lex);
            auto parsed = parser.parse_toplevel();
            if (!parsed) continue;

            reader::expander::Expander expander;
            auto expanded = expander.expand_many(*parsed);
            if (!expanded) continue;

            /// Collect transitive imports before moving forms
            collect_imports(*expanded, resolved_path.parent_path(), next_load);

            for (auto& f : *expanded) all_forms.push_back(std::move(f));
        }
        to_load = std::move(next_load);
    }
}

/**
 * Transport: Header-Content framing over stdio
 */

std::optional<std::string> LspServer::read_message() {
    return tooling::read_framed_message(in_, "eta-lsp");
}

void LspServer::send_message(const Value& msg) {
    tooling::write_framed_message(out_, json::to_string(msg));
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
    } else if (method == "textDocument/rangeFormatting") {
        auto result = handle_range_formatting(params);
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
    } else if (method == "workspace/executeCommand") {
        auto result = handle_execute_command(params);
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

Value LspServer::handle_initialize(const Value& params) {
    initialized_ = true;

    /// Editor-configurable tooling options (§8.2/§8.3). Document-format options
    /// still take precedence per request; these are the defaults.
    const auto& init_options = params["initializationOptions"];
    if (init_options.is_object()) {
        if (auto enabled = init_options["lintEnabled"]; enabled.is_bool()) {
            lint_enabled_ = enabled.as_bool();
        }
        if (auto width = init_options.get_int("maxWidth"); width && *width > 0) {
            format_max_width_ = static_cast<std::size_t>(*width);
        }
        if (auto indent = init_options.get_int("indent"); indent && *indent > 0) {
            format_indent_ = static_cast<std::size_t>(*indent);
        }
    }

    /// Negotiate the position encoding. UTF-16 is the default; only switch to
    /// UTF-8 when the client explicitly advertises it (§2.4).
    std::string negotiated_encoding = "utf-16";
    const auto& encodings = params["capabilities"]["general"]["positionEncodings"];
    if (encodings.is_array()) {
        for (const auto& enc : encodings.as_array()) {
            if (enc.is_string() && enc.as_string() == "utf-8") {
                position_encoding_ = cst::PositionEncoding::Utf8;
                negotiated_encoding = "utf-8";
                break;
            }
        }
    }

    /// Record the client's workspace folders as the fallback scope for manual
    /// workspace linting. Prefer `workspaceFolders`, then `rootUri`/`rootPath`.
    workspace_folders_.clear();
    const auto& folders = params["workspaceFolders"];
    if (folders.is_array()) {
        for (const auto& folder : folders.as_array()) {
            if (auto folder_uri = folder.get_string("uri")) {
                if (auto path = uri_to_path(*folder_uri)) {
                    workspace_folders_.push_back(std::move(*path));
                }
            }
        }
    }
    if (workspace_folders_.empty()) {
        if (auto root_uri = params.get_string("rootUri")) {
            if (auto path = uri_to_path(*root_uri)) {
                workspace_folders_.push_back(std::move(*path));
            }
        }
    }
    if (workspace_folders_.empty()) {
        if (auto root_path = params.get_string("rootPath"); root_path && !root_path->empty()) {
            workspace_folders_.emplace_back(*root_path);
        }
    }

    return json::object({
        {"capabilities", json::object({
            {"positionEncoding", negotiated_encoding},
            {"textDocumentSync", json::object({
                {"openClose", true},
                {"change", 2},  ///< 2 = Incremental sync; full replacement is accepted too
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
            {"documentRangeFormattingProvider", true},
            {"documentHighlightProvider", true},
            {"executeCommandProvider", json::object({
                {"commands", json::array({
                    "eta.lsp.lintFile", "eta.lsp.lintWorkspace"
                })},
            })},
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
    if (workspace_context_) {
        response.insert_or_assign("context", *workspace_context_);
    }
    if (active_manifest_path_) {
        response.insert_or_assign("manifestPath", active_manifest_path_->string());
    }
    if (package_manifest_path_) {
        response.insert_or_assign("packageManifestPath", package_manifest_path_->string());
    }
    if (workspace_manifest_path_) {
        response.insert_or_assign("workspaceManifestPath", workspace_manifest_path_->string());
    }

    std::optional<std::filesystem::path> lockfile_root;
    if (workspace_manifest_path_) {
        lockfile_root = workspace_manifest_path_->parent_path();
    } else if (active_manifest_path_) {
        lockfile_root = active_manifest_path_->parent_path();
    }
    if (lockfile_root) {
        const auto lockfile = *lockfile_root / "eta.lock";
        std::error_code ec;
        if (std::filesystem::is_regular_file(lockfile, ec) && !ec) {
            response.insert_or_assign("lockfilePath", lockfile.string());
        }
    }
    return Value(std::move(response));
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
} ///< namespace eta::lsp
