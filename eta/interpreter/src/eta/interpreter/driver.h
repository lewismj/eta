#pragma once

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

#include "eta/reader/lexer.h"
#include "eta/reader/parser.h"
#include "eta/reader/expander.h"
#include "eta/reader/module_linker.h"
#include "eta/semantics/semantic_analyzer.h"
#include "eta/semantics/emitter.h"
#include "eta/semantics/optimization_pipeline.h"
#include "eta/runtime/vm/vm.h"
#include "eta/runtime/vm/bytecode_serializer.h"
#include "eta/runtime/vm/disassembler.h"
#include "eta/runtime/builtin_env.h"
#include "eta/runtime/core_primitives.h"
#include "eta/runtime/io_primitives.h"
#include "eta/runtime/port_primitives.h"
#include "eta/runtime/value_formatter.h"
#include "eta/diagnostic/diagnostic.h"

#ifdef ETA_HAS_TORCH
#include <eta/torch/torch_primitives.h>
#endif

#ifdef ETA_HAS_NNG
#include <nng/nng.h>
#include <nng/protocol/pair0/pair.h>
#include <eta/nng/nng_socket_ptr.h>
#include <eta/nng/nng_factory.h>
#include <eta/nng/nng_primitives.h>
#include <eta/nng/process_mgr.h>
#endif

#include "eta/interpreter/module_path.h"

