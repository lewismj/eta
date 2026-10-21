/// Dependency fetching, cache materialization and project state resolution.

#include <algorithm>
#include <cctype>
#include <chrono>
#include <sstream>
#include <system_error>

#include "eta/cli/cli_internal.h"

namespace eta::cli {

namespace {

[[nodiscard]] std::string dependency_source_id(const eta::package::ManifestDependency& dependency) {
    switch (dependency.kind) {
        case eta::package::ManifestDependencyKind::Path:
            return {};
        case eta::package::ManifestDependencyKind::Git:
            return "git+" + dependency.git + "#" + dependency.rev;
        case eta::package::ManifestDependencyKind::Tarball:
            return "tarball+" + dependency.tarball + "#sha256=" + dependency.sha256;
    }
    return {};
}

[[nodiscard]] std::string cache_key_from_source(std::string_view source) {
    return to_hex_u64(fnv1a_64(source));
}

CliResult<std::string> compute_sha256_file(const fs::path& file_path) {
#ifdef _WIN32
    auto result = run_external_capture("certutil",
                                       {"-hashfile", file_path.string(), "SHA256"});
    if (!result) return std::unexpected(result.error());
    if (result->exit_code != 0) {
        return std::unexpected("certutil failed while hashing " + file_path.string());
    }

    std::istringstream in(result->output);
    std::string line;
    while (std::getline(in, line)) {
        std::string hex_line;
        bool invalid_line = false;
        for (const char c : line) {
            const auto uc = static_cast<unsigned char>(c);
            if (std::isxdigit(uc)) {
                hex_line.push_back(static_cast<char>(std::tolower(uc)));
                continue;
            }
            if (!std::isspace(uc)) {
                invalid_line = true;
                break;
            }
        }
        if (!invalid_line && hex_line.size() == 64u) {
            return hex_line;
        }
    }
    return std::unexpected("failed to parse SHA256 digest from certutil output");
#else
    auto parse_sha_output = [](std::string_view text) -> std::optional<std::string> {
        std::istringstream in{std::string(text)};
        std::string token;
        in >> token;
        if (!looks_like_hex(token, 64u)) return std::nullopt;
        std::transform(token.begin(), token.end(), token.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });
        return token;
    };

