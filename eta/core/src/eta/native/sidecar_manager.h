/**
 * @file sidecar_manager.h
 * @brief Native sidecar discovery/load orchestration for eta runtimes.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>

#include "eta/native/extension_registry.h"
#include "eta/native/sidecar_loader.h"
#include "eta/runtime/types/primitive.h"

namespace eta::native {

namespace fs = std::filesystem;

/**
 * @brief Coordinates package/bundled native sidecar loading and symbol sync.
 */
class NativeSidecarManager {
public:
    /**
     * @brief Runtime host callbacks used to materialize sidecar symbols.
     */
    class Host {
    public:
        virtual ~Host() = default;

        virtual void register_builtin_primitive(std::string name,
                                                uint32_t arity,
                                                bool has_rest,
                                                runtime::types::PrimitiveFunc func) = 0;
        [[nodiscard]] virtual bool has_builtin_primitive(std::string_view name) const = 0;
        virtual void overwrite_builtin_primitive(std::string_view name,
                                                 runtime::types::PrimitiveFunc func) = 0;
        virtual void register_extension_primitive(std::string name,
                                                  uint32_t arity,
                                                  bool has_rest,
                                                  runtime::types::PrimitiveFunc func) = 0;
        [[nodiscard]] virtual bool can_register_extension_primitives() const noexcept = 0;
        virtual void invalidate_primitive_installer() = 0;
        virtual void emit_sidecar_error(std::string message) = 0;
    };

    explicit NativeSidecarManager(Host& host)
        : host_(host),
          loader_(registry_) {}

    NativeSidecarManager(const NativeSidecarManager&) = delete;
    NativeSidecarManager& operator=(const NativeSidecarManager&) = delete;
    NativeSidecarManager(NativeSidecarManager&&) = delete;
    NativeSidecarManager& operator=(NativeSidecarManager&&) = delete;

    /**
     * @brief Set runtime binding passed to sidecar entrypoints.
     */
    void set_runtime_context(void* runtime_context) noexcept;

    /**
     * @brief Load lockfile-selected native sidecars for one package context.
     *
     * The first discovered package manifest context is kept for the lifetime
     * of this manager instance to keep extension slot assignment deterministic.
     */
    bool ensure_package_sidecars_loaded(std::optional<fs::path> start_dir,
                                        std::span<const fs::path> module_dirs,
                                        std::string_view etai_path);

private:
    static runtime::types::PrimitiveFunc make_sidecar_placeholder_primitive(
        std::string extension_id,
        std::string symbol_name);
    static runtime::types::PrimitiveFunc make_registered_sidecar_primitive(
        std::string extension_id,
        std::string symbol_name,
        void* callable);

    bool ensure_bundled_sidecars_loaded(std::span<const fs::path> module_dirs,
                                        std::string_view etai_path);
    bool ensure_module_path_sidecars_loaded(std::span<const fs::path> module_dirs);
    bool sync_sidecar_extensions_into_environment();

    Host& host_;
    ExtensionRegistry registry_;
    SidecarLoader loader_;
    std::size_t sidecar_registered_extension_count_{0};
    std::optional<std::string> sidecar_manifest_key_;
    bool bundled_sidecars_attempted_{false};
    bool module_path_sidecars_attempted_{false};
    bool module_path_sidecars_loaded_{false};
};

} // namespace eta::native