namespace eta::interpreter {

namespace fs = std::filesystem;

/**
 * @brief Compilation + execution driver for the eta language.
 *
 * Owns the full runtime state (Heap, InternTable, VM, etc.) and provides
 * incremental execution — successive calls to run_source() share the same
 * VM globals and linker state, so definitions persist across REPL inputs.
 *
 * Pipeline:  source → lex → parse → expand → link → analyze → emit → execute
 */
class Driver {
public:
    explicit Driver(ModulePathResolver resolver,
                    std::size_t heap_bytes = 4 * 1024 * 1024,
                    std::string etai_path  = {})
        : resolver_(std::move(resolver)),
          heap_(heap_bytes),
          intern_table_(),
          registry_(),
          builtins_(),
          vm_(heap_, intern_table_),
          diag_engine_(),
          next_file_id_(1) // 0 is reserved for REPL / anonymous input
    {
        // core primitives — pass vm_ so map/for-each support closures
        runtime::register_core_primitives(builtins_, heap_, intern_table_, &vm_);
        // port primitives (require VM reference)
        runtime::register_port_primitives(builtins_, heap_, intern_table_, vm_);
        // I/O primitives (require VM for port-aware display/newline)
        runtime::register_io_primitives(builtins_, heap_, intern_table_, vm_);

#ifdef ETA_HAS_TORCH
        // Torch/NN/Optim primitives (CPU or CUDA, detected at runtime)
        torch_bindings::register_torch_primitives(builtins_, heap_, intern_table_, &vm_);
#endif

#ifdef ETA_HAS_NNG
        // Detect etai binary path if not explicitly supplied
        if (etai_path.empty()) etai_path = detect_etai_path();
        etai_path_ = etai_path;

        // Build colon-separated module search path to forward to child processes.
        // Child receives this via ETA_MODULE_PATH only if ETA_MODULE_PATH is not
        // already set in the environment (so user env overrides always win).
        std::string module_search_path;
        {
#ifdef _WIN32
            constexpr char path_sep = ';';
#else
            constexpr char path_sep = ':';
#endif
            for (const auto& d : resolver_.dirs()) {
                if (!module_search_path.empty()) module_search_path += path_sep;
                module_search_path += d.string();
            }
        }

        // The lambda captures only the module search path string (value, not ref),
        // so it is safe to use from any thread. It creates a fresh Driver + VM
        // for each actor thread.
        eta::nng::ProcessManager::ThreadWorkerFn thread_worker_fn =
            [module_search_path](
                const std::string& th_module_path,
                const std::string& th_func_name,
                const std::string& th_endpoint,
                std::vector<std::string> th_text_args,
                std::shared_ptr<std::atomic<bool>> alive) noexcept
        {
            try {
                auto resolver = ModulePathResolver::from_path_string(module_search_path);
                Driver child(std::move(resolver));
                child.load_prelude();

                // Dial the parent's inproc:// listener → sets current-mailbox
                if (!child.install_mailbox(th_endpoint)) {
                    alive->store(false, std::memory_order_release);
                    return;
                }

                // Load the target module
                if (!child.run_file(fs::path(th_module_path))) {
                    alive->store(false, std::memory_order_release);
                    return;
                }

                // Build and evaluate: (func-name arg1 arg2 ...)
                std::string call_src = "(" + th_func_name;
                for (const auto& a : th_text_args) {
                    call_src += " ";
                    call_src += a;
                }
                call_src += ")";
                child.run_source(call_src);
            } catch (...) {}
            alive->store(false, std::memory_order_release);
        };

        // Receives a SerializedClosure, deserializes the bytecode into the child
        // Driver's registry, reconstructs upvalues, creates the Closure heap
        // object, and calls it via call_value().
        eta::nng::ProcessManager::ClosureWorkerFn closure_worker_fn =
            [module_search_path](
                const std::string& th_endpoint,
                eta::nng::ProcessManager::SerializedClosure sc,
                std::shared_ptr<std::atomic<bool>> alive) noexcept
        {
            try {
                auto resolver = ModulePathResolver::from_path_string(module_search_path);
                Driver child(std::move(resolver));
                child.load_prelude();

                // Dial the parent's inproc:// listener → sets current-mailbox
                if (!child.install_mailbox(th_endpoint)) {
                    alive->store(false, std::memory_order_release);
                    return;
                }

                // Deserialize the function registry from the etac-format blob
                runtime::vm::BytecodeSerializer ser(child.heap(), child.intern_table());
                std::istringstream iss(std::string(sc.funcs_bytes.begin(),
                                                   sc.funcs_bytes.end()),
                                      std::ios::binary);
                auto etac_res = ser.deserialize(iss, /*expected_builtins=*/0);
                if (!etac_res) {
                    alive->store(false, std::memory_order_release);
                    return;
                }
                auto& etac = *etac_res;

                // Rebase and register the functions in the child's registry
                uint32_t base_idx = static_cast<uint32_t>(child.registry().size());
                for (const auto& func : etac.registry.all()) {
                    runtime::vm::BytecodeFunction copy = func;
                    copy.rebase_func_indices(static_cast<int32_t>(base_idx));
                    child.registry().add(std::move(copy));
                }

                // Deserialize upvalues
                std::vector<runtime::nanbox::LispVal> upval_vals;
                upval_vals.reserve(sc.upvals.size());
                for (const auto& uv_bytes : sc.upvals) {
                    auto uv = eta::nng::deserialize_binary(
                        std::span<const uint8_t>(uv_bytes),
                        child.heap(), child.intern_table());
                    if (!uv) {
                        alive->store(false, std::memory_order_release);
                        return;
                    }
                    upval_vals.push_back(*uv);
                }

                // Reconstruct the Closure heap object in the child's heap
                const auto* entry_func = child.registry().get(base_idx);
                if (!entry_func) {
                    alive->store(false, std::memory_order_release);
                    return;
                }
                auto closure_val = runtime::memory::factory::make_closure(
                    child.heap(), entry_func, std::move(upval_vals));
                if (!closure_val) {
                    alive->store(false, std::memory_order_release);
                    return;
                }

                // Call the thunk with 0 arguments
                (void) child.vm().call_value(*closure_val, {});
            } catch (...) {}
            alive->store(false, std::memory_order_release);
        };

        // nng networking + actor-model primitives
        eta::nng::register_nng_primitives(
            builtins_, heap_, intern_table_,
            &proc_mgr_, etai_path_, &mailbox_val_,
            module_search_path, std::move(thread_worker_fn),
            &registry_);

        proc_mgr_.set_closure_factory(std::move(closure_worker_fn));
#endif

        // Wire up function resolver
        vm_.set_function_resolver([this](uint32_t idx) {
            return registry_.get(idx);
        });
    }

    // Non-copyable, non-movable (owns references captured in lambdas)
    Driver(const Driver&) = delete;
    Driver& operator=(const Driver&) = delete;
    Driver(Driver&&) = delete;
    Driver& operator=(Driver&&) = delete;

    /// Result of a load_prelude() call.
    struct PreludeResult {
        bool found{false};    ///< Was prelude.eta found on disk?
        bool loaded{false};   ///< Did it compile and execute successfully?
        fs::path path;        ///< Path to the prelude file (if found).
    };

    /// Result of a compile_file() call — metadata for all newly emitted modules.
    struct CompileModuleEntry {
        std::string name;
        uint32_t init_func_index{0};        ///< index relative to base_func_idx
        uint32_t total_globals{0};
        std::optional<uint32_t> main_func_slot;
    };

    struct CompileResult {
        std::vector<CompileModuleEntry> modules;
        std::vector<std::string> imports;    ///< Non-prelude module dependencies
        uint32_t base_func_idx{0};   ///< first function index in the registry for this compilation
        uint32_t end_func_idx{0};    ///< one past the last function index
    };