    if (auto result = run_external_capture("sha256sum", {file_path.string()}); result) {
        if (result->exit_code == 0) {
            if (auto parsed = parse_sha_output(result->output)) return *parsed;
        }
    }
    if (auto result = run_external_capture("shasum", {"-a", "256", file_path.string()}); result) {
        if (result->exit_code == 0) {
            if (auto parsed = parse_sha_output(result->output)) return *parsed;
        }
    }
    return std::unexpected("failed to compute SHA256 for " + file_path.string());
#endif
}

CliResult<fs::path> resolve_tarball_archive_path(const eta::package::ManifestDependency& dependency,
                                                 const fs::path& owner_root,
                                                 const fs::path& cache_root) {
    const std::string source = dependency.tarball;
    auto starts_with = [&](std::string_view prefix) {
        return source.rfind(prefix, 0) == 0;
    };

    if (starts_with("http://") || starts_with("https://")) {
        fs::path downloads_dir = cache_root / "downloads";
        if (auto mk = ensure_directory(downloads_dir); !mk) return std::unexpected(mk.error());
        fs::path archive_path =
            downloads_dir / ("tarball-" + cache_key_from_source(dependency_source_id(dependency)) + ".tar.gz");
        auto download_result = run_external("curl",
                                            {"-L", "--fail", "-o", archive_path.string(), source});
        if (!download_result) return std::unexpected(download_result.error());
        if (*download_result != 0) {
            return std::unexpected("failed to download tarball: " + source);
        }
        return archive_path;
    }

    if (starts_with("file://")) {
        fs::path file_path = fs::path(source.substr(7u));
        return canonicalize_path(file_path);
    }

    fs::path archive_path = fs::path(source);
    if (!archive_path.is_absolute()) {
        archive_path = owner_root / archive_path;
    }
    return canonicalize_path(archive_path);
}

CliResult<fs::path> ensure_git_cache(const eta::package::ManifestDependency& dependency,
                                     const fs::path& cache_root) {
    const std::string source_id = dependency_source_id(dependency);
    const fs::path cache_dir = cache_root / ("git-" + cache_key_from_source(source_id));
    std::error_code ec;
    if (fs::is_regular_file(cache_dir / "eta.toml", ec) && !ec) {
        return cache_dir;
    }

    if (auto mk = ensure_directory(cache_root); !mk) return std::unexpected(mk.error());

    const auto stamp = std::to_string(
        std::chrono::steady_clock::now().time_since_epoch().count());
    const fs::path tmp_dir = cache_root / ("tmp-git-" + stamp);
    if (auto rm = remove_path_if_exists(tmp_dir); !rm) return std::unexpected(rm.error());

    // Lockfile and `eta add` sources reach here without passing through the
    // manifest parser, so re-check, and use `--` so the URL is never an option.
    if (!eta::package::is_safe_git_url(dependency.git)) {
        return std::unexpected("refusing unsafe git url for dependency '" + dependency.name
                               + "': " + dependency.git);
    }
    auto clone_result = run_external(
        "git", {"clone", "--quiet", "--no-checkout", "--", dependency.git, tmp_dir.string()});
    if (!clone_result) return std::unexpected(clone_result.error());
    if (*clone_result != 0) {
        (void)remove_path_if_exists(tmp_dir);
        return std::unexpected("git clone failed for dependency '" + dependency.name + "'");
    }

    auto checkout_result = run_external(
        "git", {"-C", tmp_dir.string(), "checkout", "--quiet", "--detach", dependency.rev});
    if (!checkout_result) return std::unexpected(checkout_result.error());
    if (*checkout_result != 0) {
        (void)remove_path_if_exists(tmp_dir);
        return std::unexpected("git checkout failed for dependency '" + dependency.name + "'");
    }

    if (!fs::is_regular_file(tmp_dir / "eta.toml", ec) || ec) {
        (void)remove_path_if_exists(tmp_dir);
        return std::unexpected("git dependency does not contain eta.toml at repository root: "
                               + dependency.git);
    }

    if (auto rm = remove_path_if_exists(cache_dir); !rm) {
        (void)remove_path_if_exists(tmp_dir);
        return std::unexpected(rm.error());
    }
    fs::rename(tmp_dir, cache_dir, ec);
    if (ec) {
        auto copy = copy_directory_tree(tmp_dir, cache_dir);
        (void)remove_path_if_exists(tmp_dir);
        if (!copy) return std::unexpected(copy.error());
    }
    return cache_dir;
}

CliResult<fs::path> find_tarball_package_root(const fs::path& extracted_root) {
    std::error_code ec;
    if (fs::is_regular_file(extracted_root / "eta.toml", ec) && !ec) {
        return extracted_root;
    }

    std::vector<fs::path> candidates;
    for (const auto& entry : fs::directory_iterator(extracted_root, ec)) {
        if (ec) break;
        std::error_code entry_ec;
        if (!entry.is_directory(entry_ec) || entry_ec) continue;
        if (fs::is_regular_file(entry.path() / "eta.toml", entry_ec) && !entry_ec) {
            candidates.push_back(entry.path());
        }
    }
    if (candidates.size() == 1u) return candidates.front();
    if (candidates.empty()) {
        return std::unexpected("tarball extraction did not produce an eta.toml package root");
    }
    return std::unexpected("tarball extraction produced multiple package roots containing eta.toml");
}

CliResult<fs::path> ensure_tarball_cache(const eta::package::ManifestDependency& dependency,
                                         const fs::path& owner_root,
                                         const fs::path& cache_root) {
    const std::string source_id = dependency_source_id(dependency);
    const fs::path cache_dir = cache_root / ("tarball-" + cache_key_from_source(source_id));
    std::error_code ec;
    if (fs::is_regular_file(cache_dir / "eta.toml", ec) && !ec) {
        return cache_dir;
    }

    if (auto mk = ensure_directory(cache_root); !mk) return std::unexpected(mk.error());

    auto archive_path_res = resolve_tarball_archive_path(dependency, owner_root, cache_root);
    if (!archive_path_res) return std::unexpected(archive_path_res.error());
    const fs::path archive_path = *archive_path_res;

    if (!fs::is_regular_file(archive_path, ec) || ec) {
        return std::unexpected("tarball file not found: " + archive_path.string());
    }

    auto digest = compute_sha256_file(archive_path);
    if (!digest) return std::unexpected(digest.error());
    auto expected = lower_ascii(dependency.sha256);
    if (*digest != expected) {
        return std::unexpected("tarball sha256 mismatch for dependency '" + dependency.name + "'");
    }

    const auto stamp = std::to_string(
        std::chrono::steady_clock::now().time_since_epoch().count());
    const fs::path tmp_extract = cache_root / ("tmp-tarball-" + stamp);
    if (auto rm = remove_path_if_exists(tmp_extract); !rm) return std::unexpected(rm.error());
    if (auto mk = ensure_directory(tmp_extract); !mk) return std::unexpected(mk.error());

    auto extract_result = run_external("tar",
                                       {"-xf", archive_path.string(), "-C", tmp_extract.string()});
    if (!extract_result) {
        (void)remove_path_if_exists(tmp_extract);
        return std::unexpected(extract_result.error());
    }
    if (*extract_result != 0) {
        (void)remove_path_if_exists(tmp_extract);
        return std::unexpected("failed to extract tarball dependency '" + dependency.name + "'");
    }

    auto package_root_res = find_tarball_package_root(tmp_extract);
    if (!package_root_res) {
        (void)remove_path_if_exists(tmp_extract);
        return std::unexpected(package_root_res.error());
    }

    if (auto rm = remove_path_if_exists(cache_dir); !rm) {
        (void)remove_path_if_exists(tmp_extract);
        return std::unexpected(rm.error());
    }
    std::error_code rename_ec;
    fs::rename(*package_root_res, cache_dir, rename_ec);
    if (rename_ec) {
        auto copy = copy_directory_tree(*package_root_res, cache_dir);
        if (!copy) {
            (void)remove_path_if_exists(tmp_extract);
            return std::unexpected(copy.error());
        }
    }
    (void)remove_path_if_exists(tmp_extract);
    return cache_dir;
}

CliResult<eta::package::ResolvedDependencyLocation>
materialize_non_path_for_resolver(const eta::package::Manifest& owner,
                                  const eta::package::ManifestDependency& dependency,
                                  const fs::path& cache_root) {
    fs::path cache_pkg;
    if (dependency.kind == eta::package::ManifestDependencyKind::Git) {
        auto cache = ensure_git_cache(dependency, cache_root);
        if (!cache) return std::unexpected(cache.error());
        cache_pkg = *cache;
    } else if (dependency.kind == eta::package::ManifestDependencyKind::Tarball) {
        auto cache = ensure_tarball_cache(dependency, owner.manifest_path.parent_path(), cache_root);
        if (!cache) return std::unexpected(cache.error());
        cache_pkg = *cache;
    } else {
        return std::unexpected("internal error: non-path materializer received path dependency");
    }

    auto dep_manifest = eta::package::read_manifest(cache_pkg / "eta.toml");
    if (!dep_manifest) {
        return std::unexpected("failed to read dependency manifest: " + dep_manifest.error().message);
    }
    if (dep_manifest->name != dependency.name) {
        return std::unexpected("dependency key '" + dependency.name
                               + "' does not match package.name '" + dep_manifest->name + "'");
    }

    eta::package::ResolvedDependencyLocation location;
    location.manifest_path = canonicalize_path(cache_pkg / "eta.toml");
    location.source = dependency_source_id(dependency);
    return location;
}

CliResult<std::pair<std::string, std::string>> parse_git_source(std::string_view source) {
    if (source.rfind("git+", 0) != 0) {
        return std::unexpected("invalid git source in lockfile: " + std::string(source));
    }
    const std::size_t hash_pos = source.find('#', 4u);
    if (hash_pos == std::string_view::npos || hash_pos + 1u >= source.size()) {
        return std::unexpected("invalid git source in lockfile: " + std::string(source));
    }
    const std::string url(source.substr(4u, hash_pos - 4u));
    const std::string rev(source.substr(hash_pos + 1u));
    if (!looks_like_hex(rev, 40u)) {
        return std::unexpected("invalid git rev in lockfile source: " + std::string(source));
    }
    return std::make_pair(url, rev);
}

CliResult<std::pair<std::string, std::string>> parse_tarball_source(std::string_view source) {
    if (source.rfind("tarball+", 0) != 0) {
        return std::unexpected("invalid tarball source in lockfile: " + std::string(source));
    }
    const std::string marker = "#sha256=";
    const std::size_t marker_pos = source.find(marker, 8u);
    if (marker_pos == std::string_view::npos || marker_pos + marker.size() >= source.size()) {
        return std::unexpected("invalid tarball source in lockfile: " + std::string(source));
    }
    const std::string tarball(source.substr(8u, marker_pos - 8u));
    const std::string sha256(source.substr(marker_pos + marker.size()));
    if (!looks_like_hex(sha256, 64u)) {
        return std::unexpected("invalid tarball sha256 in lockfile source: " + std::string(source));
    }
    return std::make_pair(tarball, sha256);
}

ErrorResult verify_materialized_native_sidecar(const eta::package::LockfilePackage& package,
                                               const fs::path& package_dir) {
    if (!package.native_id.has_value()) return {};

    if (!package.native_artifact_relpath.has_value() || !package.native_sha256.has_value()) {
        return std::unexpected("lockfile package has incomplete native metadata: " + package.name);
    }

    const fs::path artifact_relpath = fs::path(*package.native_artifact_relpath);
    if (artifact_relpath.empty() || artifact_relpath.is_absolute()) {
        return std::unexpected("native sidecar artifact path must be relative for package '"
                               + package.name + "'");
    }

    const fs::path canonical_package_dir = canonicalize_path(package_dir);
    const fs::path artifact_path = canonicalize_path(canonical_package_dir / artifact_relpath);
    if (!is_path_within(canonical_package_dir, artifact_path)) {
        return std::unexpected("native sidecar artifact path escapes package root for package '"
                               + package.name + "': " + artifact_relpath.generic_string());
    }

    std::error_code ec;
    if (!fs::is_regular_file(artifact_path, ec) || ec) {
        return std::unexpected("native sidecar artifact missing for package '" + package.name
                               + "': " + artifact_path.string());
    }

    auto digest = compute_sha256_file(artifact_path);
    if (!digest) return std::unexpected(digest.error());

    const auto expected = lower_ascii(*package.native_sha256);
    if (*digest != expected) {
        return std::unexpected("native sidecar checksum mismatch for package '" + package.name + "'");
    }

    return {};
}

} ///< namespace

