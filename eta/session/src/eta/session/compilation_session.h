/**
 * @file compilation_session.h
 * @brief Incremental compilation/link state for eta session runtimes.
 */

#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "eta/reader/module_linker.h"
#include "eta/reader/parser.h"
#include "eta/runtime/error.h"
#include "eta/runtime/nanbox.h"

namespace eta::diagnostic {
class DiagnosticEngine;
}

namespace eta::runtime {

class BuiltinEnvironment;
class ExtensionEnvironment;

namespace memory::heap {
class Heap;
}

namespace memory::intern {
class InternTable;
}

namespace vm {
class VM;
struct ModuleEntry;
}

} // namespace eta::runtime

namespace eta::semantics {
class BytecodeFunctionRegistry;
class OptimizationPipeline;
struct ModuleSemantics;
} // namespace eta::semantics

namespace eta::session {

class RuntimePrimitiveInstaller;

/**
 * @brief Owns incremental source compilation and module-link session state.
 */
class CompilationSession {
public:
    struct CompileModuleEntry {
        struct ImportBinding {
            uint32_t local_slot{0};
            std::string from_module;
            std::string remote_name;
        };

        struct ExportBinding {
            std::string name;
            uint32_t slot{0};
        };

        std::string name;
        uint32_t init_func_index{0};
        uint32_t total_globals{0};
        std::optional<uint32_t> main_func_slot;
        uint32_t first_func_index{0};
        uint32_t func_count{0};
        std::vector<uint32_t> owned_global_slots;
        std::vector<ImportBinding> import_bindings;
        std::vector<ExportBinding> export_bindings;
    };

    struct CompileResult {
        std::vector<CompileModuleEntry> modules;
        std::vector<std::string> imports;
        uint32_t base_func_idx{0};
        uint32_t end_func_idx{0};
    };

    struct RuntimeModuleInfo {
        std::unordered_map<std::string, uint32_t> export_slots;
    };

    struct CompiledModuleLinkInfo {
        std::string name;
        std::vector<std::string> exports;
        std::filesystem::path artifact_path;
    };

    /**
     * @brief RAII guard that temporarily marks active module init frames as executed.
     *
     * This is used by runtime eval compilation so import resolution treats the
     * currently executing module stack as already executed.
     */
    class ActiveModuleExecutionGuard {
    public:
        ActiveModuleExecutionGuard() = default;
        ActiveModuleExecutionGuard(const ActiveModuleExecutionGuard&) = delete;
        ActiveModuleExecutionGuard& operator=(const ActiveModuleExecutionGuard&) = delete;
        ActiveModuleExecutionGuard(ActiveModuleExecutionGuard&& other) noexcept;
        ActiveModuleExecutionGuard& operator=(ActiveModuleExecutionGuard&& other) noexcept;
        ~ActiveModuleExecutionGuard();

    private:
        friend class CompilationSession;

        explicit ActiveModuleExecutionGuard(CompilationSession* session) noexcept
            : session_(session) {}

        void reset() noexcept;

        CompilationSession* session_{nullptr};
        std::vector<std::string> inserted_modules_;
    };

    /**
     * @brief Runtime host operations used by CompilationSession.
     */
    class Host {
    public:
        virtual ~Host() = default;

        [[nodiscard]] virtual std::optional<std::filesystem::path> resolve_import_path(
            const std::string& module_name,
            bool* shadow_conflict = nullptr) = 0;
        virtual bool run_module_file(const std::filesystem::path& path) = 0;
        virtual void emit_link_error(const reader::LinkError& error) = 0;
        virtual void emit_runtime_error(const runtime::error::RuntimeError& error) = 0;

        [[nodiscard]] virtual diagnostic::DiagnosticEngine& diagnostics() noexcept = 0;
        [[nodiscard]] virtual runtime::vm::VM& vm() noexcept = 0;
        [[nodiscard]] virtual runtime::memory::heap::Heap& heap() noexcept = 0;
        [[nodiscard]] virtual runtime::memory::intern::InternTable& intern_table() noexcept = 0;
        [[nodiscard]] virtual semantics::BytecodeFunctionRegistry& registry() noexcept = 0;
        [[nodiscard]] virtual runtime::BuiltinEnvironment& builtins() noexcept = 0;
        [[nodiscard]] virtual runtime::ExtensionEnvironment& extensions() noexcept = 0;
        [[nodiscard]] virtual RuntimePrimitiveInstaller& primitive_installer() noexcept = 0;
        [[nodiscard]] virtual semantics::OptimizationPipeline& optimization_pipeline() noexcept = 0;
        [[nodiscard]] virtual std::size_t total_primitive_count() const noexcept = 0;
    };