    /**
     * @brief Load and execute the prelude from the module path.
     *
     * Searches for "prelude.eta" in the configured search directories.
     * If found, it is compiled and executed, seeding the global environment
     * with standard library definitions.
     */
    PreludeResult load_prelude() {
        PreludeResult result;
        auto prelude_path = resolver_.find_file("prelude.eta");
        if (!prelude_path) {
            return result; // not found
        }
        result.found = true;
        result.path = *prelude_path;
        result.loaded = run_file(*prelude_path);
        return result;
    }

    /// Check whether a module with the given name has been executed.
    [[nodiscard]] bool has_module(const std::string& name) const {
        return executed_modules_.contains(name);
    }

    /**
     * @brief Read, compile and execute a .eta file.
     * @return true on success, false on error (diagnostics emitted to engine).
     */
    bool run_file(const fs::path& path) {
        std::ifstream in(path, std::ios::in | std::ios::binary);
        if (!in) {
            diag_engine_.emit_error(
                diagnostic::DiagnosticCode::ModuleNotFound, {},
                "cannot open file: " + path.string());
            return false;
        }
        std::ostringstream buf;
        buf << in.rdbuf();

        auto file_id = allocate_file_id(path.string());
        return run_source_impl(buf.str(), file_id);
    }

    /**
     * @brief Compile a .eta file without executing it.
     *
     * Runs the full pipeline (lex → parse → expand → link → analyze → emit)
     * but skips VM execution.  The prelude and imported dependencies are still
     * executed normally (they must populate globals for semantic analysis).
     *
     * @return CompileResult on success, std::nullopt on error.
     */
    std::optional<CompileResult> compile_file(const fs::path& path) {
        std::ifstream in(path, std::ios::in | std::ios::binary);
        if (!in) {
            diag_engine_.emit_error(
                diagnostic::DiagnosticCode::ModuleNotFound, {},
                "cannot open file: " + path.string());
            return std::nullopt;
        }
        std::ostringstream buf;
        buf << in.rdbuf();

        auto file_id = allocate_file_id(path.string());
        CompileResult cr;
        if (!run_source_impl(buf.str(), file_id, /*result=*/nullptr,
                             /*result_binding=*/{}, /*execute=*/false, &cr)) {
            return std::nullopt;
        }
        return cr;
    }

    /**
     * @brief Load and compile the prelude without executing user code.
     *
     * Note: the prelude itself IS executed (its globals must be live for
     * downstream modules).  This is simply a convenience alias for
     * load_prelude().
     */
    PreludeResult compile_prelude() {
        return load_prelude();
    }

    /**
     * @brief Compile and execute a source string (e.g. from the REPL).
     *
     * Incremental: shares VM globals, linker state, and registry with
     * all previous invocations so definitions persist.
     *
     * @param source  The eta source text (one or more top-level forms).
     * @param result  If non-null, receives the last expression value.
     * @param result_binding  If non-empty, look up this binding name in
     *                        the last module's globals to retrieve the result
     *                        (module init functions return Nil by design).
     * @return true on success, false on error.
     */
    bool run_source(std::string_view source,
                    runtime::nanbox::LispVal* result = nullptr,
                    const std::string& result_binding = {}) {
        return run_source_impl(std::string(source), /*file_id=*/0, result, result_binding);
    }

    /// Access the diagnostic engine (for printing / LSP forwarding).
    [[nodiscard]] diagnostic::DiagnosticEngine& diagnostics() noexcept { return diag_engine_; }
    [[nodiscard]] const diagnostic::DiagnosticEngine& diagnostics() const noexcept { return diag_engine_; }

    /// Create a FileResolver that maps file_id → filename string.
    /// Suitable for passing to format_diagnostic / DiagnosticEngine::print_all.
    [[nodiscard]] diagnostic::FileResolver file_resolver() const {
        return [this](uint32_t id) -> std::string {
            auto it = file_id_to_path_.find(id);
            if (it != file_id_to_path_.end())
                return it->second.filename().string();
            return {};
        };
    }

    /// Access the module path resolver.
    [[nodiscard]] ModulePathResolver& resolver() noexcept { return resolver_; }

    /// Direct access to the VM — required for DAP debug hooks.
    runtime::vm::VM& vm() noexcept { return vm_; }
    const runtime::vm::VM& vm() const noexcept { return vm_; }

    /// Direct access to the bytecode registry — required for breakpoint line resolution.
    semantics::BytecodeFunctionRegistry& registry() noexcept { return registry_; }
    const semantics::BytecodeFunctionRegistry& registry() const noexcept { return registry_; }

    /// Reverse-lookup: file_id → absolute path. Returns nullptr if not found.
    [[nodiscard]] const fs::path* path_for_file_id(uint32_t id) const noexcept {
        auto it = file_id_to_path_.find(id);
        return it != file_id_to_path_.end() ? &it->second : nullptr;
    }

