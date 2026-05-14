/**
 * @file etac_loader.h
 * @brief `.etac` artifact loading and execution for eta sessions.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <unordered_map>

#include "eta/runtime/error.h"
#include "eta/session/compilation_session.h"

namespace eta::diagnostic {
class DiagnosticEngine;
}

namespace eta::runtime {

namespace memory::heap {
class Heap;
}

namespace memory::intern {
class InternTable;
}

namespace vm {
struct BytecodeFunction;
struct EtacFile;
class VM;
} // namespace vm

} // namespace eta::runtime

namespace eta::semantics {
class BytecodeFunctionRegistry;
} // namespace eta::semantics

namespace eta::session {

class RuntimePrimitiveInstaller;

/**
 * @brief Owns `.etac` deserialization, freshness checks, and module execution.
 */
class EtacLoader {
public:
    /**
     * @brief Runtime host operations required by EtacLoader.
     */
    class Host {
    public:
        virtual ~Host() = default;

        virtual bool ensure_package_sidecars_loaded(
            std::optional<std::filesystem::path> start_dir) = 0;
        [[nodiscard]] virtual std::optional<std::filesystem::path> resolve_import_path(
            const std::string& module_name,
            bool* shadow_conflict = nullptr) = 0;
        virtual bool run_module_file(const std::filesystem::path& path) = 0;
        virtual bool run_source_file(const std::filesystem::path& path) = 0;
        virtual void emit_runtime_error(
            const runtime::error::RuntimeError& error) = 0;

        [[nodiscard]] virtual diagnostic::DiagnosticEngine& diagnostics() noexcept = 0;
        [[nodiscard]] virtual runtime::vm::VM& vm() noexcept = 0;
        [[nodiscard]] virtual runtime::memory::heap::Heap& heap() noexcept = 0;
        [[nodiscard]] virtual runtime::memory::intern::InternTable& intern_table() noexcept = 0;
        [[nodiscard]] virtual semantics::BytecodeFunctionRegistry& registry() noexcept = 0;
        [[nodiscard]] virtual RuntimePrimitiveInstaller& primitive_installer() noexcept = 0;

        [[nodiscard]] virtual std::size_t total_primitive_count() const noexcept = 0;
        [[nodiscard]] virtual std::size_t builtin_count() const noexcept = 0;
        [[nodiscard]] virtual std::uint64_t extension_env_hash() const noexcept = 0;
    };

    EtacLoader(Host& host,
               CompilationSession& compilation,
               CompilationSession::Host& compilation_host) noexcept
        : host_(host),
          compilation_(compilation),
          compilation_host_(compilation_host) {}

    bool run_etac_file(const std::filesystem::path& path);

private:
    static void relocate_function_global_slots(
        runtime::vm::BytecodeFunction& func,
        const std::unordered_map<std::uint32_t, std::uint32_t>& slot_map);

    [[nodiscard]] static std::optional<std::uint64_t> hash_file_for_etac_freshness(
        const std::filesystem::path& file_path);

    bool execute_deserialized_etac(runtime::vm::EtacFile& etac,
                                   const std::filesystem::path& artifact_path);

    Host& host_;
    CompilationSession& compilation_;
    CompilationSession::Host& compilation_host_;
};

} // namespace eta::session
