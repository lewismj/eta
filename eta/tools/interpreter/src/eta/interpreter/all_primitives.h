#pragma once

/**
 * @file all_primitives.h
 * @brief Single source of truth for ALL live primitive registrations.
 *
 * Every executable that runs Eta code must call register_all_primitives()
 * (which handles core + port + io + os + process + time and sidecar placeholders
 * for torch + stats + log).
 *
 * Canonical registration order  (MUST match builtin_names.h exactly):
 *   1. core_primitives.h
 *   2. port_primitives.h
 *   3. io_primitives.h
 *   4. os_primitives.h
 *   5. process_primitives.h
 *   6. time_primitives.h
 *   7. torch_primitives.h
 *   8. stats_primitives.h
 *   9. log_primitives.h
 *  10. nng_primitives.h  (registered separately by the Driver)
 *
 * For analysis-only tools (LSP), builtin_names.h provides null-func
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
 * driver-specific arguments (ProcessManager, etai path, mailbox, etc.)
 * and must be called by the Driver after register_all_primitives().
 */

namespace eta::interpreter {

namespace detail {

[[nodiscard]] inline bool is_log_primitive_name(const std::string_view name) {
    return name.rfind("%log-", 0u) == 0u;
}

[[nodiscard]] inline bool is_stats_primitive_name(const std::string_view name) {
    return name == "%stats-mean-vec"
        || name == "%stats-var-vec"
        || name == "%stats-cov-matrix"
        || name == "%stats-cor-matrix"
        || name == "%stats-quantile-vec"
        || name == "%stats-ols-multi";
}

[[nodiscard]] inline bool is_torch_primitive_name(const std::string_view name) {
    return name.rfind("torch/", 0u) == 0u
        || name.rfind("nn/", 0u) == 0u
        || name.rfind("optim/", 0u) == 0u;
}

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

template <typename Predicate>
inline void register_sidecar_placeholders(runtime::BuiltinEnvironment& env,
                                          Predicate&& predicate,
                                          const std::string_view package_name) {
    for (const auto& builtin : runtime::builtin_metadata()) {
        if (!predicate(builtin.name)) continue;
        env.register_builtin(
            builtin.name,
            builtin.arity,
            builtin.has_rest,
            make_missing_sidecar_primitive(
                std::string(builtin.name), std::string(package_name)));
    }
}

inline void register_torch_sidecar_placeholders(runtime::BuiltinEnvironment& env) {
    register_sidecar_placeholders(env, is_torch_primitive_name, "eta-torch");
}

inline void register_stats_sidecar_placeholders(runtime::BuiltinEnvironment& env) {
    register_sidecar_placeholders(env, is_stats_primitive_name, "eta-stats");
}

inline void register_log_sidecar_placeholders(runtime::BuiltinEnvironment& env) {
    register_sidecar_placeholders(env, is_log_primitive_name, "eta-log");
}

} // namespace detail

/**
 * Register core primitive implementations and sidecar placeholders for
 * torch/stats/log.
 *
 * NNG placeholders are installed by the Driver to complete the full builtin set.
 */
inline void register_all_primitives(
    runtime::BuiltinEnvironment& env,
    runtime::memory::heap::Heap& heap,
    runtime::memory::intern::InternTable& intern,
    runtime::vm::VM& vm,
    std::span<const std::string> command_line_arguments = {})
{
    /// Order MUST match builtin_names.h  (see canonical order above)
    runtime::register_core_primitives(env, heap, intern, &vm);
    runtime::register_port_primitives(env, heap, intern, vm);
    runtime::register_io_primitives(env, heap, intern, vm);
    runtime::register_os_primitives(env, heap, intern, vm, command_line_arguments);
    runtime::register_process_primitives(env, heap, intern, vm);
    runtime::register_time_primitives(env, heap, intern, &vm);
    detail::register_torch_sidecar_placeholders(env);
    detail::register_stats_sidecar_placeholders(env);
    detail::register_log_sidecar_placeholders(env);
}

} ///< namespace eta::interpreter

