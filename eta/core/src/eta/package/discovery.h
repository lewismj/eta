#pragma once

#include <expected>
#include <filesystem>
#include <optional>

#include "eta/package/manifest.h"

namespace eta::package {

namespace fs = std::filesystem;

/**
 * @brief Command context detected from manifest/workspace discovery.
 */
enum class ManifestContextKind {
    StandalonePackage,
    WorkspaceRoot,
    WorkspaceMember,
    WorkspaceNonMember,
};

/**
 * @brief Nearest manifest discovery result for a working directory.
 */
struct ManifestDiscovery {
    fs::path start_dir;
    std::optional<ManifestContextKind> context;
    std::optional<fs::path> active_manifest_path;
    std::optional<fs::path> package_manifest_path;
    std::optional<fs::path> workspace_manifest_path;
};

using ManifestDiscoveryResult = std::expected<ManifestDiscovery, ManifestError>;

/**
 * @brief Walk parent directories to classify package/workspace command context.
 *
 * Discovery starts at @p start_dir and scans `eta.toml` files upward.
 * The nearest workspace manifest terminates the walk, while package manifests
 * discovered below that root are treated as member ownership candidates.
 *
 * `active_manifest_path` preserves package-first command routing:
 * - nearest package manifest, if present,
 * - otherwise nearest workspace manifest.
 */
ManifestDiscoveryResult discover_manifest_context(const fs::path& start_dir);

/**
 * @brief Resolve the nearest manifest path for a directory walk.
 *
 * Prefers workspace manifest, then package manifest, then active manifest
 * reported by @ref discover_manifest_context. Falls back to raw parent walking
 * if manifest parsing fails.
 */
std::optional<fs::path> find_nearest_manifest_path(const fs::path& start_dir);

} // namespace eta::package
