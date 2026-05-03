#include "eta/package/resolver.h"

#include <algorithm>
#include <cctype>
#include <functional>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace eta::package {

namespace {

[[nodiscard]] fs::path canonicalize_path(const fs::path& path) {
    std::error_code ec;
    const auto canonical = fs::weakly_canonical(path, ec);
    if (!ec) return canonical;
    return path.lexically_normal();
}

[[nodiscard]] std::string canonical_key(const fs::path& path) {
    auto normalized = canonicalize_path(path).generic_string();
#if defined(_WIN32)
    std::transform(normalized.begin(),
                   normalized.end(),
                   normalized.begin(),
                   [](const char c) {
                       return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                   });
#endif
    return normalized;
}

[[nodiscard]] std::expected<fs::path, ResolveError>
normalize_manifest_path(const fs::path& manifest_path_or_dir) {
    auto candidate = manifest_path_or_dir;
    std::error_code ec;
    if (fs::is_directory(candidate, ec) && !ec) {
        candidate /= "eta.toml";
    }
    if (!fs::exists(candidate, ec) || ec || !fs::is_regular_file(candidate, ec) || ec) {
        return std::unexpected(ResolveError{
            ResolveError::Code::MissingDependencyManifest,
            "manifest not found: " + candidate.string(),
        });
    }
    return canonicalize_path(candidate);
}

[[nodiscard]] std::string dependency_source_id(const ManifestDependency& dependency) {
    switch (dependency.kind) {
        case ManifestDependencyKind::Path:
            return {};
        case ManifestDependencyKind::Git:
            return "git+" + dependency.git + "#" + dependency.rev;
        case ManifestDependencyKind::Tarball:
            return "tarball+" + dependency.tarball + "#sha256=" + dependency.sha256;
    }
    return {};
}

[[nodiscard]] std::expected<ResolvedDependencyLocation, ResolveError>
resolve_path_dependency(const Manifest& owner, const ManifestDependency& dependency) {
    fs::path candidate = owner.manifest_path.parent_path() / dependency.path;
    std::error_code ec;
    if (fs::is_directory(candidate, ec) && !ec) {
        candidate /= "eta.toml";
    }
    if (!fs::exists(candidate, ec) || ec || !fs::is_regular_file(candidate, ec) || ec) {
        return std::unexpected(ResolveError{
            ResolveError::Code::MissingDependencyManifest,
            "dependency '" + dependency.name + "' manifest not found at "
                + candidate.string(),
        });
    }

    ResolvedDependencyLocation location;
    location.manifest_path = canonicalize_path(candidate);
    location.source = "path+" + canonicalize_path(location.manifest_path.parent_path()).generic_string();
    return location;
}

[[nodiscard]] std::expected<ResolvedDependencyLocation, ResolveError>
resolve_non_path_from_lockfile(const Manifest& owner,
                               const ManifestDependency& dependency,
                               const ResolveOptions& options) {
    if (options.lockfile == nullptr) {
        return std::unexpected(ResolveError{
            ResolveError::Code::UnsupportedDependencySource,
            "dependency '" + dependency.name
                + "' uses a non-path source and requires lockfile-backed materialization",
        });
    }

    const auto source_id = dependency_source_id(dependency);
    const LockfilePackage* locked = nullptr;
    for (const auto& package : options.lockfile->packages) {
        if (package.name != dependency.name || package.source == "root") continue;
        if (!source_id.empty() && package.source != source_id) continue;
        locked = &package;
        break;
    }
    if (locked == nullptr) {
        return std::unexpected(ResolveError{
            ResolveError::Code::MissingDependencyManifest,
            "dependency '" + dependency.name
                + "' is not present in eta.lock; run `eta update` or `eta vendor`",
        });
    }

    fs::path modules_root = options.modules_root;
    if (modules_root.empty()) {
        modules_root = owner.manifest_path.parent_path() / ".eta" / "modules";
    }
    const auto package_dir = modules_root / (locked->name + "-" + locked->version);
    const auto manifest_path = package_dir / "eta.toml";

    std::error_code ec;
    if (!fs::exists(manifest_path, ec) || ec || !fs::is_regular_file(manifest_path, ec) || ec) {
        return std::unexpected(ResolveError{
            ResolveError::Code::MissingDependencyManifest,
            "materialized dependency missing for '" + dependency.name + "': "
                + manifest_path.string(),
        });
    }

    ResolvedDependencyLocation location;
    location.manifest_path = canonicalize_path(manifest_path);
    location.source = locked->source;
    return location;
}

[[nodiscard]] std::expected<ResolvedDependencyLocation, ResolveError>
resolve_dependency_location(const Manifest& owner,
                            const ManifestDependency& dependency,
                            const ResolveOptions& options) {
    if (dependency.kind == ManifestDependencyKind::Path) {
        return resolve_path_dependency(owner, dependency);
    }

    if (options.dependency_locator) {
        auto located = options.dependency_locator(owner, dependency);
        if (!located) return std::unexpected(located.error());
        located->manifest_path = canonicalize_path(located->manifest_path);
        if (located->source.empty()) {
            located->source = dependency_source_id(dependency);
        }
        return located;
    }

    return resolve_non_path_from_lockfile(owner, dependency, options);
}

} // namespace