    /// Install a custom output port so that display/write/newline go through
    /// the given port rather than falling back to std::cout.
    /// Typical use in the DAP: pass a CallbackPort that fires send_event().
    void set_output_port(std::shared_ptr<runtime::Port> port) {
        auto val = runtime::memory::factory::make_port(heap_, std::move(port));
        if (val) vm_.set_current_output_port(*val);
    }

    /// Install a custom error port (used by eprintln / error output).
    void set_error_port(std::shared_ptr<runtime::Port> port) {
        auto val = runtime::memory::factory::make_port(heap_, std::move(port));
        if (val) vm_.set_current_error_port(*val);
    }

    /// Pre-register a file path so that its file_id is known before the file
    /// is actually loaded.  The DAP uses this to install breakpoints BEFORE
    /// the VM thread starts running.  If the path is already registered the
    /// existing id is returned unchanged.
    uint32_t ensure_file_id(const fs::path& path) {
        auto canon = canon_path_key(path);
        auto it = path_to_file_id_.find(canon);
        if (it != path_to_file_id_.end()) return it->second;
        uint32_t id = next_file_id_++;
        file_id_to_path_[id] = path;
        path_to_file_id_[canon] = id;
        return id;
    }

    /// Forward-lookup: canonical path string → file_id. Returns 0 if not found.
    /// Input is normalised before lookup so case differences on Windows are handled.
    [[nodiscard]] uint32_t file_id_for_path(const std::string& path) const {
        auto canon = canon_path_key(fs::path(path));
        auto it = path_to_file_id_.find(canon);
        return it != path_to_file_id_.end() ? it->second : 0u;
    }

    /// Format a runtime value for display.
    [[nodiscard]] std::string format_value(runtime::nanbox::LispVal v,
                                           runtime::FormatMode mode = runtime::FormatMode::Write) {
        return runtime::format_value(v, mode, heap_, intern_table_);
    }

#ifdef ETA_HAS_NNG
    /// Install the `--mailbox` socket for a spawned child process.
    ///
    /// Called by main_etai.cpp when the `--mailbox <endpoint>` argument is
    /// present.  Creates a PAIR socket, dials the endpoint (connecting to the
    /// parent's listening socket), and stores the socket as `current-mailbox`.
    ///
    /// @return true on success, false if the dial fails.
    bool install_mailbox(const std::string& endpoint) {
        eta::nng::NngSocketPtr sp;
        sp.protocol = eta::nng::NngProtocol::Pair;
        int rv = nng_pair0_open(&sp.socket);
        if (rv != 0) return false;

        nng_socket_set_ms(sp.socket, NNG_OPT_RECVTIMEO, 1000);

        rv = nng_dial(sp.socket, endpoint.c_str(), nullptr, 0);
        if (rv != 0) return false;
        sp.dialed = true;

        auto val = eta::nng::factory::make_nng_socket(heap_, std::move(sp));
        if (!val) return false;
        mailbox_val_ = *val;
        return true;
    }

    /// Access the process manager (for DAP child process tree view).
    [[nodiscard]] eta::nng::ProcessManager* process_manager() noexcept {
        return &proc_mgr_;
    }
    [[nodiscard]] const eta::nng::ProcessManager* process_manager() const noexcept {
        return &proc_mgr_;
    }

    /// Return the current mailbox socket (Nil if not a spawned child).
    [[nodiscard]] runtime::nanbox::LispVal mailbox() const noexcept {
        return mailbox_val_;
    }
#endif

    /// Named global variables: slot index → variable name.
    /// Populated during compilation for debugger display.
    [[nodiscard]] const std::unordered_map<uint32_t, std::string>& global_names() const noexcept {
        return global_names_;
    }

    /// Direct access to the heap — used by the DAP heap inspector.
    runtime::memory::heap::Heap& heap() noexcept { return heap_; }
    const runtime::memory::heap::Heap& heap() const noexcept { return heap_; }

    /// Direct access to the intern table.
    runtime::memory::intern::InternTable& intern_table() noexcept { return intern_table_; }

    /// Access the optimization pipeline — add passes before running files.
    semantics::OptimizationPipeline& optimization_pipeline() noexcept { return optimization_pipeline_; }

    /// Number of registered builtins (used to embed in .etac for mismatch detection).
    [[nodiscard]] std::size_t builtin_count() const noexcept { return builtins_.specs().size(); }

