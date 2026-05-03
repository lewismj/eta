#pragma once

#include <expected>
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "eta/package/lockfile.h"
#include "eta/package/manifest.h"

namespace eta::package {

namespace fs = std::filesystem;

/**
 * @brief One package node in a resolved path-dependency graph.
 */
struct ResolvedPackage {
    std::string name;
    std::string version;
    fs::path manifest_path;
    fs::path package_root;
    std::string source;
    std::vector<std::string> dependency_names;
};

/**
 * @brief Resolved dependency graph rooted at the requested manifest.
 */
struct ResolvedGraph {
    std::string root_name;
    std::vector<ResolvedPackage> packages;

    [[nodiscard]] const ResolvedPackage* find(std::string_view name) const;
};

/**
 * @brief Graph resolver error.
 */
struct ResolveError {
    enum class Code {
        ManifestReadError,
        MissingDependencyManifest,
        DependencyNameMismatch,
        DuplicatePackageName,
        CycleDetected,
        UnsupportedDependencySource,
    };

    Code code{Code::ManifestReadError};
    std::string message;
    std::vector<std::string> cycle;
};

using ResolveResult = std::expected<ResolvedGraph, ResolveError>;

/**
 * @brief Resolved on-disk location for one dependency edge.
 */
struct ResolvedDependencyLocation {
    fs::path manifest_path;
    std::string source;
};

/**
 * @brief Dependency locator callback used for non-path sources.
 */
using DependencyLocator =
    std::function<std::expected<ResolvedDependencyLocation, ResolveError>(
        const Manifest& owner,
        const ManifestDependency& dependency)>;

/**
 * @brief Options controlling graph resolution behavior.
 */
struct ResolveOptions {
    bool include_dev_dependencies{false};
    const Lockfile* lockfile{nullptr};
    fs::path modules_root;
    DependencyLocator dependency_locator;
};

/**
 * @brief Resolve a root `eta.toml` dependency graph.
 *
 * Path dependencies are resolved directly from manifest-relative paths.
 * Non-path dependencies can be provided by `dependency_locator`, or by
 * lockfile-backed lookup in `modules_root`.
 */
ResolveResult resolve_dependencies(const fs::path& root_manifest_path,
                                   const ResolveOptions& options = {});

/**
 * @brief Resolve a root `eta.toml` plus transitive local `path` dependencies.
 */
ResolveResult resolve_path_dependencies(const fs::path& root_manifest_path);

/**
 * @brief Materialize deterministic lockfile rows from a resolved graph.
 */
Lockfile build_lockfile(const ResolvedGraph& graph);

} // namespace eta::package
