#pragma once

#include <cstdint>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "eta/util/json.h"
#include "eta/interpreter/module_path.h"
#include "eta/reader/parser.h"
#include "eta/cst/source.h"

namespace eta::lsp {

using eta::json::Value;
using eta::json::Object;
using eta::json::Array;

/**
 * Document store
 */

struct TextDocument {
    std::string uri;
    std::string language_id;
    int64_t version{0};
    std::string content;
};

/**
 * LSP Diagnostic (subset)
 */

struct Position {
    int64_t line{0};
    int64_t character{0};
};

struct Range {
    Position start;
    Position end;
};

struct LspDiagnostic {
    Range range;
    int severity{1}; ///< 1=Error, 2=Warning, 3=Info, 4=Hint
    std::string source{"eta"};
    std::string message;
};

/**
 * LSP Server
 */

class LspServer {
public:
    /// Production constructor: communicates over the process stdin/stdout.
    LspServer();

    /// Testable constructor: communicates over the supplied streams.
    LspServer(std::istream& in, std::ostream& out);

    /// Main loop: reads JSON-RPC from stdin, writes to stdout. Blocks until exit.
    void run();

    /**
     * Collect all defined symbols from source (for completion / hover / documentSymbol).
     * When capture_signature is true, also captures the parameter list text
     * for defun/define forms (e.g. "(f g)" from "(defun compose (f g) ...)").
     */
    struct SymbolInfo {
        std::string name;
        std::string kind; ///< "define", "defun", "macro", "module", etc.
        std::string signature;
        std::string module_name; ///< originating module (empty for document-local)
        std::string file_path; ///< filesystem path of the originating file (empty for current doc)
        int64_t line{0};
        int64_t character{0};
    };
    static std::vector<SymbolInfo> collect_symbols(const std::string& source, bool capture_signature = false);

    /**
     * Find all occurrences of a symbol name in the given source text.
     * Returns a vector of Range for each occurrence.
     */
    static std::vector<Range> find_all_occurrences(const std::string& source,
                                                    const std::string& symbol);

    /**
     * Find nested S-expression ranges enclosing the given position.
     * Returns ranges from innermost to outermost (each is a balanced parenthesized form).
     */
    static std::vector<Range> enclosing_sexp_ranges(
        const std::string& source, int64_t line, int64_t character);

    /// Convert a filesystem path to a file:// URI.
    static std::string path_to_uri(const std::string& path);

private:
    std::istream& in_;
    std::ostream& out_;

    bool running_{true};
    bool initialized_{false};
    bool shutdown_requested_{false};

    /// Document store: uri -> document
    std::unordered_map<std::string, TextDocument> documents_;

    interpreter::ModulePathResolver resolver_;

    /// Transport
    std::optional<std::string> read_message();
    void send_message(const Value& msg);
    void send_response(const Value& id, const Value& result);
    void send_error(const Value& id, int code, const std::string& message);
    void send_notification(const std::string& method, const Value& params);

    /// Dispatch
    void dispatch(const Value& msg);

    /// LSP Methods
    Value handle_initialize(const Value& params);
    void handle_initialized(const Value& params);
    void handle_shutdown();
    void handle_exit();
    Value handle_lockfile_explain(const Value& params);

    /**
     * workspace/executeCommand dispatcher. Routes the integrated lint commands
     * to the same validation pipeline that powers live diagnostics so there is
     * a single authoritative editor diagnostic path.
     */
    Value handle_execute_command(const Value& params);

    /**
     * `eta.lsp.lintFile`: force a fresh validation of one open document and
     * republish its diagnostics. Returns a structured summary for the client.
     */
    Value lint_file_command(const Value& arguments);

    /**
     * `eta.lsp.lintWorkspace`: discover and validate every `*.eta` file under
     * the supplied (or resolved) workspace roots, publishing diagnostics for
     * each and clearing diagnostics that are no longer in scope. Returns a
     * structured summary for the client.
     */
    Value lint_workspace_command(const Value& arguments);

    /// Text document sync
    void handle_did_open(const Value& params);
    void handle_did_change(const Value& params);
    void handle_did_close(const Value& params);
    void handle_did_save(const Value& params);

    /// Language features
    Value handle_hover(const Value& params);
    Value handle_definition(const Value& params);
    Value handle_completion(const Value& params);
    Value handle_document_symbol(const Value& params);
    Value handle_references(const Value& params);
    Value handle_rename(const Value& params);
    Value handle_signature_help(const Value& params);
    Value handle_folding_range(const Value& params);
    Value handle_workspace_symbol(const Value& params);
    Value handle_selection_range(const Value& params);
    Value handle_semantic_tokens_full(const Value& params);
    Value handle_formatting(const Value& params);
    Value handle_range_formatting(const Value& params);
    Value handle_document_highlight(const Value& params);
    Value handle_inlay_hint(const Value& params);

    /// Diagnostics
    void validate_document(const std::string& uri);
    void publish_diagnostics(const std::string& uri, const std::vector<LspDiagnostic>& diags);