const ResolvedPackage* ResolvedGraph::find(std::string_view name) const {
    for (const auto& pkg : packages) {
        if (pkg.name == name) return &pkg;
    }
    return nullptr;
}

ResolveResult resolve_dependencies(const fs::path& root_manifest_path,
                                   const ResolveOptions& options) {
    auto root_path_res = normalize_manifest_path(root_manifest_path);
    if (!root_path_res) return std::unexpected(root_path_res.error());
    const fs::path root_path = *root_path_res;

    ResolvedGraph graph;
    std::unordered_map<std::string, std::size_t> index_by_name;
    std::unordered_map<std::string, std::string> name_by_manifest;
    std::vector<std::string> stack;
    std::unordered_set<std::string> visiting;

    std::function<std::expected<void, ResolveError>(
        const fs::path&,
        std::optional<std::string_view>,
        bool,
        std::optional<std::string>)> visit;

    visit = [&](const fs::path& manifest_path,
                std::optional<std::string_view> expected_name,
                bool is_root,
                std::optional<std::string> source_override) -> std::expected<void, ResolveError> {
        auto manifest_res = read_manifest(manifest_path);
        if (!manifest_res) {
            return std::unexpected(ResolveError{
                ResolveError::Code::ManifestReadError,
                "failed to read manifest '" + manifest_path.string()
                    + "': " + manifest_res.error().message,
            });
        }
        Manifest manifest = std::move(*manifest_res);
        manifest.manifest_path = manifest_path;

        if (expected_name.has_value() && manifest.name != *expected_name) {
            return std::unexpected(ResolveError{
                ResolveError::Code::DependencyNameMismatch,
                "dependency key '" + std::string(*expected_name)
                    + "' does not match package.name '" + manifest.name
                    + "' in " + manifest_path.string(),
            });
        }

        const auto manifest_key = canonical_key(manifest_path);
        if (auto existing = name_by_manifest.find(manifest_key);
            existing != name_by_manifest.end()) {
            return {};
        }

        if (visiting.contains(manifest.name)) {
            std::vector<std::string> cycle;
            auto start_it = std::find(stack.begin(), stack.end(), manifest.name);
            if (start_it != stack.end()) {
                cycle.insert(cycle.end(), start_it, stack.end());
            }
            cycle.push_back(manifest.name);
            return std::unexpected(ResolveError{
                ResolveError::Code::CycleDetected,
                "dependency cycle detected at package '" + manifest.name + "'",
                std::move(cycle),
            });
        }

        if (auto existing = index_by_name.find(manifest.name); existing != index_by_name.end()) {
            const auto& prior = graph.packages[existing->second];
            if (canonical_key(prior.manifest_path) != manifest_key) {
                return std::unexpected(ResolveError{
                    ResolveError::Code::DuplicatePackageName,
                    "package name '" + manifest.name + "' is provided by multiple manifests: "
                        + prior.manifest_path.string() + " and " + manifest_path.string(),
                });
            }
            return {};
        }

        visiting.insert(manifest.name);
        stack.push_back(manifest.name);

        ResolvedPackage node;
        node.name = manifest.name;
        node.version = manifest.version;
        node.manifest_path = manifest_path;
        node.package_root = manifest_path.parent_path();
        node.source = is_root ? "root"
                              : source_override.value_or(
                                    "path+" + canonicalize_path(node.package_root).generic_string());
        const bool include_dev = options.include_dev_dependencies && is_root;
        node.dependency_names.reserve(
            manifest.dependencies.size() + (include_dev ? manifest.dev_dependencies.size() : 0u));

        const std::size_t node_index = graph.packages.size();
        graph.packages.push_back(std::move(node));
        index_by_name.emplace(manifest.name, node_index);
        name_by_manifest.emplace(manifest_key, manifest.name);

        std::vector<ManifestDependency> dependencies = manifest.dependencies;
        if (include_dev) {
            dependencies.insert(
                dependencies.end(), manifest.dev_dependencies.begin(), manifest.dev_dependencies.end());
        }
        std::sort(dependencies.begin(),
                  dependencies.end(),
                  [](const ManifestDependency& lhs, const ManifestDependency& rhs) {
                      return lhs.name < rhs.name;
                  });

        for (const auto& dependency : dependencies) {
            graph.packages[node_index].dependency_names.push_back(dependency.name);

            auto dep_location = resolve_dependency_location(manifest, dependency, options);
            if (!dep_location) {
                stack.pop_back();
                visiting.erase(manifest.name);
                return std::unexpected(dep_location.error());
            }

            auto dep_result = visit(
                dep_location->manifest_path, dependency.name, false, dep_location->source);
            if (!dep_result) {
                stack.pop_back();
                visiting.erase(manifest.name);
                return std::unexpected(dep_result.error());
            }
        }

        std::sort(graph.packages[node_index].dependency_names.begin(),
                  graph.packages[node_index].dependency_names.end());
        graph.packages[node_index].dependency_names.erase(
            std::unique(graph.packages[node_index].dependency_names.begin(),
                        graph.packages[node_index].dependency_names.end()),
            graph.packages[node_index].dependency_names.end());

        stack.pop_back();
        visiting.erase(manifest.name);
        return {};
    };

    auto root_result = visit(root_path, std::nullopt, true, std::nullopt);
    if (!root_result) return std::unexpected(root_result.error());

    if (graph.packages.empty()) {
        return std::unexpected(ResolveError{
            ResolveError::Code::ManifestReadError,
            "resolver produced an empty graph",
        });
    }

    graph.root_name = graph.packages.front().name;
    std::stable_sort(graph.packages.begin(),
                     graph.packages.end(),
                     [&](const ResolvedPackage& lhs, const ResolvedPackage& rhs) {
                         const bool lhs_root = lhs.name == graph.root_name;
                         const bool rhs_root = rhs.name == graph.root_name;
                         if (lhs_root != rhs_root) return lhs_root;
                         if (lhs.name != rhs.name) return lhs.name < rhs.name;
                         return lhs.version < rhs.version;
                     });
    return graph;
}

