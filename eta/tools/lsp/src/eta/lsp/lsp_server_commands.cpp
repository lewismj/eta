/// LSP workspace/executeCommand: lint file and lint workspace commands.
#include "lsp_server.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_set>


/// Eta compiler includes
#include "eta/reader/expander.h"
#include "eta/reader/module_linker.h"
#include "eta/runtime/memory/heap.h"
#include "eta/runtime/vm/bytecode_serializer.h"

/// Shared tooling libraries (§8.2): the LSP links the formatter and linter
/// libraries directly and never shells out to eta_fmt / eta_lint.
#include "lsp_server_internal.h"

namespace eta::lsp {

using namespace eta::json;

/**
 * workspace/executeCommand
 */

Value LspServer::handle_execute_command(const Value& params) {
    const auto command = params.get_string("command").value_or("");
    const auto& arguments = params["arguments"];
    if (command == "eta.lsp.lintFile") {
        return lint_file_command(arguments);
    }
    if (command == "eta.lsp.lintWorkspace") {
        return lint_workspace_command(arguments);
    }
    return json::object({
        {"ok", false},
        {"error", "unknown command: " + command},
    });
}

namespace {

/// Extract a document URI from a single executeCommand argument that is either a
/// bare URI string or an object carrying a `uri` field.
[[nodiscard]] std::string argument_uri(const Value& arg) {
    if (arg.is_string()) return arg.as_string();
    if (arg.is_object()) return arg.get_string("uri").value_or("");
    return {};
}

} // namespace

Value LspServer::lint_file_command(const Value& arguments) {
    std::string uri;
    if (arguments.is_array() && !arguments.as_array().empty()) {
        uri = argument_uri(arguments.as_array().front());
    }
    if (uri.empty()) {
        return json::object({
            {"ok", false},
            {"error", "lintFile requires a document URI argument"},
        });
    }

    auto it = documents_.find(uri);
    if (it == documents_.end()) {
        return json::object({
            {"ok", false},
            {"uri", uri},
            {"error", "document is not open"},
        });
    }

    /// Re-run the exact same validation path used for live diagnostics, forcing a
    /// recompute even when the buffer is unchanged so "Lint File" always refreshes.
    ensure_workspace_for_uri(uri);
    publish_workspace_package_diagnostics(uri);
    const auto& source = it->second.content;
    const auto document_path = uri_to_path(uri);
    auto diags = compute_document_diagnostics(
        source, document_path ? document_path->parent_path() : std::filesystem::path{});
    last_validated_content_[uri] = source;
    publish_diagnostics(uri, diags);

    return json::object({
        {"ok", true},
        {"uri", uri},
        {"diagnosticsCount", static_cast<int64_t>(diags.size())},
    });
}

Value LspServer::lint_workspace_command(const Value& arguments) {
    const auto start = std::chrono::steady_clock::now();

    /// Resolve the lint scope: prefer folders passed by the client, then the
    /// workspace folders captured at initialize, then the resolved manifest root.
    std::vector<std::filesystem::path> roots;
    auto add_root = [&](const std::filesystem::path& path) {
        if (path.empty()) return;
        auto canonical = canonicalize_path(path);
        if (std::find(roots.begin(), roots.end(), canonical) == roots.end()) {
            roots.push_back(std::move(canonical));
        }
    };
    if (arguments.is_array()) {
        for (const auto& arg : arguments.as_array()) {
            const auto folder_uri = argument_uri(arg);
            if (folder_uri.empty()) continue;
            if (auto path = uri_to_path(folder_uri)) add_root(*path);
        }
    }
    if (roots.empty()) {
        for (const auto& folder : workspace_folders_) add_root(folder);
    }
    if (roots.empty() && workspace_root_path_) {
        add_root(*workspace_root_path_);
    }

    if (roots.empty()) {
        return json::object({
            {"ok", false},
            {"error", "no workspace folder is open"},
        });
    }

    const auto files = discover_workspace_eta_files(roots);

    std::unordered_set<std::string> visited;
    int64_t files_with_diagnostics = 0;
    int64_t diagnostics_count = 0;
    for (const auto& file : files) {
        const auto uri = path_to_uri(file.string());

        std::string source;
        if (auto open = documents_.find(uri); open != documents_.end()) {
            source = open->second.content;
        } else {
            std::ifstream in(file, std::ios::in | std::ios::binary);
            if (!in) continue;
            std::ostringstream ss;
            ss << in.rdbuf();
            source = ss.str();
        }

        ensure_workspace_for_uri(uri);
        auto diags = compute_document_diagnostics(source, file.parent_path());
        last_validated_content_[uri] = source;
        publish_diagnostics(uri, diags);
        visited.insert(uri);
        if (!diags.empty()) {
            ++files_with_diagnostics;
            diagnostics_count += static_cast<int64_t>(diags.size());
        }
    }

    /// Clear diagnostics for files linted by the previous manual workspace pass
    /// that are no longer in scope, so stale results never linger.
    for (const auto& previous : workspace_lint_uris_) {
        if (visited.find(previous) == visited.end()) {
            publish_diagnostics(previous, {});
        }
    }
    workspace_lint_uris_ = std::move(visited);

    const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start).count();

    return json::object({
        {"ok", true},
        {"filesVisited", static_cast<int64_t>(files.size())},
        {"filesWithDiagnostics", files_with_diagnostics},
        {"diagnosticsCount", diagnostics_count},
        {"durationMs", static_cast<int64_t>(elapsed_ms)},
        {"cancelled", false},
    });
}

bool LspServer::is_ignored_directory_name(const std::string& name) {
    if (name.empty()) return false;
    /// Hidden directories (e.g. .git, .eta module cache, .cache) are never source.
    if (name.front() == '.') return true;
    static const std::unordered_set<std::string> ignored = {
        "build", "build-release", "out", "node_modules", "target", "_deps",
        "dist", "cmake-build-debug", "cmake-build-release",
    };
    return ignored.find(name) != ignored.end();
}

std::vector<std::filesystem::path> LspServer::discover_workspace_eta_files(
    const std::vector<std::filesystem::path>& roots) {
    std::vector<std::filesystem::path> files;
    std::unordered_set<std::string> seen;

    for (const auto& root : roots) {
        std::error_code ec;
        if (!std::filesystem::exists(root, ec) || ec) continue;

        if (std::filesystem::is_regular_file(root, ec) && !ec) {
            if (root.extension() == ".eta") {
                if (seen.insert(canonicalize_path(root).string()).second) {
                    files.push_back(root);
                }
            }
            continue;
        }

        std::filesystem::recursive_directory_iterator it(
            root, std::filesystem::directory_options::skip_permission_denied, ec);
        if (ec) continue;
        const std::filesystem::recursive_directory_iterator end;
        for (; it != end; it.increment(ec)) {
            if (ec) { ec.clear(); continue; }

            std::error_code entry_ec;
            if (it->is_directory(entry_ec) && !entry_ec) {
                if (is_ignored_directory_name(it->path().filename().string())) {
                    it.disable_recursion_pending();
                }
                continue;
            }
            if (it->is_regular_file(entry_ec) && !entry_ec
                && it->path().extension() == ".eta") {
                if (seen.insert(canonicalize_path(it->path()).string()).second) {
                    files.push_back(it->path());
                }
            }
        }
    }

    std::sort(files.begin(), files.end());
    return files;
}
} ///< namespace eta::lsp
