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

[[nodiscard]] std::string host_target_triple() {
#if defined(_WIN32)
#if defined(_M_X64) || defined(__x86_64__)
    return "x86_64-pc-windows-msvc";
#elif defined(_M_ARM64) || defined(__aarch64__)
    return "aarch64-pc-windows-msvc";
#elif defined(_M_IX86) || defined(__i386__)
    return "i686-pc-windows-msvc";
#else
    return "unknown-pc-windows-msvc";
#endif
#elif defined(__APPLE__)
#if defined(__aarch64__)
    return "aarch64-apple-darwin";
#elif defined(__x86_64__)
    return "x86_64-apple-darwin";
#else
    return "unknown-apple-darwin";
#endif
#elif defined(__linux__)
#if defined(__x86_64__)
    return "x86_64-unknown-linux-gnu";
#elif defined(__aarch64__)
    return "aarch64-unknown-linux-gnu";
#else
    return "unknown-unknown-linux-gnu";
#endif
#else
    return "unknown-unknown-unknown";
#endif
}

[[nodiscard]] std::string selected_target_triple(const ResolveOptions& options) {
    if (!options.target_triple.empty()) return options.target_triple;
    return host_target_triple();
}

[[nodiscard]] std::expected<ResolvedNativePackage, ResolveError>
select_native_target(const Manifest& manifest, const ResolveOptions& options) {
    if (!manifest.native.has_value()) {
        return std::unexpected(ResolveError{
            ResolveError::Code::MissingNativeTargetForTriple,
            "internal error: native target selection requested for non-native package '"
                + manifest.name + "'",
        });
    }

    const auto target_triple = selected_target_triple(options);
    const auto target_it = std::find_if(manifest.native->targets.begin(),
                                        manifest.native->targets.end(),
                                        [&](const ManifestNativeTarget& target) {
                                            return target.triple == target_triple;
                                        });
    if (target_it == manifest.native->targets.end()) {
        return std::unexpected(ResolveError{
            ResolveError::Code::MissingNativeTargetForTriple,
            "native package '" + manifest.name
                + "' does not declare a sidecar target for triple '" + target_triple
                + "' in " + manifest.manifest_path.string(),
        });
    }

    ResolvedNativePackage selected;
    selected.id = manifest.native->id;
    selected.abi = manifest.native->abi;
    selected.entry = manifest.native->entry;
    selected.target_triple = target_it->triple;
    selected.artifact_relpath = target_it->artifact;
    selected.sha256 = target_it->sha256;
    return selected;
}