    [[nodiscard]] bool has_module(const std::string& name) const noexcept;
    [[nodiscard]] bool can_register_extension_primitives() const noexcept;

    [[nodiscard]] const std::optional<std::filesystem::path>& prelude_origin_path() const noexcept;
    void set_prelude_origin_path(std::optional<std::filesystem::path> path);

    [[nodiscard]] const std::unordered_map<uint32_t, std::string>& global_names() const noexcept;
    [[nodiscard]] std::unordered_map<uint32_t, std::string>& mutable_global_names() noexcept;

    [[nodiscard]] const std::unordered_map<std::string, CompiledModuleLinkInfo>&
    compiled_link_modules() const noexcept;
    [[nodiscard]] const RuntimeModuleInfo* runtime_module_info(
        const std::string& module_name) const noexcept;
    [[nodiscard]] std::optional<uint32_t> runtime_export_slot(
        std::string_view module_name,
        std::string_view export_name) const;

    void mark_module_executed(std::string module_name);
    [[nodiscard]] ActiveModuleExecutionGuard make_active_module_execution_guard();

    bool clear_module_cache(const std::string& module_name);

    bool run_source_impl(Host& host,
                         const std::string& source,
                         uint32_t file_id,
                         runtime::nanbox::LispVal* result = nullptr,
                         const std::string& result_binding = {},
                         bool execute = true,
                         CompileResult* out_cr = nullptr);

    bool append_etac_global_reservation(Host& host,
                                        uint32_t slots_to_reserve,
                                        std::string* reserve_module_name = nullptr);
    bool release_etac_global_reservation(const std::string& module_name);
    void add_etac_module_reservation(const std::string& module_name,
                                     std::string reserve_module_name);

    bool hydrate_executed_module_source(Host& host, const std::string& module_name);

    void record_runtime_exports_from_source_module(const semantics::ModuleSemantics& mod);
    void record_runtime_exports_from_compiled_module(
        const std::string& module_name,
        const std::unordered_map<std::string, uint32_t>& export_slots);
    void record_compiled_link_exports_from_compiled_module(
        const runtime::vm::ModuleEntry& module,
        const std::filesystem::path& artifact_path);

private:
    [[nodiscard]] static std::vector<std::string> collect_imported_modules(
        std::span<const reader::parser::SExprPtr> forms);
    [[nodiscard]] static std::unordered_set<std::string> collect_declared_module_names(
        std::span<const reader::parser::SExprPtr> forms);
    [[nodiscard]] static bool form_declares_module(
        const reader::parser::SExprPtr& form,
        const std::string& module_name);
    [[nodiscard]] bool module_declared(
        const std::string& module_name,
        std::span<const reader::parser::SExprPtr> new_forms) const;
    bool auto_load_imports(Host& host, std::span<const reader::parser::SExprPtr> new_forms);
    void drop_etac_reservation_modules(const std::vector<std::string>& reserve_modules);
    void unmark_executed_modules(const std::vector<std::string>& module_names);

    std::vector<reader::parser::SExprPtr> accumulated_forms_;
    std::unordered_set<std::string> executed_modules_;
    std::unordered_set<std::string> loaded_files_;
    std::unordered_set<std::string> indexed_source_files_;
    std::optional<std::filesystem::path> prelude_origin_path_;
    std::unordered_set<std::string> loading_modules_;
    std::unordered_map<uint32_t, std::string> global_names_;
    std::unordered_map<std::string, RuntimeModuleInfo> runtime_module_info_;
    std::unordered_map<std::string, CompiledModuleLinkInfo> compiled_link_modules_;
    uint64_t etac_reserve_counter_{0};
    std::unordered_map<std::string, std::vector<std::string>> etac_module_reservations_;
    std::vector<std::string> active_module_init_stack_;
};

} // namespace eta::session
