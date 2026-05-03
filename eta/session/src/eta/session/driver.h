#pragma once

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <expected>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <memory>
#include <optional>
#include <set>
#include <sstream>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
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
#include "eta/runtime/builtin_names.h"
#include "eta/runtime/embedded_prelude.h"
#include "eta/runtime/port.h"
#include "eta/runtime/value_formatter.h"
#include "eta/diagnostic/diagnostic.h"

/// Single source of truth for live primitive registration order:
#include "eta/interpreter/all_primitives.h"
#include "eta/session/eval_display.h"
#include "eta/interpreter/repl_wrap.h"
#include "eta/interpreter/repl_complete.h"

#include <nng/nng.h>
#include <nng/protocol/pair0/pair.h>
#include <eta/nng/nng_socket_ptr.h>
#include <eta/nng/nng_factory.h>
#include <eta/nng/nng_primitives.h>
#include <eta/nng/process_mgr.h>
#include <eta/log/log_state.h>

#include "eta/interpreter/module_path.h"

namespace eta::session {

namespace fs = std::filesystem;
using eta::interpreter::ModulePathResolver;
using eta::interpreter::PriorModule;
using eta::interpreter::wrap_repl_submission;
using eta::interpreter::register_all_primitives;

/**
 * @brief Compilation + execution driver for the eta language.
 *
 * Owns the full runtime state (Heap, InternTable, VM, etc.) and provides
 * VM globals and linker state, so definitions persist across REPL inputs.
 *
 */
class Driver {
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
     * @param env_var     Name of the environment variable to read.
     * @param default_val Returned when the variable is absent, empty, or invalid.
     */
    static std::size_t parse_heap_env_var(
        const char*  env_var,
        std::size_t  default_val = DEFAULT_HEAP_SOFT_LIMIT_BYTES) noexcept
    {
        const char* s = std::getenv(env_var);
        if (!s || s[0] == '\0') return default_val;

        char* end = nullptr;
        errno = 0;
        const unsigned long long raw = std::strtoull(s, &end, 10);
        if (end == s || errno == ERANGE || raw == 0) return default_val;

        std::uint64_t mult = 1;
        if (end && *end != '\0') {
            switch (*end) {
                case 'K': case 'k': mult = 1024ULL;                  ++end; break;
                case 'M': case 'm': mult = 1024ULL * 1024ULL;        ++end; break;
                case 'G': case 'g': mult = 1024ULL * 1024ULL * 1024ULL; ++end; break;
                default: break;
            }
            /// Skip optional trailing whitespace, then require end-of-string.
            while (*end && std::isspace(static_cast<unsigned char>(*end))) ++end;
            if (*end != '\0') return default_val; ///< unexpected trailing characters
        }

        const std::uint64_t result = static_cast<std::uint64_t>(raw) * mult;
        /// Guard against overflow vs. std::size_t.
        constexpr std::uint64_t SIZE_T_MAX = static_cast<std::uint64_t>(~std::size_t{0});
        if (result > SIZE_T_MAX) return default_val;
        return static_cast<std::size_t>(result);
    }

    explicit Driver(ModulePathResolver resolver,
                    std::size_t heap_bytes = DEFAULT_HEAP_SOFT_LIMIT_BYTES,
                    std::string etai_path  = {},
                    std::vector<std::string> command_line_arguments = {})
        : resolver_(std::move(resolver)),
          heap_(heap_bytes),
          intern_table_(),
          registry_(),
          builtins_(),
          vm_(heap_, intern_table_),
          diag_engine_(),
          command_line_arguments_(std::move(command_line_arguments)),
          next_file_id_(1) ///< 0 is reserved for REPL / anonymous input
    {
        /**
         * The VM installs a default heap GC callback in its constructor, but at
         * the Driver level we also need compiled bytecode constants in the
         * function registry to act as GC roots. Large modules (e.g. portfolio)
         * can emit quoted heap-backed constants long before they are executed or
         * serialized for spawn-thread.
         */
        heap_.set_gc_callback([this]() { collect_garbage_with_registry_roots(); });

        /**
         * Register all core + port + io + os + time + torch + stats + log primitives.
         * NNG follows with driver-specific arguments.
         * Step 1: Populate all slots with metadata (name/arity/has_rest) + null funcs.
         *         builtin_names.h is the Single Source of Truth for slot order.
         */
        runtime::register_builtin_names(builtins_);
        ///         validate metadata and install the real func.
        builtins_.begin_patching();
        register_all_primitives(
            builtins_, heap_, intern_table_, vm_, command_line_arguments_);


        /// Detect etai binary path if not explicitly supplied
        if (etai_path.empty()) etai_path = detect_etai_path();
        etai_path_ = etai_path;

        /**
         * Build colon-separated module search path to forward to child processes.
         * Child receives this via ETA_MODULE_PATH only if ETA_MODULE_PATH is not
         * already set in the environment (so user env overrides always win).
         */
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
        module_search_path_ = std::move(module_search_path);

        /// nng networking + actor-model primitives
        eta::nng::register_nng_primitives(
            builtins_, heap_, intern_table_,
            &proc_mgr_, etai_path_, &mailbox_val_,
            module_search_path_, {},
            &registry_, &vm_.globals());

        /// Step 3: Verify every pre-registered slot now has a real implementation.
        builtins_.verify_all_patched();

        builtins_.overwrite_func("eval",
            [this](std::span<const runtime::nanbox::LispVal> args)
                -> std::expected<runtime::nanbox::LispVal, runtime::error::RuntimeError>
            {
                using runtime::error::RuntimeError;
                using runtime::error::RuntimeErrorCode;
                using runtime::error::VMError;
                using runtime::memory::heap::ObjectKind;
                using runtime::nanbox::LispVal;
                using runtime::nanbox::Tag;

                if (args.size() != 1) {
                    return std::unexpected(RuntimeError{VMError{
                        RuntimeErrorCode::InvalidArity,
                        "eval: expected 1 argument"}});
                }

                const LispVal expr = args[0];
                bool should_eval = false;
                if (runtime::nanbox::ops::is_boxed(expr)) {
                    const auto tag = runtime::nanbox::ops::tag(expr);
                    if (tag == Tag::Symbol) {
                        should_eval = true;
                    } else if (tag == Tag::HeapObject) {
                        const auto id = runtime::nanbox::ops::payload(expr);
                        should_eval = (heap_.try_get_as<ObjectKind::Cons, void>(id) != nullptr);
                    }
                }
                if (!should_eval) return expr;

                auto lexical_bindings = collect_eval_lexical_bindings();
                const std::string expr_source = format_value(expr, runtime::FormatMode::Write);

                auto closure_res = [&]() -> std::expected<LispVal, RuntimeError> {
                    runtime::vm::VM::ExecutionScope scope(vm_);
                    return compile_eval_lambda(expr_source, lexical_bindings);
                }();
                if (!closure_res) {
                    return std::unexpected(closure_res.error());
                }
                return invoke_eval_lambda(*closure_res, lexical_bindings);
            });

        install_actor_worker_factories();


        /// Wire up function resolver
        vm_.set_function_resolver([this](uint32_t idx) {
            return registry_.get(idx);
        });
    }

    /// Non-copyable, non-movable (owns references captured in lambdas)
    Driver(const Driver&) = delete;
    Driver& operator=(const Driver&) = delete;
    Driver(Driver&&) = delete;
    Driver& operator=(Driver&&) = delete;

    ~Driver() {
        eta::log::global_log_state().erase_vm(vm_);
    }

    /// Result of a load_prelude() call.
    struct PreludeResult {
        bool found{false};    ///< Was a prelude artifact discovered?
        bool loaded{false};   ///< Did it compile and execute successfully?
        fs::path path;        ///< Source/.etac path, or an embedded marker.
    };

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
     * @brief Captured lexical binding used when evaluating an expression.
     */
    struct EvalBinding {
        std::string name;
        runtime::nanbox::LispVal value{runtime::nanbox::Nil};
    };