[[nodiscard]] bool has_matching_native_metadata(const ResolvedPackage& lhs,
                                                const ResolvedPackage& rhs) {
    if (lhs.native.has_value() != rhs.native.has_value()) return false;
    if (!lhs.native.has_value()) return true;

    auto normalize_relpath = [](const fs::path& relpath) {
        auto normalized = relpath.lexically_normal().generic_string();
#if defined(_WIN32)
        std::transform(normalized.begin(),
                       normalized.end(),
                       normalized.begin(),
                       [](const char c) {
                           return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                       });
#endif
        return normalized;
    };

    return lhs.native->id == rhs.native->id
        && lhs.native->abi == rhs.native->abi
        && lhs.native->entry == rhs.native->entry
        && lhs.native->target_triple == rhs.native->target_triple
        && normalize_relpath(lhs.native->artifact_relpath)
            == normalize_relpath(rhs.native->artifact_relpath)
        && lhs.native->sha256 == rhs.native->sha256;
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

[[nodiscard]] bool has_wildcard(std::string_view text) {
    return text.find('*') != std::string_view::npos
        || text.find('?') != std::string_view::npos;
}

[[nodiscard]] bool glob_match_segment(std::string_view pattern, std::string_view value) {
    std::size_t p = 0;
    std::size_t v = 0;
    std::size_t star = std::string_view::npos;
    std::size_t star_match = 0;

    while (v < value.size()) {
        if (p < pattern.size()
            && (pattern[p] == '?' || pattern[p] == value[v])) {
            ++p;
            ++v;
            continue;
        }
        if (p < pattern.size() && pattern[p] == '*') {
            star = p++;
            star_match = v;
            continue;
        }
        if (star != std::string_view::npos) {
            p = star + 1u;
            v = ++star_match;
            continue;
        }
        return false;
    }

    while (p < pattern.size() && pattern[p] == '*') ++p;
    return p == pattern.size();
}

[[nodiscard]] std::vector<std::string> split_pattern_segments(std::string_view pattern) {
    std::vector<std::string> segments;
    std::string current;
    current.reserve(pattern.size());
    for (const char c : pattern) {
        if (c == '/' || c == '\\') {
            if (!current.empty()) {
                segments.push_back(current);
                current.clear();
            }
            continue;
        }
        current.push_back(c);
    }
    if (!current.empty()) segments.push_back(current);
    return segments;
}

[[nodiscard]] bool is_within_root(const fs::path& root, const fs::path& candidate) {
    const auto root_key = canonical_key(root);
    const auto candidate_key = canonical_key(candidate);
    if (candidate_key == root_key) return true;
    if (candidate_key.size() <= root_key.size()) return false;
    if (candidate_key.compare(0u, root_key.size(), root_key) != 0) return false;
    if (!root_key.empty() && root_key.back() == '/') return true;
    return candidate_key[root_key.size()] == '/';
}

[[nodiscard]] std::string workspace_member_source(const fs::path& workspace_root,
                                                  const fs::path& member_root) {
    std::error_code ec;
    auto relative = fs::relative(member_root, workspace_root, ec);
    if (ec || relative.empty() || relative == ".") {
        return "workspace+.";
    }
    const auto relative_text = relative.generic_string();
    if (relative_text.empty()) return "workspace+.";
    return "workspace+" + relative_text;
}

void sort_unique_paths(std::vector<fs::path>& paths) {
    std::sort(paths.begin(),
              paths.end(),
              [](const fs::path& lhs, const fs::path& rhs) {
                  return canonical_key(lhs) < canonical_key(rhs);
              });
    paths.erase(std::unique(paths.begin(),
                            paths.end(),
                            [](const fs::path& lhs, const fs::path& rhs) {
                                return canonical_key(lhs) == canonical_key(rhs);
                            }),
                paths.end());
}

[[nodiscard]] std::expected<std::vector<fs::path>, ResolveError>
expand_workspace_pattern(const fs::path& workspace_manifest_path,
                         const fs::path& workspace_root,
                         std::string_view pattern) {
    auto pattern_path = fs::path(std::string(pattern));
    if (pattern_path.is_absolute()) {
        return std::unexpected(ResolveError{
            ResolveError::Code::InvalidWorkspaceMember,
            "workspace member pattern must be relative: " + std::string(pattern),
        });
    }

    std::vector<fs::path> candidates{workspace_root};
    auto segments = split_pattern_segments(pattern);
    if (segments.empty()) {
        if (pattern == "." || pattern == "./") return candidates;
        return std::unexpected(ResolveError{
            ResolveError::Code::InvalidWorkspaceMember,
            "workspace member pattern is empty",
        });
    }

    for (const auto& raw_segment : segments) {
        std::string segment = raw_segment;
#if defined(_WIN32)
        std::transform(segment.begin(),
                       segment.end(),
                       segment.begin(),
                       [](const char c) {
                           return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                       });
#endif

        if (segment == ".") continue;
        if (segment == "..") {
            std::vector<fs::path> next;
            next.reserve(candidates.size());
            for (const auto& base : candidates) {
                const auto parent = canonicalize_path(base.parent_path());
                if (!is_within_root(workspace_root, parent)) {
                    return std::unexpected(ResolveError{
                        ResolveError::Code::InvalidWorkspaceMember,
                        "workspace member pattern escapes workspace root: " + std::string(pattern),
                    });
                }
                next.push_back(parent);
            }
            sort_unique_paths(next);
            candidates = std::move(next);
            continue;
        }

        const bool wildcard = has_wildcard(segment);
        std::vector<fs::path> next;
        for (const auto& base : candidates) {
            std::error_code ec;
            if (!fs::is_directory(base, ec) || ec) continue;

            if (!wildcard) {
                const auto child = canonicalize_path(base / segment);
                std::error_code child_ec;
                if (fs::is_directory(child, child_ec) && !child_ec) {
                    next.push_back(child);
                }
                continue;
            }

            for (const auto& entry : fs::directory_iterator(base, ec)) {
                if (ec) break;
                std::error_code entry_ec;
                if (!entry.is_directory(entry_ec) || entry_ec) continue;

                auto file_name = entry.path().filename().string();
#if defined(_WIN32)
                std::transform(file_name.begin(),
                               file_name.end(),
                               file_name.begin(),
                               [](const char c) {
                                   return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                               });
#endif
                if (!glob_match_segment(segment, file_name)) continue;
                next.push_back(canonicalize_path(entry.path()));
            }
        }

        sort_unique_paths(next);
        candidates = std::move(next);
        if (candidates.empty()) break;
    }

    for (const auto& candidate : candidates) {
        if (!is_within_root(workspace_root, candidate)) {
            return std::unexpected(ResolveError{
                ResolveError::Code::InvalidWorkspaceMember,
                "workspace member '" + candidate.string()
                    + "' is outside workspace root '" + workspace_manifest_path.parent_path().string() + "'",
            });
        }
    }

    return candidates;
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
        node.source = is_root ? (options.root_source.empty() ? "root" : options.root_source)
                              : source_override.value_or(
                                    "path+" + canonicalize_path(node.package_root).generic_string());
        if (manifest.native.has_value()) {
            auto selected_native = select_native_target(manifest, options);
            if (!selected_native) {
                stack.pop_back();
                visiting.erase(manifest.name);
                return std::unexpected(selected_native.error());
            }
            node.native = std::move(*selected_native);
        }
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

WorkspaceMembersResult resolve_workspace_members(const fs::path& workspace_manifest_path) {
    auto workspace_manifest_res = normalize_manifest_path(workspace_manifest_path);
    if (!workspace_manifest_res) return std::unexpected(workspace_manifest_res.error());

    const auto workspace_manifest = *workspace_manifest_res;
    auto document = read_manifest_document(workspace_manifest);
    if (!document) {
        return std::unexpected(ResolveError{
            ResolveError::Code::ManifestReadError,
            "failed to read workspace manifest '" + workspace_manifest.string()
                + "': " + document.error().message,
        });
    }
    if (!document->workspace.has_value()) {
        return std::unexpected(ResolveError{
            ResolveError::Code::InvalidWorkspaceMember,
            "manifest does not declare [workspace]: " + workspace_manifest.string(),
        });
    }

    WorkspaceMembers workspace;
    workspace.workspace_manifest_path = workspace_manifest;
    workspace.workspace_root = canonicalize_path(workspace_manifest.parent_path());

    std::vector<fs::path> member_roots;
    for (const auto& pattern : document->workspace->members) {
        auto expanded = expand_workspace_pattern(workspace_manifest, workspace.workspace_root, pattern);
        if (!expanded) return std::unexpected(expanded.error());
        member_roots.insert(member_roots.end(), expanded->begin(), expanded->end());
    }
    sort_unique_paths(member_roots);

    std::unordered_set<std::string> excluded_roots;
    for (const auto& pattern : document->workspace->exclude) {
        auto expanded = expand_workspace_pattern(workspace_manifest, workspace.workspace_root, pattern);
        if (!expanded) return std::unexpected(expanded.error());
        for (const auto& candidate : *expanded) {
            excluded_roots.insert(canonical_key(candidate));
        }
    }

    if (document->package.has_value()
        && !excluded_roots.contains(canonical_key(workspace.workspace_root))) {
        member_roots.push_back(workspace.workspace_root);
    }
    sort_unique_paths(member_roots);

    std::vector<fs::path> selected_roots;
    selected_roots.reserve(member_roots.size());
    for (const auto& member_root : member_roots) {
        if (excluded_roots.contains(canonical_key(member_root))) continue;
        selected_roots.push_back(member_root);
    }
    sort_unique_paths(selected_roots);

    if (selected_roots.empty()) {
        return std::unexpected(ResolveError{
            ResolveError::Code::InvalidWorkspaceMember,
            "workspace does not resolve to any selected members: " + workspace_manifest.string(),
        });
    }

    std::unordered_map<std::string, fs::path> manifest_by_name;
    for (const auto& member_root : selected_roots) {
        const auto manifest_candidate = canonicalize_path(member_root / "eta.toml");
        std::error_code ec;
        if (!fs::is_regular_file(manifest_candidate, ec) || ec) {
            return std::unexpected(ResolveError{
                ResolveError::Code::MissingDependencyManifest,
                "workspace member manifest not found: " + manifest_candidate.string(),
            });
        }

        auto manifest = read_manifest(manifest_candidate);
        if (!manifest) {
            return std::unexpected(ResolveError{
                ResolveError::Code::ManifestReadError,
                "failed to read workspace member manifest '" + manifest_candidate.string()
                    + "': " + manifest.error().message,
            });
        }

        if (auto existing = manifest_by_name.find(manifest->name);
            existing != manifest_by_name.end()) {
            if (canonical_key(existing->second) != canonical_key(manifest_candidate)) {
                return std::unexpected(ResolveError{
                    ResolveError::Code::DuplicatePackageName,
                    "workspace members contain duplicate package.name '" + manifest->name
                        + "': " + existing->second.string()
                        + " and " + manifest_candidate.string(),
                });
            }
        } else {
            manifest_by_name.emplace(manifest->name, manifest_candidate);
        }

        WorkspaceMember member;
        member.name = manifest->name;
        member.manifest_path = manifest_candidate;
        member.package_root = canonicalize_path(manifest_candidate.parent_path());
        member.source = workspace_member_source(workspace.workspace_root, member.package_root);
        workspace.members.push_back(std::move(member));
    }

    std::sort(workspace.members.begin(),
              workspace.members.end(),
              [](const WorkspaceMember& lhs, const WorkspaceMember& rhs) {
                  if (lhs.source != rhs.source) return lhs.source < rhs.source;
                  if (lhs.name != rhs.name) return lhs.name < rhs.name;
                  return canonical_key(lhs.manifest_path) < canonical_key(rhs.manifest_path);
              });

    return workspace;
}

ResolveResult resolve_workspace_dependencies(const WorkspaceMembers& workspace,
                                             const ResolveOptions& options) {
    if (workspace.members.empty()) {
        return std::unexpected(ResolveError{
            ResolveError::Code::InvalidWorkspaceMember,
            "workspace has no members to resolve",
        });
    }

    ResolvedGraph merged;
    merged.root_name = workspace.members.front().name;

    std::unordered_map<std::string, std::size_t> index_by_name;
    std::unordered_map<std::string, std::string> member_source_by_name;
    member_source_by_name.reserve(workspace.members.size());
    for (const auto& member : workspace.members) {
        member_source_by_name.emplace(member.name, member.source);

        ResolveOptions member_options = options;
        member_options.root_source = member.source;
        auto graph = resolve_dependencies(member.manifest_path, member_options);
        if (!graph) return std::unexpected(graph.error());

        for (auto& pkg : graph->packages) {
            auto it = index_by_name.find(pkg.name);
            if (it == index_by_name.end()) {
                const std::size_t index = merged.packages.size();
                index_by_name.emplace(pkg.name, index);
                merged.packages.push_back(std::move(pkg));
                continue;
            }

            auto& existing = merged.packages[it->second];
            if (canonical_key(existing.manifest_path) != canonical_key(pkg.manifest_path)) {
                return std::unexpected(ResolveError{
                    ResolveError::Code::DuplicatePackageName,
                    "package name '" + pkg.name + "' is provided by multiple manifests: "
                        + existing.manifest_path.string() + " and " + pkg.manifest_path.string(),
                });
            }
            if (!has_matching_native_metadata(existing, pkg)) {
                return std::unexpected(ResolveError{
                    ResolveError::Code::DuplicatePackageName,
                    "package '" + pkg.name
                        + "' has inconsistent native metadata across resolver graphs",
                });
            }

            existing.dependency_names.insert(existing.dependency_names.end(),
                                             pkg.dependency_names.begin(),
                                             pkg.dependency_names.end());
            if (existing.source.rfind("workspace+", 0) != 0
                && pkg.source.rfind("workspace+", 0) == 0) {
                existing.source = pkg.source;
            }
        }
    }

    for (auto& pkg : merged.packages) {
        if (auto it = member_source_by_name.find(pkg.name); it != member_source_by_name.end()) {
            pkg.source = it->second;
        }
        std::sort(pkg.dependency_names.begin(), pkg.dependency_names.end());
        pkg.dependency_names.erase(std::unique(pkg.dependency_names.begin(),
                                               pkg.dependency_names.end()),
                                   pkg.dependency_names.end());
    }

    std::stable_sort(merged.packages.begin(),
                     merged.packages.end(),
                     [](const ResolvedPackage& lhs, const ResolvedPackage& rhs) {
                         if (lhs.name != rhs.name) return lhs.name < rhs.name;
                         if (lhs.version != rhs.version) return lhs.version < rhs.version;
                         return lhs.source < rhs.source;
                     });

    return merged;
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
        if (pkg.native.has_value()) {
            entry.native_id = pkg.native->id;
            entry.native_abi = pkg.native->abi;
            entry.native_entry = pkg.native->entry;
            entry.native_target_triple = pkg.native->target_triple;
            entry.native_artifact_relpath = pkg.native->artifact_relpath.generic_string();
            entry.native_sha256 = pkg.native->sha256;
        }
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
