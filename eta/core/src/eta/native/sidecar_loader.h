#pragma once

#include <expected>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

#include "eta/native/extension_registry.h"
#include "eta/package/discovery.h"

namespace eta::native {

namespace fs = std::filesystem;

/**
 * @brief Sidecar loader and native context error.
 */
struct SidecarLoaderError {
    enum class Code {
        ManifestDiscoveryFailed,
        MissingPackageContext,
        WorkspaceResolutionFailed,
        DependencyResolutionFailed,
        LockfileReadFailed,
        UnknownPackageRoot,
        ArtifactPathNotRelative,
        ArtifactPathEscapesPackageRoot,
        ChecksumMismatch,
        LibraryOpenFailed,
        SymbolLookupFailed,
        ExtensionRegistrationFailed,
        AbiMismatch,
        InvalidExtensionMetadata,
        RegistryConflict,
    };

    Code code{Code::ManifestDiscoveryFailed};
    std::string message;
};

/**
 * @brief Sidecar-aware package/workspace context resolved from a start directory.
 */
struct NativeLoadContext {
    package::ManifestContextKind context_kind{
        package::ManifestContextKind::StandalonePackage};
    fs::path active_manifest_path;
    std::optional<fs::path> workspace_manifest_path;
    fs::path lockfile_root;
    fs::path modules_root;
    std::unordered_map<std::string, fs::path> package_root_by_name;
};

using NativeLoadContextResult = std::expected<NativeLoadContext, SidecarLoaderError>;

/**
 * @brief One native sidecar entry resolved from package metadata.
 */
struct NativeSidecarSpec {
    std::string package_name;
    fs::path artifact_relpath;
    std::string abi{"eta-native-v1"};
    std::string entrypoint{"eta_register_extension_v1"};
    std::optional<std::string> expected_extension_id;
    std::optional<std::string> expected_sha256;
};

/**
 * @brief One sidecar artifact mapped to absolute package-relative paths.
 */
struct ResolvedNativeSidecar {
    NativeSidecarSpec spec;
    fs::path package_root;
    fs::path artifact_path;
};

using NativeSidecarResolutionResult =
    std::expected<std::vector<ResolvedNativeSidecar>, SidecarLoaderError>;

/**
 * @brief Build a sidecar load context using package/workspace discovery rules.
 */
NativeLoadContextResult build_native_load_context(const fs::path& start_dir);

/**
 * @brief Resolve package-relative sidecar artifacts into absolute paths.
 *
 * Output order matches input order.
 */
NativeSidecarResolutionResult resolve_native_sidecars(
    const NativeLoadContext& context,
    std::span<const NativeSidecarSpec> sidecars);

/**
 * @brief Compute lowercase SHA-256 for one sidecar artifact file.
 */
std::expected<std::string, SidecarLoaderError> compute_sidecar_sha256(
    const fs::path& file_path);

/**
 * @brief Return true if @p candidate is equal to or under @p root.
 */
[[nodiscard]] bool is_path_within(const fs::path& root, const fs::path& candidate);

/**
 * @brief Dynamic sidecar loader that keeps opened library handles alive.
 */
class SidecarLoader {
public:
    explicit SidecarLoader(ExtensionRegistry& registry);
    ~SidecarLoader();

    SidecarLoader(const SidecarLoader&) = delete;
    SidecarLoader& operator=(const SidecarLoader&) = delete;
    SidecarLoader(SidecarLoader&&) = delete;
    SidecarLoader& operator=(SidecarLoader&&) = delete;

    /**
     * @brief Load one sidecar and register its extension symbols.
     */
    std::expected<void, SidecarLoaderError> load(const ResolvedNativeSidecar& sidecar);

    /**
     * @brief Return number of libraries currently retained by the loader.
     */
    [[nodiscard]] std::size_t loaded_library_count() const noexcept;

    /**
     * @brief Set optional runtime binding context passed into sidecar entrypoints.
     */
    void set_runtime_context(void* runtime_context) noexcept;

private:
    struct LoadedLibrary;

    ExtensionRegistry& registry_;
    void* runtime_context_{nullptr};
    std::vector<std::unique_ptr<LoadedLibrary>> loaded_libraries_;
};

} // namespace eta::native