    /**
     * @brief Load and execute the prelude.
     *
     * Resolution order:
     *  1) embedded bytecode blob
     *  2) bundled/resolved "prelude.etac"
     *  3) source "prelude.eta"
     */
    PreludeResult load_prelude() {
        PreludeResult result;

        if (has_module("std.prelude")) {
            result.found = true;
            if (prelude_origin_path_) {
                result.path = *prelude_origin_path_;
            }
            return result;
        }

        if (try_load_embedded_prelude()) {
            result.found = true;
            result.loaded = true;
            result.path = embedded_prelude_marker_path();
            prelude_origin_path_ = result.path;
            return result;
        }

        if (auto prelude_etac = resolver_.find_file("prelude.etac")) {
            result.found = true;
            result.path = *prelude_etac;
            if (run_etac_file(*prelude_etac)) {
                result.loaded = true;
                prelude_origin_path_ = result.path;
                return result;
            }
        }

        auto prelude_path = resolver_.find_file("prelude.eta");
        if (!prelude_path) {
            return result;
        }

        result.found = true;
        result.path = *prelude_path;
        result.loaded = run_file(*prelude_path);
        if (result.loaded) {
            prelude_origin_path_ = result.path;
        }
        return result;
    }

    /// Check whether a module with the given name has been executed.
    [[nodiscard]] bool has_module(const std::string& name) const {
        return executed_modules_.contains(name);
    }