ResolveResult resolve_path_dependencies(const fs::path& root_manifest_path) {
    ResolveOptions options;
    return resolve_dependencies(root_manifest_path, options);
}

Lockfile build_lockfile(const ResolvedGraph& graph) {
    Lockfile lockfile;
    lockfile.schema_version = 1;

    std::unordered_map<std::string, std::string> versions_by_name;
    for (const auto& pkg : graph.packages) {
        versions_by_name.insert_or_assign(pkg.name, pkg.version);
    }

    lockfile.packages.reserve(graph.packages.size());
    for (const auto& pkg : graph.packages) {
        LockfilePackage entry;
        entry.name = pkg.name;
        entry.version = pkg.version;
        entry.source = pkg.source;
        entry.dependencies.reserve(pkg.dependency_names.size());
        for (const auto& dep_name : pkg.dependency_names) {
            if (auto it = versions_by_name.find(dep_name); it != versions_by_name.end()) {
                entry.dependencies.push_back(LockedDependency{dep_name, it->second});
            }
        }
        std::sort(entry.dependencies.begin(),
                  entry.dependencies.end(),
                  [](const LockedDependency& lhs, const LockedDependency& rhs) {
                      if (lhs.name != rhs.name) return lhs.name < rhs.name;
                      return lhs.version < rhs.version;
                  });
        lockfile.packages.push_back(std::move(entry));
    }
    return lockfile;
}

} // namespace eta::package
