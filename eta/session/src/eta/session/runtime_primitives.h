/**
 * @file runtime_primitives.h
 * @brief Runtime primitive installation and naming helpers.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <string>
#include <unordered_map>
#include <vector>

#include "eta/runtime/builtin_env.h"
#include "eta/runtime/extension_env.h"
#include "eta/runtime/memory/heap.h"
#include "eta/runtime/nanbox.h"

namespace eta::session {

/**
 * @brief Installs builtin/extension primitive objects into VM globals.
 *
 * Centralizes primitive slot installation for source execution, `.etac`
 * execution, and spawned child runtimes.
 */
class RuntimePrimitiveInstaller {
public:
    RuntimePrimitiveInstaller(runtime::memory::heap::Heap& heap,
                              runtime::BuiltinEnvironment& builtins,
                              runtime::ExtensionEnvironment& extensions) noexcept
        : heap_(heap),
          builtins_(builtins),
          extensions_(extensions) {}

    /// Number of core builtin slots.
    [[nodiscard]] std::size_t builtin_count() const noexcept;

    /// Total primitive slot count (core + extensions).
    [[nodiscard]] std::size_t total_primitive_count() const noexcept;

    /// Whether primitives have been installed into globals since last invalidation.
    [[nodiscard]] bool installed() const noexcept { return installed_; }

    /// Mark primitive installation stale after builtin/extension mutation.
    void invalidate() noexcept { installed_ = false; }

    /**
     * @brief Install primitive objects into reserved primitive slots.
     *
     * @param globals Destination VM globals.
     * @param total_globals Total global slot count required by current module set.
     * @return error if primitive object allocation fails.
     */
    std::expected<void, runtime::error::RuntimeError> install_into(
        std::vector<runtime::nanbox::LispVal>& globals,
        std::size_t total_globals);

    /**
     * @brief Record canonical primitive names into debugger global-name metadata.
     *
     * Primitive names are written to slots `0..N-1` where `N` is
     * `total_primitive_count()`.
     */
    void record_names(std::unordered_map<uint32_t, std::string>& global_names) const;

private:
    runtime::memory::heap::Heap& heap_;
    runtime::BuiltinEnvironment& builtins_;
    runtime::ExtensionEnvironment& extensions_;
    bool installed_{false};
};

} // namespace eta::session