    /**
     * @brief Remove cached linker/execution state for one module.
     *
     * This clears the module from the executed-module set, removes matching
     * top-level module forms from the accumulated linker input, and drops any
     * recorded global binding names for that module.
     *
     * @return true when any cached entry was removed.
     */
    bool clear_module_cache(const std::string& module_name) {
        bool changed = false;

        if (executed_modules_.erase(module_name) > 0) {
            changed = true;
        }

        const auto before_forms = accumulated_forms_.size();
        accumulated_forms_.erase(
            std::remove_if(
                accumulated_forms_.begin(),
                accumulated_forms_.end(),
                [&module_name](const reader::parser::SExprPtr& form) {
                    auto* module = form->template as<reader::parser::ModuleForm>();
                    if (module) return module->name == module_name;

                    auto* lst = form->template as<reader::parser::List>();
                    if (!lst || lst->elems.size() < 2) return false;
                    if (!reader::utils::is_symbol_named(lst->elems[0], "module")) return false;
                    auto* name = lst->elems[1]->template as<reader::parser::Symbol>();
                    return name && name->name == module_name;
                }),
            accumulated_forms_.end());
        if (accumulated_forms_.size() != before_forms) {
            changed = true;
        }

        const std::string prefix = module_name + ".";
        for (auto it = global_names_.begin(); it != global_names_.end();) {
            if (it->second == module_name || it->second.starts_with(prefix)) {
                it = global_names_.erase(it);
                changed = true;
            } else {
                ++it;
            }
        }

        repl_modules_.erase(
            std::remove_if(
                repl_modules_.begin(),
                repl_modules_.end(),
                [&module_name](const PriorModule& prior) {
                    return prior.name == module_name;
                }),
            repl_modules_.end());

        return changed;
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

    /**
     * @brief Compile an eval expression into a closure with lexical parameters.
     *
     * The generated module/function names are unique per Driver instance.
     */
    std::expected<runtime::nanbox::LispVal, runtime::error::RuntimeError>
    compile_eval_lambda(std::string_view expr_source,
                        std::span<const EvalBinding> lexical_bindings) {
        const uint64_t eval_id = eval_counter_++;
        const std::string module_name = "__eta.eval." + std::to_string(eval_id);
        const std::string fn_name = "__eta_eval_fn_" + std::to_string(eval_id);

        struct TemporaryExecutedModuleGuard {
            std::unordered_set<std::string>& executed_modules;
            std::vector<std::string> inserted_modules;

            ~TemporaryExecutedModuleGuard() {
                for (const auto& name : inserted_modules) {
                    executed_modules.erase(name);
                }
            }
        };

        TemporaryExecutedModuleGuard executed_guard{executed_modules_};
        executed_guard.inserted_modules.reserve(active_module_init_stack_.size());
        for (const auto& active_module_name : active_module_init_stack_) {
            if (executed_modules_.insert(active_module_name).second) {
                executed_guard.inserted_modules.push_back(active_module_name);
            }
        }

        std::ostringstream source;
        source << "(module " << module_name << "\n"
               << "  (define " << fn_name << "\n"
               << "    (lambda (";
        for (std::size_t i = 0; i < lexical_bindings.size(); ++i) {
            if (i > 0) source << ' ';
            source << lexical_bindings[i].name;
        }
        source << ")\n"
               << "      " << expr_source << ")))";

        runtime::nanbox::LispVal closure = runtime::nanbox::Nil;
        if (!run_source_impl(source.str(), /*file_id=*/0, &closure, fn_name)) {
            return std::unexpected(runtime::error::RuntimeError{runtime::error::VMError{
                runtime::error::RuntimeErrorCode::UserError,
                diagnostics_to_string()}});
        }
        return closure;
    }

    /**
     * @brief Invoke a previously compiled eval closure with captured lexical values.
     */
    std::expected<runtime::nanbox::LispVal, runtime::error::RuntimeError>
    invoke_eval_lambda(runtime::nanbox::LispVal closure,
                       std::span<const EvalBinding> lexical_bindings) {
        std::vector<runtime::nanbox::LispVal> args;
        args.reserve(lexical_bindings.size());
        for (const auto& binding : lexical_bindings) {
            args.push_back(binding.value);
        }
        return vm_.call_value(closure, std::move(args));
    }

    using StreamSink = std::function<void(std::string_view)>;

    struct ActorEvent {
        enum class Kind {
            Started,
            Exited,
        };
        Kind kind{Kind::Started};
        int index{-1};
        std::string name;
    };

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
    bool eval_string(std::string source, std::string& out) {
        out.clear();
        auto forms = split_toplevel_forms(source);
        if (forms.empty()) return true;

        auto wrapped = wrap_repl_submission(
            forms, repl_counter_++, has_module("std.prelude"), repl_modules_);

        runtime::nanbox::LispVal result{runtime::nanbox::Nil};
        const bool ok = wrapped.last_is_expr
            ? run_source(wrapped.source, &result, wrapped.result_name)
            : run_source(wrapped.source);
        if (!ok) return false;

        repl_modules_.push_back(PriorModule{
            wrapped.module_name,
            wrapped.user_defines,
            wrapped.user_imports});
        if (wrapped.last_is_expr && result != runtime::nanbox::Nil) {
            out = format_value(result, runtime::FormatMode::Write);
        }
        return true;
    }

    /**
     * @brief Completion payload for front-ends.
     */
    struct CompletionResult {
        std::vector<std::string> matches;
        std::size_t cursor_start{0};
        std::size_t cursor_end{0};
    };

    /**
     * @brief Collect completion matches at @p cursor_pos in @p source.
     *
     * Matches are sourced from keywords, builtin primitives, currently loaded
     * module/global bindings, and module names discoverable on the module path.
     */
    [[nodiscard]] CompletionResult completions_at(const std::string& source,
                                                  std::size_t cursor_pos) const {
        if (cursor_pos > source.size()) cursor_pos = source.size();

        const auto tok = interpreter::repl_complete::token_at(source, cursor_pos);
        CompletionResult out{
            .matches = {},
            .cursor_start = tok.start,
            .cursor_end = tok.end,
        };

        std::string prefix;
        if (!tok.text.empty() && tok.start <= cursor_pos && cursor_pos <= tok.end) {
            prefix = source.substr(tok.start, cursor_pos - tok.start);
        } else {
            out.cursor_start = cursor_pos;
            out.cursor_end = cursor_pos;
        }

        std::unordered_set<std::string> candidates;
        auto add_candidate = [&candidates](std::string name) {
            if (!name.empty()) candidates.insert(std::move(name));
        };

        for (const char* kw : {
                 "define", "lambda", "if", "begin", "set!", "quote",
                 "let", "let*", "letrec", "letrec*", "cond", "case",
                 "and", "or", "when", "unless", "do",
                 "module", "import", "export",
                 "define-syntax", "syntax-rules", "define-record-type",
                 "def", "defun", "progn", "quasiquote",
                 "call/cc", "dynamic-wind", "values", "call-with-values",
                 "apply", "raise", "catch",
                 "logic-var", "unify", "deref-lvar", "trail-mark", "unwind-trail", "copy-term",
             }) {
            add_candidate(kw);
        }

        for (const auto& spec : builtins_.specs()) {
            add_candidate(spec.name);
        }

        for (const auto& [_, qualified] : global_names_) {
            add_candidate(qualified);
            const auto dot = qualified.find_last_of('.');
            if (dot != std::string::npos && dot + 1 < qualified.size()) {
                add_candidate(qualified.substr(dot + 1));
            }
        }

        for (auto& mod : discover_module_names()) {
            add_candidate(std::move(mod));
        }

        out.matches.reserve(candidates.size());
        for (const auto& name : candidates) {
            if (prefix.empty() || name.starts_with(prefix)) {
                out.matches.push_back(name);
            }
        }
        std::sort(out.matches.begin(), out.matches.end());
        return out;
    }

    /**
     * @brief Return Markdown hover text for a symbol.
     *
     * Returns an empty string when no hover content is available.
     */
    [[nodiscard]] std::string hover_at(const std::string& symbol) const {
        if (symbol.empty()) return {};

        static const std::unordered_map<std::string, std::string> keyword_docs = {
            {"define", "**define**  -  Define a variable or function.\n\n`(define name expr)`"},
            {"lambda", "**lambda**  -  Create an anonymous function.\n\n`(lambda (args...) body...)`"},
            {"if", "**if**  -  Conditional expression.\n\n`(if test consequent alternate)`"},
            {"begin", "**begin**  -  Sequence expressions.\n\n`(begin expr...)`"},
            {"module", "**module**  -  Declare a module.\n\n`(module name body...)`"},
            {"import", "**import**  -  Import bindings from a module.\n\n`(import module-name)`"},
            {"export", "**export**  -  Export bindings.\n\n`(export name...)`"},
            {"defun", "**defun**  -  Alias for function definition.\n\n`(defun name (args...) body...)`"},
            {"eval", "**eval**  -  Evaluate an expression in the current lexical and global environment.\n\n`(eval expr)`"},
            {"raise", "**raise**  -  Raise an exception.\n\n`(raise tag value)`"},
            {"catch", "**catch**  -  Catch an exception by tag.\n\n`(catch 'tag body ...)`"},
            {"logic-var", "**logic-var**  -  Create a fresh logic variable.\n\n`(logic-var)`"},
            {"unify", "**unify**  -  Unify two terms.\n\n`(unify term1 term2)`"},
        };

        if (auto it = keyword_docs.find(symbol); it != keyword_docs.end()) {
            return it->second;
        }

        for (const auto& spec : builtins_.specs()) {
            if (symbol == spec.name) {
                std::string doc = "**" + symbol + "**  -  builtin primitive.";
                doc += "\n\n`arity: " + std::to_string(spec.arity);
                if (spec.has_rest) doc += "+";
                doc += "`";
                return doc;
            }
        }

        for (const auto& [_, qualified] : global_names_) {
            if (qualified == symbol) {
                const auto dot = qualified.find_last_of('.');
                if (dot != std::string::npos) {
                    const auto mod = qualified.substr(0, dot);
                    const auto name = qualified.substr(dot + 1);
                    return "**" + name + "**  -  binding from `" + mod + "`.";
                }
                return "**" + qualified + "**  -  module binding.";
            }

            const auto dot = qualified.find_last_of('.');
            if (dot != std::string::npos && dot + 1 < qualified.size()) {
                const auto short_name = qualified.substr(dot + 1);
                if (short_name == symbol) {
                    const auto mod = qualified.substr(0, dot);
                    return "**" + short_name + "**  -  binding from `" + mod + "`.";
                }
            }
        }

        return {};
    }

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
    [[nodiscard]] bool is_complete_expression(const std::string& src,
                                              std::string* indent_hint = nullptr) const {
        int paren_depth = 0;
        int block_comment_depth = 0;
        bool in_string = false;
        bool in_line_comment = false;
        bool escape = false;

        if (indent_hint) indent_hint->clear();

        for (std::size_t i = 0; i < src.size(); ++i) {
            const char c = src[i];
            const char next = (i + 1 < src.size()) ? src[i + 1] : '\0';

            if (in_line_comment) {
                if (c == '\n') in_line_comment = false;
                continue;
            }

            if (block_comment_depth > 0) {
                if (c == '#' && next == '|') {
                    ++block_comment_depth;
                    ++i;
                    continue;
                }
                if (c == '|' && next == '#') {
                    --block_comment_depth;
                    ++i;
                }
                continue;
            }

            if (in_string) {
                if (escape) {
                    escape = false;
                    continue;
                }
                if (c == '\\') {
                    escape = true;
                    continue;
                }
                if (c == '"') in_string = false;
                continue;
            }

            if (c == ';') {
                in_line_comment = true;
                continue;
            }

            if (c == '#' && next == '|') {
                ++block_comment_depth;
                ++i;
                continue;
            }

            if (c == '"') {
                in_string = true;
                continue;
            }

            if (c == '(') ++paren_depth;
            else if (c == ')' && paren_depth > 0) --paren_depth;
        }

        bool dot_prefixed_continuation = false;
        std::size_t line_start = 0;
        while (line_start <= src.size()) {
            std::size_t line_end = src.find('\n', line_start);
            if (line_end == std::string::npos) line_end = src.size();

            std::string_view line(src.data() + line_start, line_end - line_start);
            const auto first_non_ws = line.find_first_not_of(" \t\r");
            if (first_non_ws != std::string_view::npos) {
                auto trimmed = line.substr(first_non_ws);
                if (!trimmed.empty() && trimmed.front() != ';') {
                    dot_prefixed_continuation = (trimmed.front() == '.');
                }
            }

            if (line_end == src.size()) break;
            line_start = line_end + 1;
        }

        const bool complete =
            (paren_depth == 0) &&
            !in_string &&
            (block_comment_depth == 0) &&
            !dot_prefixed_continuation;

        if (!complete && indent_hint) {
            if (paren_depth > 0) {
                indent_hint->assign(static_cast<std::size_t>(paren_depth) * 2u, ' ');
            } else if (dot_prefixed_continuation) {
                *indent_hint = "  ";
            } else {
                indent_hint->clear();
            }
        }

        return complete;
    }

    /**
     * @brief Request interruption of the currently executing VM run.
     */
    void request_interrupt() {
        vm_.request_interrupt();
    }

    /**
     * @brief Evaluate source and return a structured display value.
     *
     * @param source Source text to evaluate.
     * @return Structured display payload for front-end rendering.
     */
    [[nodiscard]] DisplayValue eval_to_display(const std::string& source) {
        auto forms = split_toplevel_forms(source);
        if (forms.empty()) return DisplayValue{};

        auto wrapped = wrap_repl_submission(
            forms, repl_counter_++, has_module("std.prelude"), repl_modules_);

        runtime::nanbox::LispVal result{runtime::nanbox::Nil};
        const bool ok = wrapped.last_is_expr
            ? run_source(wrapped.source, &result, wrapped.result_name)
            : run_source(wrapped.source);
        if (!ok) {
            return DisplayValue{
                .tag = DisplayTag::Error,
                .text = diagnostics_to_string(),
                .value = runtime::nanbox::Nil,
            };
        }

        repl_modules_.push_back(PriorModule{
            wrapped.module_name,
            wrapped.user_defines,
            wrapped.user_imports});
        if (!wrapped.last_is_expr || result == runtime::nanbox::Nil) {
            return DisplayValue{
                .tag = DisplayTag::Text,
                .text = {},
                .value = runtime::nanbox::Nil,
            };
        }

        return DisplayValue{
            .tag = classify_display_tag(result),
            .text = format_value(result, runtime::FormatMode::Write),
            .value = result,
        };
    }

    /**
     * @brief Override VM stdout/stderr routing with callback sinks.
     *
     * Passing an empty sink leaves the current port unchanged.
     */
    void set_stream_sinks(StreamSink stdout_sink, StreamSink stderr_sink) {
        auto stdout_for_children = stdout_sink;
        auto stderr_for_children = stderr_sink;

        if (stdout_sink) {
            set_output_port(std::make_shared<runtime::CallbackPort>(
                [sink = std::move(stdout_sink)](const std::string& text) {
                    sink(text);
                }));
        }
        if (stderr_sink) {
            set_error_port(std::make_shared<runtime::CallbackPort>(
                [sink = std::move(stderr_sink)](const std::string& text) {
                    sink(text);
                }));
        }

        /**
         * Capture the active sink routing in the spawn-thread factories so child
         * actor output keeps publishing to the same notebook cell stream.
         */
        install_actor_worker_factories(
            std::move(stdout_for_children),
            std::move(stderr_for_children));
    }

    /**
     * @brief Register a listener for actor lifecycle events.
     *
     * Events are emitted for spawned actor threads as they start and exit.
     */
    void on_actor_lifecycle(std::function<void(const ActorEvent&)> on_event) {
        if (!on_event) {
            proc_mgr_.set_debug_listener({});
            return;
        }

        proc_mgr_.set_debug_listener(
            [cb = std::move(on_event)](const eta::nng::ProcessManager::ThreadDebugEvent& ev) {
                ActorEvent out;
                out.kind = (ev.kind == eta::nng::ProcessManager::ThreadDebugEvent::Kind::Started)
                    ? ActorEvent::Kind::Started
                    : ActorEvent::Kind::Exited;
                out.index = ev.index;
                out.name = ev.name;
                try {
                    cb(out);
                } catch (...) {}
            });
    }

    /// Access the diagnostic engine (for printing / LSP forwarding).
    [[nodiscard]] diagnostic::DiagnosticEngine& diagnostics() noexcept { return diag_engine_; }
    [[nodiscard]] const diagnostic::DiagnosticEngine& diagnostics() const noexcept { return diag_engine_; }

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

    runtime::vm::VM& vm() noexcept { return vm_; }
    const runtime::vm::VM& vm() const noexcept { return vm_; }

    semantics::BytecodeFunctionRegistry& registry() noexcept { return registry_; }
    const semantics::BytecodeFunctionRegistry& registry() const noexcept { return registry_; }

    [[nodiscard]] const fs::path* path_for_file_id(uint32_t id) const noexcept {
        auto it = file_id_to_path_.find(id);
        return it != file_id_to_path_.end() ? &it->second : nullptr;
    }

    /**
     * Install a custom output port so that display/write/newline go through
     * the given port rather than falling back to std::cout.
     * Typical use in the DAP: pass a CallbackPort that fires send_event().
     */
    void set_output_port(std::shared_ptr<runtime::Port> port) {
        auto val = runtime::memory::factory::make_port(heap_, std::move(port));
        if (val) vm_.set_current_output_port(*val);
    }

    /// Install a custom error port (used by eprintln / error output).
    void set_error_port(std::shared_ptr<runtime::Port> port) {
        auto val = runtime::memory::factory::make_port(heap_, std::move(port));
        if (val) vm_.set_current_error_port(*val);
    }

    /**
     * Pre-register a file path so that its file_id is known before the file
     * is actually loaded.  The DAP uses this to install breakpoints BEFORE
     * the VM thread starts running.  If the path is already registered the
     * existing id is returned unchanged.
     */
    uint32_t ensure_file_id(const fs::path& path) {
        auto canon = canon_path_key(path);
        auto it = path_to_file_id_.find(canon);
        if (it != path_to_file_id_.end()) return it->second;
        uint32_t id = next_file_id_++;
        file_id_to_path_[id] = path;
        path_to_file_id_[canon] = id;
        return id;
    }

    /// Input is normalised before lookup so case differences on Windows are handled.
    [[nodiscard]] uint32_t file_id_for_path(const std::string& path) const {
        auto canon = canon_path_key(fs::path(path));
        auto it = path_to_file_id_.find(canon);
        return it != path_to_file_id_.end() ? it->second : 0u;
    }

    /**
     * Return every executable source line currently known for a file.
     *
     * The set is collected from emitted bytecode source maps across the
     * function registry and is suitable for DAP breakpointLocations.
     */
    [[nodiscard]] std::set<uint32_t> valid_lines_for(uint32_t file_id) const {
        std::set<uint32_t> lines;
        if (file_id == 0) return lines;

        for (const auto& fn : registry_.all()) {
            for (const auto& sp : fn.source_map) {
                if (sp.file_id == file_id && sp.start.line != 0) {
                    lines.insert(sp.start.line);
                }
            }
        }
        return lines;
    }

    /// Format a runtime value for display.
    [[nodiscard]] std::string format_value(runtime::nanbox::LispVal v,
                                           runtime::FormatMode mode = runtime::FormatMode::Write) {
        return runtime::format_value(v, mode, heap_, intern_table_);
    }

    /**
     * Install the `--mailbox` socket for a spawned child process.
     *
     * Called by main_etai.cpp when the `--mailbox <endpoint>` argument is
     * present.  Creates a PAIR socket, dials the endpoint (connecting to the
     * parent's listening socket), and stores the socket as `current-mailbox`.
     *
     * @return true on success, false if the dial fails.
     */
    bool install_mailbox(const std::string& endpoint) {
        eta::nng::NngSocketPtr sp;
        sp.protocol = eta::nng::NngProtocol::Pair;
        int rv = nng_pair0_open(&sp.socket);
        if (rv != 0) return false;

        nng_socket_set_ms(sp.socket, NNG_OPT_RECVTIMEO, 1000);

        rv = nng_dial(sp.socket, endpoint.c_str(), nullptr, 0);
        if (rv != 0) return false;
        sp.dialed = true;
        sp.endpoint_hint = endpoint;

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

    /// Populated during compilation for debugger display.
    [[nodiscard]] const std::unordered_map<uint32_t, std::string>& global_names() const noexcept {
        return global_names_;
    }

    runtime::memory::heap::Heap& heap() noexcept { return heap_; }
    const runtime::memory::heap::Heap& heap() const noexcept { return heap_; }

    /// Direct access to the intern table.
    runtime::memory::intern::InternTable& intern_table() noexcept { return intern_table_; }

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
        auto etac_res = serializer.deserialize(in, /*expected_builtins=*/0);
        if (!etac_res) {
            diag_engine_.emit_error(
                diagnostic::DiagnosticCode::ModuleNotFound, {},
                "failed to load .etac: " + std::string(runtime::vm::to_string(etac_res.error())));
            return false;
        }
        auto& etac = *etac_res;

        runtime::vm::FreshnessContext freshness;
        freshness.expected_compiler_id = runtime::vm::BytecodeSerializer::default_compiler_id();
        freshness.expected_builtin_count = static_cast<uint32_t>(builtins_.specs().size());

        auto sibling_source = path;
        sibling_source.replace_extension(".eta");
        if (std::error_code ec; fs::is_regular_file(sibling_source, ec) && !ec) {
            if (auto source_hash = hash_file_for_etac_freshness(sibling_source)) {
                freshness.expected_source_hash = *source_hash;
            }
        }

        if (auto manifest_path = find_nearest_manifest_path(path.parent_path())) {
            if (auto manifest_hash = hash_file_for_etac_freshness(*manifest_path)) {
                freshness.expected_manifest_hash = *manifest_hash;
            }
        }

        const auto freshness_result =
            runtime::vm::BytecodeSerializer::check_freshness(etac, freshness);
        if (!freshness_result.fresh()) {
            std::string message =
                "stale .etac detected: " + std::string(runtime::vm::to_string(freshness_result.status));
            if (!freshness_result.detail.empty()) {
                message += " (" + freshness_result.detail + ")";
            }
            std::error_code ec;
            if (fs::is_regular_file(sibling_source, ec) && !ec) {
                const bool fallback_ok = run_file(sibling_source);
                diag_engine_.emit_warning(
                    diagnostic::DiagnosticCode::ModuleNotFound, {},
                    message + "; falling back to source: " + sibling_source.string());
                return fallback_ok;
            }

            diag_engine_.emit_error(
                diagnostic::DiagnosticCode::ModuleNotFound, {},
                message + "; no sibling source found for fallback");
            return false;
        }

        return execute_deserialized_etac(etac);
    }

private:
    [[nodiscard]] static fs::path embedded_prelude_marker_path() {
        return fs::path("<embedded:prelude.etac>");
    }

    bool try_load_embedded_prelude() {
        const auto blob = runtime::embedded_prelude_blob();
        if (blob.empty()) return false;

        std::string bytes;
        bytes.resize(blob.size());
        for (std::size_t i = 0; i < blob.size(); ++i) {
            bytes[i] = static_cast<char>(blob[i]);
        }

        std::istringstream in(bytes, std::ios::in | std::ios::binary);
        runtime::vm::BytecodeSerializer serializer(heap_, intern_table_);
        auto etac_res = serializer.deserialize(
            in, static_cast<uint32_t>(builtins_.specs().size()));
        if (!etac_res) {
            return false;
        }
        if (!execute_deserialized_etac(*etac_res)) {
            diag_engine_.clear();
            return false;
        }
        return true;
    }

    bool execute_deserialized_etac(runtime::vm::EtacFile& etac) {
        /// Auto-load non-prelude imports
        for (const auto& imp : etac.imports) {
            if (executed_modules_.contains(imp)) continue;
            bool shadow_conflict = false;
            auto imp_path = resolve_import_path(imp, &shadow_conflict);
            if (!imp_path) {
                if (!shadow_conflict) {
                    diag_engine_.emit_error(
                        diagnostic::DiagnosticCode::ModuleNotFound, {},
                        "cannot resolve import '" + imp + "' required by .etac");
                }
                return false;
            }
            if (!run_module_file(*imp_path)) return false;
        }

        /**
         * Move functions from the deserialized registry into ours,
         * recording the base index so module init_func_index values can be offset.
         * The .etac stores 0-based (file-relative) function indices; relocate
         * them to the runner's absolute indices.
         */
        uint32_t base_idx = static_cast<uint32_t>(registry_.size());
        for (const auto& func : etac.registry.all()) {
            runtime::vm::BytecodeFunction copy = func;
            copy.rebase_func_indices(static_cast<int32_t>(base_idx));
            registry_.add(std::move(copy));
        }

        uint32_t accounted_globals = static_cast<uint32_t>(vm_.globals().size());

        /// Execute each module's _init function
        for (const auto& mod : etac.modules) {
            if (executed_modules_.contains(mod.name)) {
                if (mod.total_globals > accounted_globals) {
                    accounted_globals = mod.total_globals;
                }
                continue;
            }

            /**
             * .etac modules are loaded without source forms, but later dynamic
             * eval/compilation still re-analyzes accumulated_forms_. Reserve
             * equivalent global slots up front so synthetic eval modules cannot
             * overlap and clobber this compiled module's globals.
             */
            if (mod.total_globals > accounted_globals) {
                const auto reserve_slots = mod.total_globals - accounted_globals;
                if (!append_etac_global_reservation(reserve_slots)) {
                    return false;
                }
                accounted_globals = mod.total_globals;
            }

            auto& globals = vm_.globals();
            if (globals.size() < mod.total_globals)
                globals.resize(mod.total_globals, runtime::nanbox::Nil);

            /// Re-install builtins
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

            /// Invoke optional main
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

    bool run_module_file(const fs::path& path) {
        if (path.extension() == ".etac") return run_etac_file(path);
        return run_file(path);
    }

    [[nodiscard]] static std::optional<uint64_t>
    hash_file_for_etac_freshness(const fs::path& file_path) {
        std::ifstream in(file_path, std::ios::in | std::ios::binary);
        if (!in) return std::nullopt;

        std::ostringstream buf;
        buf << in.rdbuf();
        return runtime::vm::BytecodeSerializer::hash_source(buf.str());
    }

    [[nodiscard]] static std::optional<fs::path>
    find_nearest_manifest_path(fs::path start_dir) {
        std::error_code ec;
        start_dir = fs::weakly_canonical(start_dir, ec);
        if (ec) start_dir = start_dir.lexically_normal();
        while (!start_dir.empty()) {
            const auto manifest = start_dir / "eta.toml";
            ec.clear();
            if (fs::is_regular_file(manifest, ec) && !ec) {
                auto normalized = fs::weakly_canonical(manifest, ec);
                if (!ec) return normalized;
                return manifest.lexically_normal();
            }

            const auto parent = start_dir.parent_path();
            if (parent.empty() || parent == start_dir) break;
            start_dir = parent;
        }
        return std::nullopt;
    }

    std::optional<fs::path> resolve_import_path(const std::string& module_name,
                                                bool* shadow_conflict = nullptr) {
        auto candidates = resolver_.resolve_all(module_name);
        if (candidates.empty()) {
            if (shadow_conflict) *shadow_conflict = false;
            return std::nullopt;
        }

        if (resolver_.strict_shadow_scan() && candidates.size() > 1u) {
            if (shadow_conflict) *shadow_conflict = true;
            std::ostringstream oss;
            oss << "strict shadow mode: module '" << module_name
                << "' resolves to multiple files";
            for (const auto& candidate : candidates) {
                oss << "\n  - " << candidate.string();
            }
            diag_engine_.emit_error(
                diagnostic::DiagnosticCode::ModuleNotFound, {},
                oss.str());
            return std::nullopt;
        }

        if (shadow_conflict) *shadow_conflict = false;
        return candidates.front();
    }

    [[nodiscard]] static bool is_synthetic_eval_binding_name(std::string_view name) {
        if (name.empty() || name.size() < 2) return false;
        const char prefix = name.front();
        if (prefix != '%' && prefix != '&') return false;
        return std::all_of(name.begin() + 1, name.end(), [](unsigned char ch) {
            return std::isdigit(ch) != 0;
        });
    }

    [[nodiscard]] std::vector<EvalBinding> collect_eval_lexical_bindings() const {
        std::vector<EvalBinding> bindings;
        std::unordered_set<std::string> seen;

        auto append = [&](const std::vector<runtime::vm::VarEntry>& vars) {
            for (const auto& var : vars) {
                if (var.name.empty() || is_synthetic_eval_binding_name(var.name)) continue;
                if (!seen.insert(var.name).second) continue;
                bindings.push_back(EvalBinding{var.name, var.value});
            }
        };

        const auto frames = vm_.get_frames();
        for (std::size_t frame_index = 0; frame_index < frames.size(); ++frame_index) {
            append(vm_.get_locals(frame_index));
            append(vm_.get_upvalues(frame_index));
        }
        return bindings;
    }

    [[nodiscard]] std::string diagnostics_to_string() const {
        std::ostringstream oss;
        diag_engine_.print_all(oss, /*use_color=*/false, file_resolver());
        return oss.str();
    }

    /**
     * Reserve a contiguous global-slot range in accumulated_forms_ so future
     * source/eval compilations preserve slot layout after loading .etac modules.
     *
     * The placeholder module is marked executed and never run; it only exists to
     * keep semantic global allocation in sync with already-loaded compiled code.
     */
    bool append_etac_global_reservation(uint32_t slots_to_reserve) {
        if (slots_to_reserve == 0) return true;

        const std::string reserve_module_name =
            "__eta_etac_reserve_" + std::to_string(etac_reserve_counter_++);

        std::ostringstream source;
        source << "(module " << reserve_module_name << "\n";
        for (uint32_t i = 0; i < slots_to_reserve; ++i) {
            source << "  (define __eta_etac_slot_" << i << " '())\n";
        }
        source << ")";

        const std::string source_text = source.str();
        reader::lexer::Lexer lex(/*file_id=*/0, source_text);
        reader::parser::Parser parser(lex);
        auto parsed_res = parser.parse_toplevel();
        if (!parsed_res) {
            auto& err = parsed_res.error();
            std::visit([this](auto&& e) {
                diag_engine_.emit(diagnostic::to_diagnostic(e));
            }, err);
            return false;
        }

        reader::expander::Expander expander;
        auto expanded_res = expander.expand_many(*parsed_res);
        if (!expanded_res) {
            diag_engine_.emit(diagnostic::to_diagnostic(expanded_res.error()));
            return false;
        }

        for (auto& f : *expanded_res) {
            accumulated_forms_.push_back(reader::parser::deep_copy(f));
        }
        executed_modules_.insert(reserve_module_name);
        return true;
    }

    /**
     * Discover dotted module names from all resolver search directories.
     */
    [[nodiscard]] std::vector<std::string> discover_module_names() const {
        std::vector<std::string> out;
        std::unordered_set<std::string> seen;

        for (const auto& root : resolver_.dirs()) {
            std::error_code ec;
            if (!fs::is_directory(root, ec) || ec) continue;

            fs::recursive_directory_iterator it(
                root,
                fs::directory_options::skip_permission_denied,
                ec);
            fs::recursive_directory_iterator end;
            if (ec) continue;

            while (it != end) {
                const auto entry = *it;
                it.increment(ec);
                if (ec) {
                    ec.clear();
                    continue;
                }

                if (!entry.is_regular_file(ec) || ec) {
                    ec.clear();
                    continue;
                }

                const auto path = entry.path();
                if (path.extension() != ".eta") continue;

                auto rel = fs::relative(path, root, ec);
                if (ec) {
                    ec.clear();
                    continue;
                }

                auto mod = rel.generic_string();
                if (!mod.ends_with(".eta")) continue;
                mod.resize(mod.size() - 4);
                std::replace(mod.begin(), mod.end(), '/', '.');

                if (!mod.empty() && seen.insert(mod).second) {
                    out.push_back(std::move(mod));
                }
            }
        }

        return out;
    }

    [[nodiscard]] DisplayTag classify_display_tag(runtime::nanbox::LispVal value) const {
        using runtime::memory::heap::ObjectKind;
        std::string mime;
        if (try_unpack_jupyter_display(value, &mime, nullptr)) {
            if (auto tag = display_tag_for_mime(mime); tag != DisplayTag::Text) {
                return tag;
            }
        }

        if (!runtime::nanbox::ops::is_boxed(value) ||
            runtime::nanbox::ops::tag(value) != runtime::nanbox::Tag::HeapObject) {
            return DisplayTag::Text;
        }

        const auto id = runtime::nanbox::ops::payload(value);
        if (heap_.try_get_as<ObjectKind::Tensor, void>(id)) return DisplayTag::Tensor;
        if (heap_.try_get_as<ObjectKind::FactTable, void>(id)) return DisplayTag::FactTable;
        return DisplayTag::Text;
    }

    [[nodiscard]] static DisplayTag display_tag_for_mime(std::string_view mime) {
        if (mime == "text/html") return DisplayTag::Html;
        if (mime == "text/markdown") return DisplayTag::Markdown;
        if (mime == "text/latex") return DisplayTag::Latex;
        if (mime == "image/svg+xml") return DisplayTag::Svg;
        if (mime == "image/png") return DisplayTag::Png;
        if (mime == "application/vnd.vegalite.v5+json") return DisplayTag::VegaLite;
        if (mime == "application/vnd.eta.tensor+json") return DisplayTag::Tensor;
        if (mime == "application/vnd.eta.facttable+json") return DisplayTag::FactTable;
        return DisplayTag::Text;
    }

    [[nodiscard]] bool try_decode_string(runtime::nanbox::LispVal value,
                                         std::string* out) const {
        if (!out) return false;
        using runtime::nanbox::Tag;
        if (!runtime::nanbox::ops::is_boxed(value)) return false;

        const auto tag = runtime::nanbox::ops::tag(value);
        if (tag == Tag::String || tag == Tag::Symbol) {
            auto sv = intern_table_.get_string(runtime::nanbox::ops::payload(value));
            if (!sv) return false;
            *out = std::string(*sv);
            return true;
        }
        return false;
    }

    [[nodiscard]] bool try_unpack_jupyter_display(runtime::nanbox::LispVal value,
                                                  std::string* mime_out,
                                                  runtime::nanbox::LispVal* payload_out) const {
        using runtime::memory::heap::ObjectKind;
        using runtime::types::Vector;
        using runtime::nanbox::Tag;

        if (!runtime::nanbox::ops::is_boxed(value) ||
            runtime::nanbox::ops::tag(value) != Tag::HeapObject) {
            return false;
        }

        const auto id = runtime::nanbox::ops::payload(value);
        auto* vec = heap_.try_get_as<ObjectKind::Vector, Vector>(id);
        if (!vec || vec->elements.size() < 3) return false;

        std::string marker;
        if (!try_decode_string(vec->elements[0], &marker)) return false;
        if (marker != "jupyter-display") return false;

        std::string mime;
        if (!try_decode_string(vec->elements[1], &mime)) return false;

        if (mime_out) *mime_out = std::move(mime);
        if (payload_out) *payload_out = vec->elements[2];
        return true;
    }

    /**
     * Split input into top-level forms using parenthesis depth.
     *
     * This mirrors the REPL form splitting behaviour used by main_repl.cpp.
     */
    static std::vector<std::string> split_toplevel_forms(const std::string& input) {
        std::vector<std::string> forms;
        int depth = 0;
        bool in_string = false;
        bool escape = false;
        std::size_t form_start = std::string::npos;

        for (std::size_t i = 0; i < input.size(); ++i) {
            const char c = input[i];

            if (escape) {
                escape = false;
                continue;
            }
            if (c == '\\' && in_string) {
                escape = true;
                continue;
            }
            if (c == '"') {
                in_string = !in_string;
                if (form_start == std::string::npos) form_start = i;
                continue;
            }
            if (in_string) continue;

            if (std::isspace(static_cast<unsigned char>(c))) {
                if (depth == 0 && form_start != std::string::npos) {
                    forms.push_back(input.substr(form_start, i - form_start));
                    form_start = std::string::npos;
                }
                continue;
            }

            if (c == ';') {
                if (depth == 0 && form_start != std::string::npos) {
                    forms.push_back(input.substr(form_start, i - form_start));
                    form_start = std::string::npos;
                }
                while (i < input.size() && input[i] != '\n') ++i;
                continue;
            }

            if (form_start == std::string::npos) form_start = i;

            if (c == '(') {
                ++depth;
            } else if (c == ')') {
                --depth;
                if (depth == 0) {
                    forms.push_back(input.substr(form_start, i + 1 - form_start));
                    form_start = std::string::npos;
                }
            }
        }

        if (form_start != std::string::npos) {
            auto trailing = input.substr(form_start);
            if (trailing.find_first_not_of(" \t\n\r") != std::string::npos) {
                forms.push_back(trailing);
            }
        }

        return forms;
    }

    /**
     * Install worker factories for spawn-thread and spawn-thread-with.
     *
     * The callback sinks are captured by value at installation time, so a
     * spawned actor thread keeps writing to the stream routing that was active
     * in the spawning evaluation context.
     */
    void install_actor_worker_factories(StreamSink stdout_sink = {},
                                        StreamSink stderr_sink = {}) {
        eta::nng::ProcessManager::ThreadWorkerFn thread_worker_fn =
            [module_search_path = module_search_path_, this, stdout_sink, stderr_sink](
                const std::string& th_module_path,
                const std::string& th_func_name,
                const std::string& th_endpoint,
                std::vector<std::string> th_text_args,
                std::shared_ptr<std::atomic<bool>> alive) noexcept
        {
            try {
                auto resolver = ModulePathResolver::from_path_string(module_search_path);
                const auto child_heap = Driver::parse_heap_env_var(
                    "ETA_HEAP_SOFT_LIMIT_CHILD_THREADS",
                    Driver::parse_heap_env_var(
                        "ETA_HEAP_SOFT_LIMIT",
                        Driver::DEFAULT_CHILD_HEAP_SOFT_LIMIT_BYTES));
                Driver child(std::move(resolver), child_heap);
                child.load_prelude();

                if (stdout_sink || stderr_sink) {
                    child.set_stream_sinks(stdout_sink, stderr_sink);
                }

                if (!child.install_mailbox(th_endpoint)) {
                    alive->store(false, std::memory_order_release);
                    return;
                }

                /// Load the target module
                if (!child.run_file(fs::path(th_module_path))) {
                    alive->store(false, std::memory_order_release);
                    return;
                }

                /**
                 * Notify any DAP debug listener now that the child VM/Driver
                 * exist and have loaded source.  This lets the adapter install
                 * its per-thread stop callback and breakpoints before the
                 * spawn-thread function actually starts running user code.
                 */
                std::string th_name = fs::path(th_module_path).stem().string();
                if (!th_func_name.empty()) th_name += " (" + th_func_name + ")";
                proc_mgr_.notify_thread_started(
                    static_cast<void*>(&child.vm()),
                    static_cast<void*>(&child),
                    std::move(th_name));

                /// Build and evaluate: (func-name arg1 arg2 ...)
                std::string call_src = "(" + th_func_name;
                for (const auto& a : th_text_args) {
                    call_src += " ";
                    call_src += a;
                }
                call_src += ")";
                child.run_source(call_src);
                proc_mgr_.notify_thread_exited(static_cast<void*>(&child.vm()));
            } catch (...) {}
            alive->store(false, std::memory_order_release);
        };

        eta::nng::ProcessManager::ClosureWorkerFn closure_worker_fn =
            [module_search_path = module_search_path_, this, stdout_sink, stderr_sink](
                const std::string& th_endpoint,
                eta::nng::ProcessManager::SerializedClosure sc,
                std::shared_ptr<std::atomic<bool>> alive) noexcept
        {
            try {
                auto resolver = ModulePathResolver::from_path_string(module_search_path);
                const auto child_heap = Driver::parse_heap_env_var(
                    "ETA_HEAP_SOFT_LIMIT_CHILD_THREADS",
                    Driver::parse_heap_env_var(
                        "ETA_HEAP_SOFT_LIMIT",
                        Driver::DEFAULT_CHILD_HEAP_SOFT_LIMIT_BYTES));
                Driver child(std::move(resolver), child_heap);
                child.load_prelude();

                if (stdout_sink || stderr_sink) {
                    child.set_stream_sinks(stdout_sink, stderr_sink);
                }

                if (!child.install_mailbox(th_endpoint)) {
                    alive->store(false, std::memory_order_release);
                    return;
                }

                /// Deserialize the function registry from the etac-format blob
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

                /// Rebase and register the functions in the child's registry
                uint32_t base_idx = static_cast<uint32_t>(child.registry().size());
                for (const auto& func : etac.registry.all()) {
                    runtime::vm::BytecodeFunction copy = func;
                    copy.rebase_func_indices(static_cast<int32_t>(base_idx));
                    child.registry().add(std::move(copy));
                }

                auto capture_payload = eta::nng::deserialize_spawn_capture(
                    std::span<const uint8_t>(sc.captures_bytes),
                    child.heap(),
                    child.intern_table(),
                    [&child, base_idx](uint32_t remapped_idx)
                        -> const runtime::vm::BytecodeFunction*
                    {
                        return child.registry().get(base_idx + remapped_idx);
                    },
                    [&child](uint32_t slot) -> std::optional<runtime::nanbox::LispVal>
                    {
                        const auto& globals = child.vm().globals();
                        if (slot >= globals.size()) return std::nullopt;
                        return globals[slot];
                    });
                if (!capture_payload) {
                    alive->store(false, std::memory_order_release);
                    return;
                }

                /// Hydrate captured globals before executing the thunk.
                auto& globals = child.vm().globals();
                for (const auto& cg : capture_payload->globals) {
                    if (globals.size() <= cg.slot) globals.resize(cg.slot + 1, runtime::nanbox::Nil);
                    globals[cg.slot] = cg.value;
                }

                /// Reconstruct the Closure heap object in the child's heap.
                const auto* entry_func = child.registry().get(base_idx);
                if (!entry_func) {
                    alive->store(false, std::memory_order_release);
                    return;
                }
                auto closure_val = runtime::memory::factory::make_closure(
                    child.heap(), entry_func, std::move(capture_payload->upvals));
                if (!closure_val) {
                    alive->store(false, std::memory_order_release);
                    return;
                }

                /// Call the thunk with 0 arguments.
                proc_mgr_.notify_thread_started(
                    static_cast<void*>(&child.vm()),
                    static_cast<void*>(&child),
                    "(spawn-thread)");
                auto result = child.vm().call_value(*closure_val, {});
                if (!result) {
                }
                proc_mgr_.notify_thread_exited(static_cast<void*>(&child.vm()));
            } catch (const std::exception&) {
            } catch (...) {
            }
            alive->store(false, std::memory_order_release);
        };

        proc_mgr_.set_worker_factory(std::move(thread_worker_fn));
        proc_mgr_.set_closure_factory(std::move(closure_worker_fn));
    }

    void collect_garbage_with_registry_roots() {
        auto roots = heap_.make_external_root_frame();
        for (const auto& func : registry_.all()) {
            for (auto c : func.constants) {
                if (runtime::nanbox::ops::is_boxed(c) &&
                    runtime::nanbox::ops::tag(c) == runtime::nanbox::Tag::HeapObject) {
                    roots.push(c);
                }
            }
        }
        vm_.collect_garbage();
    }

    ModulePathResolver resolver_;
    runtime::memory::heap::Heap heap_;
    runtime::memory::intern::InternTable intern_table_;
    semantics::BytecodeFunctionRegistry registry_;
    runtime::BuiltinEnvironment builtins_;
    runtime::vm::VM vm_;

    diagnostic::DiagnosticEngine diag_engine_;

    /// IR-level optimization pipeline (runs between analyze and emit)
    semantics::OptimizationPipeline optimization_pipeline_;

    eta::nng::ProcessManager         proc_mgr_;
    runtime::nanbox::LispVal         mailbox_val_{runtime::nanbox::Nil};
    std::string                      etai_path_;
    std::string                      module_search_path_;
    std::vector<std::string>         command_line_arguments_;

    /**
     * Auto-detect the path to the etai binary at startup.
     * Reads /proc/self/exe on Linux, then looks for "etai" in the same
     * directory.  Falls back to "etai" (searched on PATH).
     */
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
        /// macOS: use _NSGetExecutablePath
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
        return "etai"; ///< fallback to PATH
    }

    /**
     * Accumulated expanded forms from all prior run_source calls.
     * The linker clears its state on each index_modules() call, so we must
     * re-feed ALL modules each time for correct cross-module resolution.
     */
    std::vector<reader::parser::SExprPtr> accumulated_forms_;

    /// Names of modules that have already been executed (to avoid re-running).
    std::unordered_set<std::string> executed_modules_;

    /// Track which files we've already loaded (to avoid double-loading prelude etc.)
    std::unordered_set<std::string> loaded_files_;
    std::unordered_set<std::string> indexed_source_files_;
    std::optional<fs::path> prelude_origin_path_;

    /// File ID allocator for diagnostic spans
    uint32_t next_file_id_;

    std::unordered_map<uint32_t, fs::path>    file_id_to_path_;
    std::unordered_map<std::string, uint32_t> path_to_file_id_;

    /// Whether builtins have been installed into VM globals yet
    bool builtins_installed_{false};

    /// Guard against recursive auto-loading cycles
    std::unordered_set<std::string> loading_modules_;

    std::unordered_map<uint32_t, std::string> global_names_;
    int repl_counter_{0};
    uint64_t eval_counter_{0};
    uint64_t etac_reserve_counter_{0};
    std::vector<std::string> active_module_init_stack_;
    std::vector<PriorModule> repl_modules_;

    /**
     * Normalise a path to a stable lowercase key used in path_to_file_id_.
     * On Windows this lowercases and ensures backslashes; on POSIX it is
     * just fs::absolute + lexically_normal.
     */
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

    /**
     * Collect all module names referenced in (import ...) clauses within
     * a set of expanded forms.  Scans the top-level module lists for
     * (import <sym>) / (import (only <sym> ...) ...) etc.
     */
    static std::vector<std::string> collect_imported_modules(
            std::span<const reader::parser::SExprPtr> forms) {
        std::vector<std::string> result;
        std::unordered_set<std::string> seen;

        auto extract_module_name = [](const reader::parser::SExprPtr& clause) -> std::string {
            namespace utils = reader::utils;
            /// Plain symbol:  std.core
            if (auto s = utils::as_symbol(clause)) return s->name;
            /// List form:  (only std.core ...) / (except ...) / (rename ...) / (prefix ...)
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
            /// Walk body for (import ...) forms
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

    [[nodiscard]] static bool form_declares_module(
            const reader::parser::SExprPtr& form,
            const std::string& module_name) {
        auto* lst = form ? form->template as<reader::parser::List>() : nullptr;
        if (!lst || lst->elems.size() < 2) return false;
        if (!reader::utils::is_symbol_named(lst->elems[0], "module")) return false;
        auto* nsym = lst->elems[1]->template as<reader::parser::Symbol>();
        return nsym && nsym->name == module_name;
    }

    [[nodiscard]] bool module_declared(
            const std::string& module_name,
            std::span<const reader::parser::SExprPtr> new_forms) const {
        for (const auto& f : accumulated_forms_) {
            if (form_declares_module(f, module_name)) return true;
        }
        for (const auto& f : new_forms) {
            if (form_declares_module(f, module_name)) return true;
        }
        return false;
    }

    bool hydrate_executed_module_source(const std::string& module_name) {
        bool shadow_conflict = false;
        auto resolved = resolve_import_path(module_name, &shadow_conflict);
        if (!resolved) return !shadow_conflict;

        fs::path source_path = *resolved;
        if (source_path.extension() == ".etac") {
            auto sibling_source = source_path;
            sibling_source.replace_extension(".eta");
            std::error_code ec;
            if (!fs::is_regular_file(sibling_source, ec) || ec) {
                return true;
            }
            source_path = sibling_source;
        } else if (source_path.extension() != ".eta") {
            return true;
        }

        const auto source_key = canon_path_key(source_path);
        if (indexed_source_files_.contains(source_key)) return true;

        indexed_source_files_.insert(source_key);
        if (!run_file(source_path)) {
            indexed_source_files_.erase(source_key);
            return false;
        }
        return true;
    }

    /**
     * Auto-load module files from the module path for any import that
     * references a module not yet in the accumulated set.
     * When execute is false, only the top-level target skips execution;
     * auto-loaded dependencies are always executed (they must populate globals).
     * Returns false on failure; diagnostics are emitted.
     */
    bool auto_load_imports(std::span<const reader::parser::SExprPtr> new_forms) {
        auto needed = collect_imported_modules(new_forms);
        for (const auto& mod_name : needed) {
            const bool already_accumulated = module_declared(mod_name, new_forms);
            if (already_accumulated) continue;

            if (executed_modules_.contains(mod_name)) {
                if (!hydrate_executed_module_source(mod_name)) return false;
                continue;
            }

            if (loading_modules_.contains(mod_name)) {
                /// Build the cycle description for a helpful error message.
                std::string cycle;
                for (const auto& m : loading_modules_) {
                    if (!cycle.empty()) cycle += " -> ";
                    cycle += m;
                }
                cycle += " -> ";
                cycle += mod_name;
                diag_engine_.emit_error(
                    diagnostic::DiagnosticCode::ModuleNotFound, {},
                    "circular module import detected: " + cycle);
                return false;
            }

            /// Try to resolve and load the module file
            bool shadow_conflict = false;
            auto path = resolve_import_path(mod_name, &shadow_conflict);
            if (!path) {
                if (shadow_conflict) return false;
                continue; ///< Will fail later at link time with a clear error
            }

            auto canonical = path->string();
            if (loaded_files_.contains(canonical)) continue;

            loading_modules_.insert(mod_name);
            loaded_files_.insert(canonical);
            bool ok = run_module_file(*path);
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

        /// Lex + Parse
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
            return true;
        }

        /// Expand
        reader::expander::Expander expander;
        auto expanded_res = expander.expand_many(parsed);
        if (!expanded_res) {
            diag_engine_.emit(diagnostic::to_diagnostic(expanded_res.error()));
            return false;
        }
        auto new_expanded = std::move(*expanded_res);

        /// Remember which modules are new (not yet executed)
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

        /// Auto-load imported modules from the module path
        std::span<const reader::parser::SExprPtr> new_span(
            new_expanded.data(), new_expanded.size());
        if (!auto_load_imports(new_span)) {
            return false;
        }

        /**
         * Rollback boundary for this submission.
         * Keep modules auto-loaded while resolving imports above, but
         * discard forms added by this submission (including nested eval).
         */
        const std::size_t accumulated_forms_base = accumulated_forms_.size();

        /**
         * Record non-prelude imports in the compile result so the
         * serializer can embed them in .etac files.
         */
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

        /// Append new forms to the accumulated set
        for (auto& f : new_expanded) {
            accumulated_forms_.push_back(reader::parser::deep_copy(f));
        }

        /// Link ALL accumulated forms
        reader::ModuleLinker linker;
        const auto rollback_accumulated_forms = [&]() {
            accumulated_forms_.resize(accumulated_forms_base);
        };
        auto idx_res = linker.index_modules(accumulated_forms_);
        if (!idx_res) {
            /// Rollback source additions from this run (including nested eval additions).
            rollback_accumulated_forms();
            emit_link_error(idx_res.error());
            return false;
        }
        auto link_res = linker.link();
        if (!link_res) {
            rollback_accumulated_forms();
            emit_link_error(link_res.error());
            return false;
        }

        /// Semantic analysis (all accumulated modules)
        semantics::SemanticAnalyzer sa;
        auto sem_res = sa.analyze_all(accumulated_forms_, linker, builtins_);
        if (!sem_res) {
            rollback_accumulated_forms();
            diag_engine_.emit(diagnostic::to_diagnostic(sem_res.error()));
            return false;
        }
        auto sem_mods = std::move(*sem_res);
        if (sem_mods.empty()) return true;

        /// Run IR optimization passes
        optimization_pipeline_.run_all(sem_mods);

        /**
         * Emit + Execute only NEW modules
         * Grow globals vector if needed, preserving existing values.
         * Re-install builtins in slots 0..N-1 (heap objects may have been GC'd).
         */
        auto& globals = vm_.globals();
        auto needed = sem_mods[0].total_globals;
        if (globals.size() < needed) {
            globals.resize(needed, runtime::nanbox::Nil);
        }

        if (execute) {
            /// Re-install builtin primitives at their fixed slots
            for (std::size_t i = 0; i < builtins_.specs().size(); ++i) {
                const auto& spec = builtins_.specs()[i];
                auto prim = runtime::memory::factory::make_primitive(
                    heap_, spec.func, spec.arity, spec.has_rest);
                if (!prim) {
                    rollback_accumulated_forms();
                    emit_runtime_error(prim.error());
                    return false;
                }
                globals[i] = *prim;
            }
            builtins_installed_ = true;
        }

        /// Track the registry range for newly emitted functions.
        uint32_t base_func_idx = static_cast<uint32_t>(registry_.size());

        for (auto& mod : sem_mods) {
            if (executed_modules_.contains(mod.name)) {
                continue; ///< Already executed in a prior call
            }

            semantics::Emitter emitter(mod, heap_, intern_table_, registry_);
            auto* init_func = emitter.emit();

            /// Prefix with "module." so the UI can group by module.
            for (const auto& bi : mod.bindings) {
                if (bi.kind == semantics::BindingInfo::Kind::Global && !bi.name.empty()) {
                    global_names_[bi.slot] = mod.name + "." + bi.name;
                }
            }

            /// Record compile metadata for this module.
            if (out_cr) {
                CompileModuleEntry cme;
                cme.name = mod.name;
                /// init_func is the last function added by emitter.emit()
                cme.init_func_index = static_cast<uint32_t>(registry_.size()) - 1 - base_func_idx;
                cme.total_globals = mod.total_globals;
                cme.main_func_slot = mod.main_func_slot;
                out_cr->modules.push_back(std::move(cme));
            }

            if (execute) {
                struct ActiveModuleInitGuard {
                    std::vector<std::string>& active_module_init_stack;

                    explicit ActiveModuleInitGuard(
                        std::vector<std::string>& stack,
                        const std::string& module_name)
                        : active_module_init_stack(stack) {
                        active_module_init_stack.push_back(module_name);
                    }

                    ~ActiveModuleInitGuard() {
                        active_module_init_stack.pop_back();
                    }
                };

                ActiveModuleInitGuard active_module_guard(active_module_init_stack_, mod.name);
                auto exec_res = vm_.execute(*init_func);
                if (!exec_res) {
                    /// Rollback this run's source additions so failed modules don't poison later submissions.
                    rollback_accumulated_forms();
                    emit_runtime_error(exec_res.error());
                    return false;
                }

                executed_modules_.insert(mod.name);

                /// Invoke optional (defun main ...) entry point
                if (mod.main_func_slot) {
                    auto main_val = globals[*mod.main_func_slot];
                    if (main_val != runtime::nanbox::Nil) {
                        auto main_res = vm_.call_value(main_val, {});
                        if (!main_res) {
                            /// Rollback for failed main invocation too.
                            rollback_accumulated_forms();
                            emit_runtime_error(main_res.error());
                            return false;
                        }
                    }
                }

                /// For REPL: capture result from the last NEW module
                if (result && !result_binding.empty()) {
                    /// Check if this is the last new module
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

} ///< namespace eta::session