ErrorResult materialize_modules_from_lockfile(const fs::path& lockfile_root,
                                              const eta::package::Lockfile& lockfile,
                                              const fs::path& modules_root) {
    auto cache_root_res = eta_cache_modules_root();
    if (!cache_root_res) return std::unexpected(cache_root_res.error());
    const fs::path cache_root = *cache_root_res;
    if (auto mk = ensure_directory(modules_root); !mk) return mk;

    for (const auto& package : lockfile.packages) {
        if (package.source == "root" || package.source.rfind("workspace+", 0) == 0) continue;

        const fs::path package_dir = modules_root / (package.name + "-" + package.version);
        if (package.source.rfind("path+", 0) == 0) {
            const fs::path source_path = fs::path(package.source.substr(5u));
            if (auto copy = copy_directory_tree(source_path, package_dir); !copy) {
                return copy;
            }
            if (auto verify = verify_materialized_native_sidecar(package, package_dir); !verify) {
                return verify;
            }
            continue;
        }

        if (package.source.rfind("git+", 0) == 0) {
            auto parsed = parse_git_source(package.source);
            if (!parsed) return std::unexpected(parsed.error());

            eta::package::ManifestDependency dep;
            dep.name = package.name;
            dep.kind = eta::package::ManifestDependencyKind::Git;
            dep.git = parsed->first;
            dep.rev = parsed->second;

            auto cache = ensure_git_cache(dep, cache_root);
            if (!cache) return std::unexpected(cache.error());
            if (auto materialize = materialize_from_cache(*cache, package_dir); !materialize) {
                return materialize;
            }
            if (auto verify = verify_materialized_native_sidecar(package, package_dir); !verify) {
                return verify;
            }
            continue;
        }

        if (package.source.rfind("tarball+", 0) == 0) {
            eta::package::ManifestDependency dep;
            dep.name = package.name;
            dep.kind = eta::package::ManifestDependencyKind::Tarball;
            auto parsed = parse_tarball_source(package.source);
            if (!parsed) return std::unexpected(parsed.error());
            dep.tarball = parsed->first;
            dep.sha256 = parsed->second;

            const fs::path cache_dir =
                cache_root / ("tarball-" + cache_key_from_source(package.source));
            std::error_code ec;
            fs::path materialized_cache = cache_dir;
            if (!fs::is_regular_file(cache_dir / "eta.toml", ec) || ec) {
                auto cache = ensure_tarball_cache(dep, lockfile_root, cache_root);
                if (!cache) return std::unexpected(cache.error());
                materialized_cache = *cache;
            }
            if (auto materialize = materialize_from_cache(materialized_cache, package_dir); !materialize) {
                return materialize;
            }
            if (auto verify = verify_materialized_native_sidecar(package, package_dir); !verify) {
                return verify;
            }
            continue;
        }

        return std::unexpected("unsupported lockfile source kind: " + package.source);
    }
    return {};
}

