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

namespace eta::lsp {

using eta::json::Value;
using eta::json::Object;
using eta::json::Array;

// ============================================================================
// Document store
// ============================================================================

struct TextDocument {
    std::string uri;
    std::string language_id;
    int64_t version{0};
    std::string content;
};

// ============================================================================
// LSP Diagnostic (subset)
// ============================================================================

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
    int severity{1}; // 1=Error, 2=Warning, 3=Info, 4=Hint
    std::string source{"eta"};
    std::string message;
};

// ============================================================================
// LSP Server
// ============================================================================

class LspServer {
public:
    LspServer();

    /// Main loop: reads JSON-RPC from stdin, writes to stdout. Blocks until exit.
    void run();

private:
    bool running_{true};
    bool initialized_{false};
    bool shutdown_requested_{false};

    // Document store: uri -> document
    std::unordered_map<std::string, TextDocument> documents_;

    // Module resolver — populated from ETA_MODULE_PATH + bundled stdlib
    interpreter::ModulePathResolver resolver_;

    // ── Transport ─────────────────────────────────────────────────────
    std::optional<std::string> read_message();
    void send_message(const Value& msg);
    void send_response(const Value& id, const Value& result);
    void send_error(const Value& id, int code, const std::string& message);
    void send_notification(const std::string& method, const Value& params);

    // ── Dispatch ──────────────────────────────────────────────────────
    void dispatch(const Value& msg);

    // ── LSP Methods ───────────────────────────────────────────────────
    Value handle_initialize(const Value& params);
    void handle_initialized(const Value& params);
    void handle_shutdown();
    void handle_exit();

    // Text document sync
    void handle_did_open(const Value& params);
    void handle_did_change(const Value& params);
    void handle_did_close(const Value& params);
    void handle_did_save(const Value& params);

    // Language features (Step 6)
    Value handle_hover(const Value& params);
    Value handle_definition(const Value& params);
    Value handle_completion(const Value& params);

    // ── Diagnostics ───────────────────────────────────────────────────
    void validate_document(const std::string& uri);
    void publish_diagnostics(const std::string& uri, const std::vector<LspDiagnostic>& diags);

    // ── Helpers ───────────────────────────────────────────────────────
    static Value position_to_json(const Position& p);
    static Value range_to_json(const Range& r);
    static Value diagnostic_to_json(const LspDiagnostic& d);

    /// Convert eta Span to LSP Range (0-based lines, 0-based columns)
    static Range span_to_range(uint32_t start_line, uint32_t start_col,
                               uint32_t end_line, uint32_t end_col);

    /// Find word at position in source text
    static std::string word_at_position(const std::string& text, int64_t line, int64_t character);

    /// Collect all defined symbols from source (for completion / hover)
    struct SymbolInfo {
        std::string name;
        std::string kind; // "define", "defun", "macro", "module", etc.
        int64_t line{0};
        int64_t character{0};
    };
    static std::vector<SymbolInfo> collect_symbols(const std::string& source);

    /// Initialise resolver_ from ETA_MODULE_PATH env var + bundled stdlib.
    void init_module_path();

    /// Read the source of a module (e.g. "std.core") from the search path.
    /// Returns nullopt if the module file cannot be found or opened.
    std::optional<std::string> resolve_module_source(const std::string& module_name);

    /// Load prelude.eta from the module search path and add all its module
    /// forms to all_forms, registering their names in seen_modules.
    /// This provides std.core, std.math, std.io, std.prelude etc. to the
    /// linker so that (import std.prelude) in user code resolves correctly.
    void preload_prelude(
        std::vector<eta::reader::parser::SExprPtr>& all_forms,
        std::unordered_set<std::string>& seen_modules);

    /// Recursively load all imported module files into all_forms so the linker
    /// can resolve cross-module references.  seen_modules must already contain
    /// the names of every module already present in all_forms.
    void preload_module_deps(
        std::vector<eta::reader::parser::SExprPtr>& all_forms,
        std::unordered_set<std::string>& seen_modules);
};

} // namespace eta::lsp