    /**
     * @brief Load and execute a pre-compiled .etac file.
     * @return true on success, false on error (diagnostics emitted to engine).
     */
    bool run_etac_file(const fs::path& path) {
        std::ifstream in(path, std::ios::in | std::ios::binary);
        if (!in) {
            diag_engine_.emit_error(
                diagnostic::DiagnosticCode::ModuleNotFound, {},
                "cannot open file: " + path.string());
            return false;
        }

        runtime::vm::BytecodeSerializer serializer(heap_, intern_table_);
        auto etac_res = serializer.deserialize(in,
            static_cast<uint32_t>(builtins_.specs().size()));
        if (!etac_res) {
            diag_engine_.emit_error(
                diagnostic::DiagnosticCode::ModuleNotFound, {},
                "failed to load .etac: " + std::string(runtime::vm::to_string(etac_res.error())));
            return false;
        }
        auto& etac = *etac_res;

        // Auto-load non-prelude imports
        for (const auto& imp : etac.imports) {
            if (executed_modules_.contains(imp)) continue;
            auto imp_path = resolver_.resolve(imp);
            if (!imp_path) {
                diag_engine_.emit_error(
                    diagnostic::DiagnosticCode::ModuleNotFound, {},
                    "cannot resolve import '" + imp + "' required by .etac");
                return false;
            }
            if (!run_file(*imp_path)) return false;
        }

        // Move functions from the deserialized registry into ours,
        // recording the base index so module init_func_index values can be offset.
        // The .etac stores 0-based (file-relative) function indices; relocate
        // them to the runner's absolute indices.
        uint32_t base_idx = static_cast<uint32_t>(registry_.size());
        for (const auto& func : etac.registry.all()) {
            runtime::vm::BytecodeFunction copy = func;
            copy.rebase_func_indices(static_cast<int32_t>(base_idx));
            registry_.add(std::move(copy));
        }

        // Wire up function resolver if not already done
        // (constructor does this, but defensive)

        // Execute each module's _init function
        for (const auto& mod : etac.modules) {
            if (executed_modules_.contains(mod.name)) continue;

            auto& globals = vm_.globals();
            if (globals.size() < mod.total_globals)
                globals.resize(mod.total_globals, runtime::nanbox::Nil);

            // Re-install builtins
            if (!builtins_installed_) {
                for (std::size_t i = 0; i < builtins_.specs().size(); ++i) {
                    const auto& spec = builtins_.specs()[i];
                    auto prim = runtime::memory::factory::make_primitive(
                        heap_, spec.func, spec.arity, spec.has_rest);
                    if (prim) globals[i] = *prim;
                }
                builtins_installed_ = true;
            }

            uint32_t func_idx = base_idx + mod.init_func_index;
            const auto* init_func = registry_.get(func_idx);
            if (!init_func) {
                diag_engine_.emit_error(
                    diagnostic::DiagnosticCode::ModuleNotFound, {},
                    "missing init function for module: " + mod.name);
                return false;
            }

            auto exec_res = vm_.execute(*init_func);
            if (!exec_res) {
                emit_runtime_error(exec_res.error());
                return false;
            }

            executed_modules_.insert(mod.name);

            // Invoke optional main
            if (mod.main_func_slot) {
                auto main_val = globals[*mod.main_func_slot];
                if (main_val != runtime::nanbox::Nil) {
                    auto main_res = vm_.call_value(main_val, {});
                    if (!main_res) {
                        emit_runtime_error(main_res.error());
                        return false;
                    }
                }
            }
        }

        return true;
    }

private:
    ModulePathResolver resolver_;
    runtime::memory::heap::Heap heap_;
    runtime::memory::intern::InternTable intern_table_;
    semantics::BytecodeFunctionRegistry registry_;
    runtime::BuiltinEnvironment builtins_;
    runtime::vm::VM vm_;

    diagnostic::DiagnosticEngine diag_engine_;

    // IR-level optimization pipeline (runs between analyze and emit)
    semantics::OptimizationPipeline optimization_pipeline_;

#ifdef ETA_HAS_NNG
    eta::nng::ProcessManager         proc_mgr_;
    runtime::nanbox::LispVal         mailbox_val_{runtime::nanbox::Nil};
    std::string                      etai_path_;

    /// Auto-detect the path to the etai binary at startup.
    /// Reads /proc/self/exe on Linux, then looks for "etai" in the same
    /// directory.  Falls back to "etai" (searched on PATH).
    static std::string detect_etai_path() {
        namespace fs = std::filesystem;
        std::error_code ec;
#if defined(__linux__)
        auto self = fs::read_symlink("/proc/self/exe", ec);
        if (!ec) {
            auto candidate = self.parent_path() / "etai";
            if (fs::exists(candidate, ec) && !ec)
                return candidate.string();
        }
#elif defined(__APPLE__)
        // macOS: use _NSGetExecutablePath
        char buf[4096] = {};
        uint32_t size = sizeof(buf);
        if (_NSGetExecutablePath(buf, &size) == 0) {
            auto candidate = fs::path(buf).parent_path() / "etai";
            if (fs::exists(candidate, ec) && !ec)
                return candidate.string();
        }
#elif defined(_WIN32)
        char buf[MAX_PATH] = {};
        if (GetModuleFileNameA(nullptr, buf, MAX_PATH)) {
            auto candidate = fs::path(buf).parent_path() / "etai.exe";
            if (fs::exists(candidate, ec) && !ec)
                return candidate.string();
        }
#endif
        return "etai"; // fallback to PATH
    }
#endif