CliResult<ResolvedProjectState> resolve_project_state(const fs::path& manifest_path,
                                                      bool include_dev_dependencies,
                                                      bool write_lockfile,
                                                      bool materialize_modules) {
    const fs::path canonical_manifest = canonicalize_path(manifest_path);
    const fs::path project_root = canonical_manifest.parent_path();

    auto manifest = eta::package::read_manifest(canonical_manifest);
    if (!manifest) {
        return std::unexpected(manifest.error().message);
    }

    std::optional<eta::package::WorkspaceMembers> workspace;
    auto discovered = eta::package::discover_manifest_context(project_root);
    if (!discovered) return std::unexpected(discovered.error().message);
    if (discovered->workspace_manifest_path.has_value()) {
        auto resolved_members =
            eta::package::resolve_workspace_members(*discovered->workspace_manifest_path);
        if (!resolved_members) {
            return std::unexpected(resolved_members.error().message);
        }

        const auto manifest_match = std::find_if(
            resolved_members->members.begin(),
            resolved_members->members.end(),
            [&](const eta::package::WorkspaceMember& member) {
                return path_key(member.manifest_path) == path_key(canonical_manifest);
            });
        if (manifest_match != resolved_members->members.end()) {
            workspace = std::move(*resolved_members);
        }
    }

    fs::path lockfile_root = project_root;
    fs::path modules_root = project_root / ".eta" / "modules";
    if (workspace.has_value()) {
        lockfile_root = workspace->workspace_root;
        modules_root = lockfile_root / ".eta" / "modules";
    }

    std::optional<eta::package::Lockfile> existing_lockfile;
    {
        auto lock = eta::package::read_lockfile(lockfile_root / "eta.lock");
        if (lock) existing_lockfile = std::move(*lock);
    }

    auto cache_root_res = eta_cache_modules_root();
    if (!cache_root_res) return std::unexpected(cache_root_res.error());
    const fs::path cache_root = *cache_root_res;

    eta::package::ResolveOptions options;
    options.include_dev_dependencies = include_dev_dependencies;
    if (existing_lockfile.has_value()) options.lockfile = &*existing_lockfile;
    options.modules_root = modules_root;
    options.dependency_locator =
        [&](const eta::package::Manifest& owner,
            const eta::package::ManifestDependency& dependency)
            -> std::expected<eta::package::ResolvedDependencyLocation, eta::package::ResolveError> {
        auto location = materialize_non_path_for_resolver(owner, dependency, cache_root);
        if (!location) {
            return std::unexpected(eta::package::ResolveError{
                eta::package::ResolveError::Code::MissingDependencyManifest,
                location.error(),
            });
        }
        return *location;
    };

    auto graph = eta::package::resolve_dependencies(canonical_manifest, options);
    if (!graph) return std::unexpected(graph.error().message);

    eta::package::Lockfile lockfile;
    if (workspace.has_value()) {
        auto workspace_graph = eta::package::resolve_workspace_dependencies(*workspace, options);
        if (!workspace_graph) return std::unexpected(workspace_graph.error().message);
        lockfile = eta::package::build_lockfile(*workspace_graph);
    } else {
        lockfile = eta::package::build_lockfile(*graph);
    }

    if (write_lockfile) {
        auto write_result = eta::package::write_lockfile_file(lockfile, lockfile_root / "eta.lock");
        if (!write_result) return std::unexpected(write_result.error().message);
    }

    if (materialize_modules) {
        auto materialize = materialize_modules_from_lockfile(lockfile_root, lockfile, modules_root);
        if (!materialize) return std::unexpected(materialize.error());
    }

    ResolvedProjectState state;
    state.manifest_path = canonical_manifest;
    state.project_root = project_root;
    state.lockfile_root = lockfile_root;
    state.modules_root = modules_root;
    if (workspace.has_value()) {
        state.workspace_root = workspace->workspace_root;
    }
    state.manifest = std::move(*manifest);
    state.graph = std::move(*graph);
    state.lockfile = std::move(lockfile);
    return state;
}

