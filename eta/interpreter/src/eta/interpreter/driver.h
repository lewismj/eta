#pragma once

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
#include "eta/runtime/vm/vm.h"
#include "eta/runtime/builtin_env.h"
#include "eta/runtime/core_primitives.h"
#include "eta/runtime/io_primitives.h"
#include "eta/runtime/port_primitives.h"
#include "eta/runtime/value_formatter.h"
#include "eta/diagnostic/diagnostic.h"

#include "module_path.h"

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
    explicit Driver(ModulePathResolver resolver, std::size_t heap_bytes = 4 * 1024 * 1024)
        : resolver_(std::move(resolver)),
          heap_(heap_bytes),
          intern_table_(),
          registry_(),
          builtins_(),
          vm_(heap_, intern_table_),
          diag_engine_(),
          next_file_id_(1) // 0 is reserved for REPL / anonymous input
    {
        // Phase 1: core primitives (no VM dependency)
        runtime::register_core_primitives(builtins_, heap_, intern_table_);
        // Phase 2: port primitives (require VM reference)
        runtime::register_port_primitives(builtins_, heap_, intern_table_, vm_);
        // Phase 3: I/O primitives (require VM for port-aware display/newline)
        runtime::register_io_primitives(builtins_, heap_, intern_table_, vm_);

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

    /// Access the module path resolver.
    [[nodiscard]] ModulePathResolver& resolver() noexcept { return resolver_; }

    /// Format a runtime value for display.
    [[nodiscard]] std::string format_value(runtime::nanbox::LispVal v,
                                           runtime::FormatMode mode = runtime::FormatMode::Write) {
        return runtime::format_value(v, mode, heap_, intern_table_);
    }

private:
    ModulePathResolver resolver_;
    runtime::memory::heap::Heap heap_;
    runtime::memory::intern::InternTable intern_table_;
    semantics::BytecodeFunctionRegistry registry_;
    runtime::BuiltinEnvironment builtins_;
    runtime::vm::VM vm_;

    diagnostic::DiagnosticEngine diag_engine_;

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

    // Whether builtins have been installed into VM globals yet
    bool builtins_installed_{false};

    // Guard against recursive auto-loading cycles
    std::unordered_set<std::string> loading_modules_;

    uint32_t allocate_file_id(const std::string& path) {
        // TODO: maintain file_id -> path map for diagnostic rendering
        (void)path;
        return next_file_id_++;
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
     * @brief Core implementation: compile + execute source text.
     *
     * Accumulates expanded forms and re-links everything each time (the
     * ModuleLinker is non-incremental). Only newly-added modules are
     * emitted and executed — previously-run modules are skipped.
     */
    bool run_source_impl(const std::string& source, uint32_t file_id,
                         runtime::nanbox::LispVal* result = nullptr,
                         const std::string& result_binding = {}) {
        diag_engine_.clear();

        // ── Lex + Parse ──────────────────────────────────────────────
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

        // ── Expand ───────────────────────────────────────────────────
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

        // ── Auto-load imported modules from the module path ──────────
        std::span<const reader::parser::SExprPtr> new_span(
            new_expanded.data(), new_expanded.size());
        if (!auto_load_imports(new_span)) {
            return false;
        }

        // Append new forms to the accumulated set
        for (auto& f : new_expanded) {
            accumulated_forms_.push_back(reader::parser::deep_copy(f));
        }

        // ── Link ALL accumulated forms ───────────────────────────────
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

        // ── Semantic analysis (all accumulated modules) ──────────────
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

        // ── Emit + Execute only NEW modules ──────────────────────────
        // Grow globals vector if needed, preserving existing values.
        // Re-install builtins in slots 0..N-1 (heap objects may have been GC'd).
        auto& globals = vm_.globals();
        auto needed = sem_mods[0].total_globals;
        if (globals.size() < needed) {
            globals.resize(needed, runtime::nanbox::Nil);
        }
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

        for (auto& mod : sem_mods) {
            if (executed_modules_.contains(mod.name)) {
                continue; // Already executed in a prior call
            }

            semantics::Emitter emitter(mod, heap_, intern_table_, registry_);
            auto* main_func = emitter.emit();

            auto exec_res = vm_.execute(*main_func);
            if (!exec_res) {
                emit_runtime_error(exec_res.error());
                return false;
            }

            executed_modules_.insert(mod.name);

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