    // Accumulated expanded forms from all prior run_source calls.
    // The linker clears its state on each index_modules() call, so we must
    // re-feed ALL modules each time for correct cross-module resolution.
    std::vector<reader::parser::SExprPtr> accumulated_forms_;

    // Names of modules that have already been executed (to avoid re-running).
    std::unordered_set<std::string> executed_modules_;

    // Track which files we've already loaded (to avoid double-loading prelude etc.)
    std::unordered_set<std::string> loaded_files_;

    // File ID allocator for diagnostic spans
    uint32_t next_file_id_;

    // Bidirectional file-id ↔ path maps (for diagnostics and DAP span resolution)
    std::unordered_map<uint32_t, fs::path>    file_id_to_path_;
    std::unordered_map<std::string, uint32_t> path_to_file_id_;

    // Whether builtins have been installed into VM globals yet
    bool builtins_installed_{false};

    // Guard against recursive auto-loading cycles
    std::unordered_set<std::string> loading_modules_;

    // Global slot → variable name mapping (for debugger display)
    std::unordered_map<uint32_t, std::string> global_names_;

    /// Normalise a path to a stable lowercase key used in path_to_file_id_.
    /// On Windows this lowercases and ensures backslashes; on POSIX it is
    /// just fs::absolute + lexically_normal.
    static std::string canon_path_key(const fs::path& p) {
        std::error_code ec;
        fs::path abs = fs::absolute(p, ec);
        if (ec) abs = p;
        std::string s = abs.lexically_normal().string();
#ifdef _WIN32
        for (char& c : s) {
            if (c == '/') c = '\\';
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
#endif
        return s;
    }

    uint32_t allocate_file_id(const std::string& raw_path) {
        return ensure_file_id(fs::path(raw_path));
    }

    /// Collect all module names referenced in (import ...) clauses within
    /// a set of expanded forms.  Scans the top-level module lists for
    /// (import <sym>) / (import (only <sym> ...) ...) etc.
    static std::vector<std::string> collect_imported_modules(
            std::span<const reader::parser::SExprPtr> forms) {
        std::vector<std::string> result;
        std::unordered_set<std::string> seen;

        auto extract_module_name = [](const reader::parser::SExprPtr& clause) -> std::string {
            namespace utils = reader::utils;
            // Plain symbol:  std.core
            if (auto s = utils::as_symbol(clause)) return s->name;
            // List form:  (only std.core ...) / (except ...) / (rename ...) / (prefix ...)
            if (auto l = utils::as_list(clause)) {
                if (l->elems.size() >= 2) {
                    if (auto m = utils::as_symbol(l->elems[1])) return m->name;
                }
            }
            return {};
        };

        for (const auto& form : forms) {
            auto* lst = form ? form->template as<reader::parser::List>() : nullptr;
            if (!lst || lst->elems.size() < 2) continue;
            if (!reader::utils::is_symbol_named(lst->elems[0], "module")) continue;
            // Walk body for (import ...) forms
            for (std::size_t i = 2; i < lst->elems.size(); ++i) {
                auto* inner = lst->elems[i] ? lst->elems[i]->template as<reader::parser::List>() : nullptr;
                if (!inner || inner->elems.empty()) continue;
                if (!reader::utils::is_symbol_named(inner->elems[0], "import")) continue;
                for (std::size_t j = 1; j < inner->elems.size(); ++j) {
                    auto name = extract_module_name(inner->elems[j]);
                    if (!name.empty() && seen.insert(name).second) {
                        result.push_back(name);
                    }
                }
            }
        }
        return result;
    }

    /// Auto-load module files from the module path for any import that
    /// references a module not yet in the accumulated set.
    /// When execute is false, only the top-level target skips execution;
    /// auto-loaded dependencies are always executed (they must populate globals).
    /// Returns false on failure; diagnostics are emitted.
    bool auto_load_imports(std::span<const reader::parser::SExprPtr> new_forms) {
        auto needed = collect_imported_modules(new_forms);
        for (const auto& mod_name : needed) {
            // Skip modules already loaded or being loaded (cycle guard)
            if (executed_modules_.contains(mod_name)) continue;
            if (loading_modules_.contains(mod_name)) continue;

            // Check if it's already in the accumulated forms (defined but not yet executed)
            bool already_accumulated = false;
            for (const auto& f : accumulated_forms_) {
                auto* lst = f ? f->template as<reader::parser::List>() : nullptr;
                if (!lst || lst->elems.size() < 2) continue;
                if (!reader::utils::is_symbol_named(lst->elems[0], "module")) continue;
                auto* nsym = lst->elems[1]->template as<reader::parser::Symbol>();
                if (nsym && nsym->name == mod_name) { already_accumulated = true; break; }
            }
            // Also check in the new forms themselves (peer modules in same source)
            if (!already_accumulated) {
                for (const auto& f : new_forms) {
                    auto* lst = f ? f->template as<reader::parser::List>() : nullptr;
                    if (!lst || lst->elems.size() < 2) continue;
                    if (!reader::utils::is_symbol_named(lst->elems[0], "module")) continue;
                    auto* nsym = lst->elems[1]->template as<reader::parser::Symbol>();
                    if (nsym && nsym->name == mod_name) { already_accumulated = true; break; }
                }
            }
            if (already_accumulated) continue;

            // Try to resolve and load the module file
            auto path = resolver_.resolve(mod_name);
            if (!path) continue; // Will fail later at link time with a clear error

            auto canonical = path->string();
            if (loaded_files_.contains(canonical)) continue;

            loading_modules_.insert(mod_name);
            loaded_files_.insert(canonical);
            bool ok = run_file(*path);
            loading_modules_.erase(mod_name);

            if (!ok) return false;
        }
        return true;
    }

    /**
     * @brief Core implementation: compile (+ optionally execute) source text.
     *
     * Accumulates expanded forms and re-links everything each time (the
     * ModuleLinker is non-incremental). Only newly-added modules are
     * emitted and executed — previously-run modules are skipped.
     *
     * @param execute  When false, skip VM execution and main invocation.
     * @param out_cr   If non-null, filled with per-module compile metadata.
     */
    bool run_source_impl(const std::string& source, uint32_t file_id,
                         runtime::nanbox::LispVal* result = nullptr,
                         const std::string& result_binding = {},
                         bool execute = true,
                         CompileResult* out_cr = nullptr) {
        diag_engine_.clear();

        // Lex + Parse
        reader::lexer::Lexer lex(file_id, source);
        reader::parser::Parser parser(lex);

        auto parsed_res = parser.parse_toplevel();
        if (!parsed_res) {
            auto& err = parsed_res.error();
            std::visit([this](auto&& e) {
                diag_engine_.emit(diagnostic::to_diagnostic(e));
            }, err);
            return false;
        }
        auto parsed = std::move(*parsed_res);
        if (parsed.empty()) {
            // Empty input — not an error
            return true;
        }

        // Expand
        reader::expander::Expander expander;
        auto expanded_res = expander.expand_many(parsed);
        if (!expanded_res) {
            diag_engine_.emit(diagnostic::to_diagnostic(expanded_res.error()));
            return false;
        }
        auto new_expanded = std::move(*expanded_res);

        // Remember which modules are new (not yet executed)
        std::vector<std::string> new_module_names;
        for (const auto& form : new_expanded) {
            if (auto* mf = form->template as<reader::parser::ModuleForm>()) {
                new_module_names.push_back(mf->name);
            } else if (auto* lst = form->template as<reader::parser::List>()) {
                if (!lst->elems.empty()) {
                    if (auto* sym = lst->elems[0]->template as<reader::parser::Symbol>()) {
                        if (sym->name == "module" && lst->elems.size() >= 2) {
                            if (auto* nsym = lst->elems[1]->template as<reader::parser::Symbol>()) {
                                new_module_names.push_back(nsym->name);
                            }
                        }
                    }
                }
            }
        }

        // Auto-load imported modules from the module path
        std::span<const reader::parser::SExprPtr> new_span(
            new_expanded.data(), new_expanded.size());
        if (!auto_load_imports(new_span)) {
            return false;
        }

        // Record non-prelude imports in the compile result so the
        // serializer can embed them in .etac files.
        if (out_cr) {
            auto needed = collect_imported_modules(new_span);
            for (const auto& mod_name : needed) {
                bool defined_locally = false;
                for (const auto& nm : new_module_names) {
                    if (nm == mod_name) { defined_locally = true; break; }
                }
                if (!defined_locally) {
                    out_cr->imports.push_back(mod_name);
                }
            }
        }

        // Append new forms to the accumulated set
        for (auto& f : new_expanded) {
            accumulated_forms_.push_back(reader::parser::deep_copy(f));
        }

        // Link ALL accumulated forms
        reader::ModuleLinker linker;
        auto idx_res = linker.index_modules(accumulated_forms_);
        if (!idx_res) {
            // Rollback: remove the forms we just added
            for (std::size_t i = 0; i < new_expanded.size(); ++i) {
                accumulated_forms_.pop_back();
            }
            emit_link_error(idx_res.error());
            return false;
        }
        auto link_res = linker.link();
        if (!link_res) {
            for (std::size_t i = 0; i < new_expanded.size(); ++i) {
                accumulated_forms_.pop_back();
            }
            emit_link_error(link_res.error());
            return false;
        }

        // Semantic analysis (all accumulated modules)
        semantics::SemanticAnalyzer sa;
        auto sem_res = sa.analyze_all(accumulated_forms_, linker, builtins_);
        if (!sem_res) {
            for (std::size_t i = 0; i < new_expanded.size(); ++i) {
                accumulated_forms_.pop_back();
            }
            diag_engine_.emit(diagnostic::to_diagnostic(sem_res.error()));
            return false;
        }
        auto sem_mods = std::move(*sem_res);
        if (sem_mods.empty()) return true;

        // Run IR optimization passes
        optimization_pipeline_.run_all(sem_mods);

        // Emit + Execute only NEW modules
        // Grow globals vector if needed, preserving existing values.
        // Re-install builtins in slots 0..N-1 (heap objects may have been GC'd).
        auto& globals = vm_.globals();
        auto needed = sem_mods[0].total_globals;
        if (globals.size() < needed) {
            globals.resize(needed, runtime::nanbox::Nil);
        }

        if (execute) {
            // Re-install builtin primitives at their fixed slots
            for (std::size_t i = 0; i < builtins_.specs().size(); ++i) {
                const auto& spec = builtins_.specs()[i];
                auto prim = runtime::memory::factory::make_primitive(
                    heap_, spec.func, spec.arity, spec.has_rest);
                if (!prim) {
                    emit_runtime_error(prim.error());
                    return false;
                }
                globals[i] = *prim;
            }
            builtins_installed_ = true;
        }

        // Track the registry range for newly emitted functions.
        uint32_t base_func_idx = static_cast<uint32_t>(registry_.size());

        for (auto& mod : sem_mods) {
            if (executed_modules_.contains(mod.name)) {
                continue; // Already executed in a prior call
            }

            semantics::Emitter emitter(mod, heap_, intern_table_, registry_);
            auto* init_func = emitter.emit();

            // Record global slot → name mapping for debugger display.
            // Prefix with "module." so the UI can group by module.
            for (const auto& bi : mod.bindings) {
                if (bi.kind == semantics::BindingInfo::Kind::Global && !bi.name.empty()) {
                    global_names_[bi.slot] = mod.name + "." + bi.name;
                }
            }

            // Record compile metadata for this module.
            if (out_cr) {
                CompileModuleEntry cme;
                cme.name = mod.name;
                // init_func is the last function added by emitter.emit()
                cme.init_func_index = static_cast<uint32_t>(registry_.size()) - 1 - base_func_idx;
                cme.total_globals = mod.total_globals;
                cme.main_func_slot = mod.main_func_slot;
                out_cr->modules.push_back(std::move(cme));
            }

            if (execute) {
                auto exec_res = vm_.execute(*init_func);
                if (!exec_res) {
                    emit_runtime_error(exec_res.error());
                    return false;
                }

                executed_modules_.insert(mod.name);

                // Invoke optional (defun main ...) entry point
                if (mod.main_func_slot) {
                    auto main_val = globals[*mod.main_func_slot];
                    if (main_val != runtime::nanbox::Nil) {
                        auto main_res = vm_.call_value(main_val, {});
                        if (!main_res) {
                            emit_runtime_error(main_res.error());
                            return false;
                        }
                    }
                }

                // For REPL: capture result from the last NEW module
                if (result && !result_binding.empty()) {
                    // Check if this is the last new module
                    bool is_last_new = (!new_module_names.empty() &&
                                        mod.name == new_module_names.back());
                    if (is_last_new) {
                        for (const auto& bi : mod.bindings) {
                            if (bi.name == result_binding) {
                                *result = vm_.globals()[bi.slot];
                                break;
                            }
                        }
                    }
                }
            }
        }

        uint32_t end_func_idx = static_cast<uint32_t>(registry_.size());
        if (out_cr) {
            out_cr->base_func_idx = base_func_idx;
            out_cr->end_func_idx = end_func_idx;
        }

        return true;
    }

    /// Convert a LinkError into a Diagnostic and emit it.
    void emit_link_error(const reader::LinkError& e) {
        diag_engine_.emit(diagnostic::to_diagnostic(e));
    }

    /// Convert a RuntimeError variant into a Diagnostic and emit it.
    void emit_runtime_error(const runtime::error::RuntimeError& err) {
        std::visit([this](auto&& e) {
            diag_engine_.emit(diagnostic::to_diagnostic(e));
        }, err);
    }
};

} // namespace eta::interpreter

