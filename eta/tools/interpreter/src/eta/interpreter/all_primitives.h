#pragma once

/**
 * @file all_primitives.h
 * @brief Single source of truth for ALL live primitive registrations.
 *
 * Every executable that runs Eta code must call register_all_primitives()
 * (which handles core + port + io + os + process + time and sidecar placeholders
 * for torch + stats + log + nng).
 *
 * Canonical registration order (MUST match builtin_catalog order exactly):
 *   1. core_primitives.h
 *   2. port_primitives.h
 *   3. io_primitives.h
 *   4. os_primitives.h
 *   5. process_primitives.h
 *   6. time_primitives.h
 *   7. torch_primitives.h
 *   8. stats_primitives.h
 *   9. log_primitives.h
 *  10. nng_primitives.h  (placeholder registration only)
 *
 * For analysis-only tools (LSP), register_builtin_specs() provides null-func
 */

#include "eta/runtime/core_primitives.h"
#include "eta/runtime/port_primitives.h"
#include "eta/runtime/io_primitives.h"
#include "eta/runtime/os_primitives.h"
#include "eta/runtime/process_primitives.h"
#include "eta/runtime/time_primitives.h"
#include "eta/runtime/builtin_metadata.h"
#include <span>
#include <string>
#include <string_view>
/**
 * Driver/sidecar-specific wiring (mailbox + actor runtime hooks) is bound later
 * when sidecars are loaded and overwrite these placeholder slots.
 */

namespace eta::interpreter {

namespace detail {

[[nodiscard]] inline runtime::types::PrimitiveFunc make_missing_sidecar_primitive(
    std::string symbol_name,
    std::string package_name) {
    return [symbol_name = std::move(symbol_name), package_name = std::move(package_name)](
               runtime::types::PrimitiveArgs)
               -> std::expected<runtime::nanbox::LispVal, runtime::error::RuntimeError> {
        runtime::error::VMError error;
        error.code = runtime::error::RuntimeErrorCode::InternalError;
        error.message = "native sidecar primitive '" + symbol_name
            + "' requires package dependency '" + package_name + "'";
        return std::unexpected(runtime::error::RuntimeError{error});
    };
}

inline void register_sidecar_placeholders(runtime::BuiltinEnvironment& env,
                                          const std::string_view package_name) {
    for (const auto& builtin : runtime::builtin_metadata()) {
        const auto owner = runtime::builtin_native_sidecar_package(builtin.name);
        if (!owner.has_value()) continue;
        if (*owner != package_name) continue;
        env.register_builtin(
            builtin.name,
            builtin.arity,
            builtin.has_rest,
            make_missing_sidecar_primitive(
                std::string(builtin.name), std::string(package_name)));
    }
}

inline void register_torch_sidecar_placeholders(runtime::BuiltinEnvironment& env) {
    register_sidecar_placeholders(env, "eta-torch");
}

inline void register_stats_sidecar_placeholders(runtime::BuiltinEnvironment& env) {
    register_sidecar_placeholders(env, "eta-stats");
}

inline void register_log_sidecar_placeholders(runtime::BuiltinEnvironment& env) {
    register_sidecar_placeholders(env, "eta-log");
}

inline void register_nng_sidecar_placeholders(runtime::BuiltinEnvironment& env) {
    register_sidecar_placeholders(env, "eta-nng");
}

} // namespace detail

/**
 * Register core primitive implementations and sidecar placeholders for
 * torch/stats/log/nng.
 */
inline void register_all_primitives(
    runtime::BuiltinEnvironment& env,
    runtime::memory::heap::Heap& heap,
    runtime::memory::intern::InternTable& intern,
    runtime::vm::VM& vm,
    std::span<const std::string> command_line_arguments = {})
{
    /// Order MUST match builtin_catalog order (see canonical order above)
    runtime::register_core_primitives(env, heap, intern, &vm);
    runtime::register_port_primitives(env, heap, intern, vm);
    runtime::register_io_primitives(env, heap, intern, vm);
    runtime::register_os_primitives(env, heap, intern, vm, command_line_arguments);
    runtime::register_process_primitives(env, heap, intern, vm);
    runtime::register_time_primitives(env, heap, intern, &vm);
    detail::register_torch_sidecar_placeholders(env);
    detail::register_stats_sidecar_placeholders(env);
    detail::register_log_sidecar_placeholders(env);
    detail::register_nng_sidecar_placeholders(env);
}

} ///< namespace eta::interpreter