[[nodiscard]] fs::path workspace_member_target_profile_root(const fs::path& workspace_root,
                                                            std::string_view profile,
                                                            std::string_view member_name) {
    return workspace_root / ".eta" / "target" / std::string(profile) / std::string(member_name);
}

[[nodiscard]] fs::path selected_package_target_profile_root(const ResolvedProjectState& state,
                                                            std::string_view profile) {
    if (state.workspace_root.has_value()) {
        return workspace_member_target_profile_root(*state.workspace_root, profile, state.manifest.name);
    }
    return state.project_root / ".eta" / "target" / std::string(profile);
}

[[nodiscard]] fs::path lockfile_package_release_root(
    const ResolvedProjectState& state,
    const eta::package::LockfilePackage& package) {
    if (package.source == "root") {
        return selected_package_target_profile_root(state, "release");
    }
    if (package.source.rfind("workspace+", 0) == 0 && state.workspace_root.has_value()) {
        return workspace_member_target_profile_root(*state.workspace_root, "release", package.name);
    }
    return state.modules_root / (package.name + "-" + package.version) / "target" / "release";
}

[[nodiscard]] std::string display_source(const eta::package::ResolvedPackage& package,
                                         const fs::path& root_dir) {
    if (package.source == "root") return ".";
    if (package.source.rfind("path+", 0) == 0) {
        const fs::path raw = package.source.substr(5u);
        std::error_code ec;
        auto rel = fs::relative(raw, root_dir, ec);
        if (!ec && !rel.empty()) return rel.generic_string();
        return raw.generic_string();
    }
    return package.source;
}

} ///< namespace eta::cli