    /**
     * Run the full reader -> expand -> link -> semantic pipeline plus the shared
     * linter over a source snapshot and return the merged diagnostics. This is
     * the single compute path shared by live validation and the manual lint
     * commands; it never publishes or touches the document store.
     */
    std::vector<LspDiagnostic> compute_document_diagnostics(const std::string& source);

    /**
     * Run the shared linter library (eta_linter_lib) over a document snapshot and
     * append its findings to `diags`. Reader/parse failures produce no lint
     * findings here; the compiler pipeline owns those diagnostics (§5.1).
     */
    void collect_lint_diagnostics(const std::string& source, std::vector<LspDiagnostic>& diags);

    /// Helpers
    static Value position_to_json(const Position& p);
    static Value range_to_json(const Range& r);
    static Value diagnostic_to_json(const LspDiagnostic& d);

    /// Convert eta Span to LSP Range (0-based lines, 0-based columns)
    static Range span_to_range(uint32_t start_line, uint32_t start_col,
                               uint32_t end_line, uint32_t end_col);

    /// Find word at position in source text
    static std::string word_at_position(const std::string& text, int64_t line, int64_t character);

    /**
     * Find the end position {line, col} of the S-expression that begins at
     * Returns {start_line, start_col} when no balanced close is found.
     */
    static std::pair<int64_t, int64_t> sexp_end(
        const std::string& source, int64_t start_line, int64_t start_col);



    /// Initialise resolver_ from ETA_MODULE_PATH env var + bundled stdlib.
    void init_module_path();
    void ensure_workspace_for_uri(const std::string& uri);
    void publish_workspace_package_diagnostics(const std::string& uri);
    static std::optional<std::filesystem::path> uri_to_path(const std::string& uri);
    static std::optional<std::filesystem::path> find_manifest_path(std::filesystem::path start_dir);

    /**
     * Recursively collect every `*.eta` file rooted at the given workspace
     * folders in stable sorted path order, skipping build/vendor/cache and
     * hidden directories so manual workspace lint never descends into compiled
     * caches or dependency output.
     */
    static std::vector<std::filesystem::path> discover_workspace_eta_files(
        const std::vector<std::filesystem::path>& roots);

    /// True for build/vendor/cache or hidden directory names that workspace
    /// traversal must not descend into.
    static bool is_ignored_directory_name(const std::string& name);

    /**
     * Read the source of a module (e.g. "std.core") from the search path.
     * Returns nullopt if the module file cannot be found or opened.
     */
    std::optional<std::string> resolve_module_source(const std::string& module_name);

    /**
     * Recursively load all imported module files into all_forms so the linker
     * can resolve cross-module references.  seen_modules must already contain
     * the names of every module already present in all_forms.  For imports that
     * resolve to `.etac` without sibling source, populate compiled_module_exports
     * with module export lists so the linker can index compiled modules too.
     */
    void preload_module_deps(
        std::vector<eta::reader::parser::SExprPtr>& all_forms,
        std::unordered_set<std::string>& seen_modules,
        std::unordered_map<std::string, std::vector<std::string>>& compiled_module_exports);

    /**
     * Completion caches
     * Cached symbols from module-path `.eta` files (populated lazily).
     */
    bool completion_cache_loaded_{false};
    std::vector<SymbolInfo> prelude_symbols_;
    std::vector<SymbolInfo> module_path_symbols_;

    /// Populate cached completion symbols from disk.
    void load_completion_cache();

    /// Scan all .eta files in the module search path and collect their symbols.
    void scan_module_path_symbols();

    /// Validation content cache
    std::unordered_map<std::string, std::string> last_validated_content_;
    std::optional<std::filesystem::path> active_manifest_path_;    std::optional<std::filesystem::path> package_manifest_path_;
    std::optional<std::filesystem::path> workspace_manifest_path_;
    std::optional<std::filesystem::path> workspace_root_path_;
    std::optional<std::string> workspace_context_;
    std::optional<std::string> manifest_diagnostics_uri_;
    std::optional<std::string> lockfile_diagnostics_uri_;

    /// Workspace folder roots advertised by the client at initialize time,
    /// used as the fallback scope for `eta.lsp.lintWorkspace`.
    std::vector<std::filesystem::path> workspace_folders_;

    /// URIs that received diagnostics during the last manual workspace lint
    /// pass, so a later run can clear stale results for files no longer in scope.
    std::unordered_set<std::string> workspace_lint_uris_;

    /// Editor-configurable tooling options supplied through initializationOptions
    /// (§8.2/§8.3). The canonical formatter uses spaces, so tabs are rejected.
    bool lint_enabled_{true};
    std::size_t format_indent_{2};
    std::size_t format_max_width_{100};
    /// Negotiated LSP position encoding; UTF-16 unless UTF-8 is agreed (§2.4).
    cst::PositionEncoding position_encoding_{cst::PositionEncoding::Utf16};
};

} ///< namespace eta::lsp

