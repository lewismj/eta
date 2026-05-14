#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "eta/diagnostic/diagnostic.h"
#include "eta/interpreter/module_path.h"
#include "eta/native/actor_runtime.h"
#include "eta/native/runtime_binding.h"
#include "eta/native/sidecar_manager.h"
#include "eta/runtime/builtin_env.h"
#include "eta/runtime/extension_env.h"
#include "eta/runtime/port.h"
#include "eta/runtime/value_formatter.h"
#include "eta/runtime/vm/vm.h"
#include "eta/semantics/emitter.h"
#include "eta/semantics/optimization_pipeline.h"
#include "eta/session/compilation_session.h"
#include "eta/session/display_classifier.h"
#include "eta/session/eval_display.h"
#include "eta/session/eval_engine.h"
#include "eta/session/etac_loader.h"
#include "eta/session/repl_controller.h"
#include "eta/session/runtime_primitives.h"
#include "eta/session/source_file_registry.h"

namespace eta::nng {
class SessionActorRuntime;
}

namespace eta::session {

namespace fs = std::filesystem;
using eta::interpreter::ModulePathResolver;

/**
 * @brief Compilation + execution driver for the eta language.
 *
 * Owns the full runtime state (Heap, InternTable, VM, etc.) and provides
 * VM globals and linker state, so definitions persist across REPL inputs.
 */
class Driver : private ReplController::ReplRuntime,
               private EvalEngine::Host,
               private CompilationSession::Host,
               private EtacLoader::Host,
               private native::NativeSidecarManager::Host {
public:
    static constexpr std::size_t DEFAULT_HEAP_SOFT_LIMIT_BYTES =
        150u * 1024u * 1024u;
    static constexpr std::size_t DEFAULT_CHILD_HEAP_SOFT_LIMIT_BYTES =
        50u * 1024u * 1024u;

    /**
     * Parse a human-readable heap size from an environment variable.
     *
     * Supported suffixes (case-insensitive): K (KiB), M (MiB), G (GiB).
     * Examples: "512K", "4M", "2G".
     *
     * @param env_var Name of the environment variable to read.
     * @param default_val Returned when the variable is absent, empty, or invalid.
     */
    static std::size_t parse_heap_env_var(
        const char* env_var,
        std::size_t default_val = DEFAULT_HEAP_SOFT_LIMIT_BYTES) noexcept;

    explicit Driver(
        ModulePathResolver resolver = ModulePathResolver{},
        std::size_t heap_bytes = DEFAULT_HEAP_SOFT_LIMIT_BYTES,
        std::string etai_path = {},
        std::vector<std::string> command_line_arguments = {});

    /// Non-copyable, non-movable (owns references captured in lambdas)
    Driver(const Driver&) = delete;
    Driver& operator=(const Driver&) = delete;
    Driver(Driver&&) = delete;
    Driver& operator=(Driver&&) = delete;

    ~Driver();

    /// Result of a load_prelude() call.
    struct PreludeResult {
        bool found{false};  ///< Deprecated: retained for API compatibility.
        bool loaded{false}; ///< Deprecated: retained for API compatibility.
        fs::path path;      ///< Deprecated: retained for API compatibility.
    };

    using CompileModuleEntry = CompilationSession::CompileModuleEntry;
    using CompileResult = CompilationSession::CompileResult;

    /**
     * @brief Legacy bootstrap hook.
     *
     * `std.prelude` has been removed from the runtime contract. This method now
     * only ensures package sidecars are loaded and otherwise performs no module
     * auto-load behavior.
     */
    PreludeResult load_prelude();

    /// Check whether a module with the given name has been executed.
    [[nodiscard]] bool has_module(const std::string& name) const override;

    /**
     * @brief Remove cached linker/execution state for one module.
     *
     * This clears the module from the executed-module set, removes matching
     * top-level module forms from the accumulated linker input, and drops any
     * recorded global binding names for that module.
     *
     * @return true when any cached entry was removed.
     */
    bool clear_module_cache(const std::string& module_name);

    /**
     * @brief Read, compile and execute a .eta file.
     * @return true on success, false on error (diagnostics emitted to engine).
     */
    bool run_file(const fs::path& path);

    /**
     * @brief Compile a .eta file without executing it.
     *
     * This path preserves semantic analysis and linker behavior used by run_file,
     * but skips VM execution. Imported dependencies are still
     * executed normally (they must populate globals for semantic analysis).
     *
     * @return CompileResult on success, std::nullopt on error.
     */
    std::optional<CompileResult> compile_file(const fs::path& path);

    /**
     * @brief Compile and execute a source string (e.g. from the REPL).
     *
     * Incremental: shares VM globals, linker state, and registry with
     * all previous invocations so definitions persist.
     *
     * @param source The eta source text (one or more top-level forms).
     * @param result If non-null, receives the last expression value.
     * @param result_binding If non-empty, look up this binding name in
     * the last module's globals to retrieve the result
     * (module init functions return Nil by design).
     * @return true on success, false on error.
     */
    bool run_source(std::string_view source,
                    runtime::nanbox::LispVal* result = nullptr,
                    const std::string& result_binding = {}) override;

    using StreamSink = native::ActorRuntime::StreamSink;
    using ActorEvent = native::ActorRuntime::ActorEvent;

    /**
     * @brief Evaluate REPL input and return a formatted output string.
     *
     * The source is split into top-level forms and wrapped into an internal
     * module so globals persist across calls with normal REPL shadowing rules.
     *
     * @param source Source text submitted by the caller.
     * @param out Receives formatted output for the final expression (if any).
     * @return true on success, false on error (diagnostics are populated).
     */
    bool eval_string(std::string source, std::string& out);

    /**
     * @brief Completion payload for front-ends.
     */
    using CompletionResult = ReplController::CompletionResult;

    /**
     * @brief Collect completion matches at @p cursor_pos in @p source.
     *
     * Matches are sourced from keywords, builtin primitives, currently loaded
     * module/global bindings, and module names discoverable on the module path.
     */
    [[nodiscard]] CompletionResult completions_at(
        const std::string& source,
        std::size_t cursor_pos) const;

    /**
     * @brief Return Markdown hover text for a symbol.
     *
     * Returns an empty string when no hover content is available.
     */
    [[nodiscard]] std::string hover_at(const std::string& symbol) const;

    /**
     * @brief Check whether @p src forms a complete evaluable expression.
     *
     * Completeness accounts for nested block comments, string literals with
     * escapes, and parenthesis depth.
     *
     * @param src Source text to inspect.
     * @param indent_hint Optional indentation hint for continuation prompts.
     * @return true when the source is complete, false when more input is needed.
     */
    [[nodiscard]] bool is_complete_expression(
        const std::string& src,
        std::string* indent_hint = nullptr) const;

    /**
     * @brief Request interruption of the currently executing VM run.
     */
    void request_interrupt();

    /**
     * @brief Evaluate source and return a structured display value.
     *
     * @param source Source text to evaluate.
     * @return Structured display payload for front-end rendering.
     */
    [[nodiscard]] DisplayValue eval_to_display(const std::string& source);

    /**
     * @brief Override VM stdout/stderr routing with callback sinks.
     *
     * Passing an empty sink leaves the current port unchanged.
     */
    void set_stream_sinks(StreamSink stdout_sink, StreamSink stderr_sink);

    /**
     * @brief Register a listener for actor lifecycle events.
     *
     * Events are emitted for spawned actor threads as they start and exit.
     */
    void on_actor_lifecycle(std::function<void(const ActorEvent&)> on_event);

    /// Access the diagnostic engine (for printing / LSP forwarding).
    [[nodiscard]] diagnostic::DiagnosticEngine& diagnostics() noexcept override;
    [[nodiscard]] const diagnostic::DiagnosticEngine& diagnostics() const noexcept;

    /// Suitable for passing to format_diagnostic / DiagnosticEngine::print_all.
    [[nodiscard]] diagnostic::FileResolver file_resolver() const;

    /// Access the module path resolver.
    [[nodiscard]] ModulePathResolver& resolver() noexcept;

    runtime::vm::VM& vm() noexcept override;
    const runtime::vm::VM& vm() const noexcept;

    semantics::BytecodeFunctionRegistry& registry() noexcept override;
    const semantics::BytecodeFunctionRegistry& registry() const noexcept;

    [[nodiscard]] const fs::path* path_for_file_id(uint32_t id) const noexcept;

    /**
     * Install a custom output port so that display/write/newline go through
     * the given port rather than falling back to std::cout.
     * Typical use in the DAP: pass a CallbackPort that fires send_event().
     */
    void set_output_port(std::shared_ptr<runtime::Port> port);

    /// Install a custom error port (used by eprintln / error output).
    void set_error_port(std::shared_ptr<runtime::Port> port);

    /**
     * Pre-register a file path so that its file_id is known before the file
     * is actually loaded. The DAP uses this to install breakpoints BEFORE
     * the VM thread starts running. If the path is already registered the
     * existing id is returned unchanged.
     */
    uint32_t ensure_file_id(const fs::path& path);

    /// Input is normalised before lookup so case differences on Windows are handled.
    [[nodiscard]] uint32_t file_id_for_path(const std::string& path) const;

    /**
     * Return every executable source line currently known for a file.
     *
     * The set is collected from emitted bytecode source maps across the
     * function registry and is suitable for DAP breakpointLocations.
     */
    [[nodiscard]] std::set<uint32_t> valid_lines_for(uint32_t file_id) const;

    /// Format a runtime value for display.
    [[nodiscard]] std::string format_value(
        runtime::nanbox::LispVal v,
        runtime::FormatMode mode = runtime::FormatMode::Write) override;

    /**
     * @brief Install the `--mailbox` socket for a spawned child process.
     *
     * Called by main_etai.cpp when the `--mailbox <endpoint>` argument is
     * present. Creates a PAIR socket, dials the endpoint (connecting to the
     * parent's listening socket), and stores the socket as `current-mailbox`.
     *
     * @return true on success, false if the dial fails.
     */
    bool install_mailbox(const std::string& endpoint);

    /// Access the process manager (for DAP child process tree view).
    [[nodiscard]] native::ActorProcessManager* process_manager() noexcept;
    [[nodiscard]] const native::ActorProcessManager* process_manager() const noexcept;

    /// Return the current mailbox socket (Nil if not a spawned child).
    [[nodiscard]] runtime::nanbox::LispVal mailbox() const noexcept;

    /// Populated during compilation for debugger display.
    [[nodiscard]] const std::unordered_map<uint32_t, std::string>&
    global_names() const noexcept override;

    runtime::memory::heap::Heap& heap() noexcept override;
    const runtime::memory::heap::Heap& heap() const noexcept;

    /// Direct access to the intern table.
    runtime::memory::intern::InternTable& intern_table() noexcept override;

    semantics::OptimizationPipeline& optimization_pipeline() noexcept override;

    /// Number of registered builtins (used to embed in .etac for mismatch detection).
    [[nodiscard]] std::size_t builtin_count() const noexcept override;

    /**
     * @brief Deterministic hash of the currently registered extension environment.
     *
     * Returns 0 when no extension primitives are registered.
     */
    [[nodiscard]] std::uint64_t extension_env_hash() const noexcept override;

    /**
     * @brief Register one extension primitive before analyzing source modules.
     *
     * Extension primitives occupy global slots immediately after core builtins.
     * This API is intended for sidecar-backed registration and test fixtures.
     */
    void register_extension_primitive(
        std::string name,
        uint32_t arity,
        bool has_rest,
        runtime::types::PrimitiveFunc func) override;

    /// Number of registered extension primitives.
    [[nodiscard]] std::size_t extension_primitive_count() const noexcept;

    /**
     * @brief Load package-managed native sidecars for one start directory.
     *
     * Returns true when sidecar loading succeeded or no package sidecars are
     * required for the discovered context.
     */
    bool load_package_sidecars(const fs::path& start_dir);

    /**
     * @brief Load and execute a pre-compiled .etac file.
     * @return true on success, false on error (diagnostics emitted to engine).
     */
    bool run_etac_file(const fs::path& path);

private:
    friend class eta::nng::SessionActorRuntime;

    [[nodiscard]] bool can_register_extension_primitives() const noexcept override;
    void register_builtin_primitive(std::string name,
                                    uint32_t arity,
                                    bool has_rest,
                                    runtime::types::PrimitiveFunc func) override;
    [[nodiscard]] bool has_builtin_primitive(std::string_view name) const override;
    void overwrite_builtin_primitive(std::string_view name,
                                     runtime::types::PrimitiveFunc func) override;
    void invalidate_primitive_installer() override;
    void emit_sidecar_error(std::string message) override;
    bool ensure_package_sidecars_loaded(std::optional<fs::path> start_dir) override;
    [[nodiscard]] std::size_t total_primitive_count() const noexcept override;
    [[nodiscard]] runtime::BuiltinEnvironment& builtins() noexcept override;
    [[nodiscard]] runtime::ExtensionEnvironment& extensions() noexcept override;
    [[nodiscard]] RuntimePrimitiveInstaller& primitive_installer() noexcept override;
    bool run_module_file(const fs::path& path) override;
    bool run_source_file(const fs::path& path) override;
    [[nodiscard]] std::optional<fs::path> resolve_import_path(
        const std::string& module_name,
        bool* shadow_conflict = nullptr) override;
    [[nodiscard]] std::string diagnostics_to_string() const override;
    [[nodiscard]] std::vector<std::string> discover_module_names() const override;
    void collect_garbage_with_registry_roots();

    /**
     * Auto-detect the path to the etai binary at startup.
     * Prefers a sibling executable next to the current process image and
     * falls back to PATH lookup.
     */
    static std::string detect_etai_path();

    /// File ID registry used by diagnostics and debugger file lookups.
    uint32_t allocate_file_id(const std::string& raw_path);
    bool hydrate_executed_module_source(const std::string& module_name);

    /// Convert a LinkError into a Diagnostic and emit it.
    void emit_link_error(const reader::LinkError& e) override;

    /// Convert a RuntimeError variant into a Diagnostic and emit it.
    void emit_runtime_error(const runtime::error::RuntimeError& err) override;

    ModulePathResolver resolver_;
    runtime::memory::heap::Heap heap_;
    runtime::memory::intern::InternTable intern_table_;
    semantics::BytecodeFunctionRegistry registry_;
    runtime::BuiltinEnvironment builtins_;
    runtime::ExtensionEnvironment extensions_;
    RuntimePrimitiveInstaller primitive_installer_;
    native::NativeSidecarManager sidecar_manager_;
    native::SidecarRuntimeBindingV1 sidecar_runtime_binding_{};
    runtime::vm::VM vm_;

    diagnostic::DiagnosticEngine diag_engine_;

    /// IR-level optimization pipeline (runs between analyze and emit)
    semantics::OptimizationPipeline optimization_pipeline_;

    std::unique_ptr<native::ActorRuntime> actor_runtime_;
    std::string etai_path_;
    std::string module_search_path_;
    std::vector<std::string> command_line_arguments_;
    CompilationSession compilation_;
    EtacLoader etac_loader_;
    EvalEngine eval_engine_;
    DisplayClassifier display_classifier_;
    ReplController repl_controller_;
    SourceFileRegistry source_files_;
};

} // namespace eta::session
