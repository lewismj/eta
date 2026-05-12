#pragma once

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "eta/package/discovery.h"
#include "eta/package/lockfile.h"
#include "eta/package/resolver.h"
#include "eta/util/path.h"
#include "eta/util/runtime_layout.h"

namespace eta::interpreter {

namespace fs = std::filesystem;

/**
 * @brief Resolves dotted module names to `.etac` / `.eta` file paths on disk.
 *
 * Module names use dot-separated components (e.g. "std.core") which map
 * to directory-separated file paths (e.g. "std/core.etac", "std/core.eta").
 *
 * Search order: each directory in the path list is tried in order.
 */
class ModulePathResolver {
public:
    /// Construct with explicit search directories.
    explicit ModulePathResolver(std::vector<fs::path> dirs = {},
                                bool strict_shadow_scan = false,
                                bool prefer_source = false)
        : dirs_(std::move(dirs)),
          strict_shadow_scan_(strict_shadow_scan),
          prefer_source_(prefer_source) {}

    /**
     * Construct from a PATH-style string (semicolon-delimited on Windows,
     * colon-delimited on POSIX).
     */
    static ModulePathResolver from_path_string(std::string_view path_str) {
        std::vector<fs::path> dirs;
        if (path_str.empty()) return ModulePathResolver{dirs, false, false};
        auto entries = split_path_entries(path_str);
        dirs.reserve(entries.size());
        for (const auto& entry : entries) {
            dirs.emplace_back(entry);
        }
        return ModulePathResolver{std::move(dirs), false, false};
    }

    /**
     * Construct from CLI --path argument, falling back to ETA_MODULE_PATH env var.
     * Injects project-local package roots when the current working directory (or
     * one of its ancestors) contains `eta.toml`.
     * Also appends compile-time and bundled stdlib fallbacks as last-resort
     * search paths so stdlib modules are found automatically.
     */
    static ModulePathResolver from_args_or_env(const std::string& cli_path,
                                               bool strict_shadow_scan = false) {
        std::error_code ec;
        auto start_dir = fs::current_path(ec);
        if (ec) start_dir.clear();
        return from_args_or_env_at(cli_path, start_dir, strict_shadow_scan);
    }

    /**
     * Construct from CLI --path argument, falling back to ETA_MODULE_PATH env var.
     * Project-root discovery starts at @p discovery_start and walks parent
     * directories for `eta.toml`.
     */
    static ModulePathResolver from_args_or_env_at(const std::string& cli_path,
                                                  fs::path discovery_start,
                                                  bool strict_shadow_scan = false,
                                                  const util::RuntimeLayoutConfig& layout =
                                                      util::kDefaultRuntimeLayoutConfig) {
        ModulePathResolver resolver{{}, strict_shadow_scan};
        std::unordered_set<std::string> seen;

        auto add_unique_dir = [&](const fs::path& raw_dir) {
            if (raw_dir.empty()) return;
            std::error_code ec;
            if (!fs::is_directory(raw_dir, ec) || ec) return;
            auto normalized = util::canonicalize_path(raw_dir);
            if (seen.insert(util::canonical_path_key(normalized)).second) {
                resolver.add_dir(std::move(normalized));
            }
        };

        auto add_package_layout_dirs = [&](const fs::path& package_root) {
            std::error_code ec;
            if (!fs::is_directory(package_root, ec) || ec) return;

            bool added = false;
            const auto release_dir = util::package_build_output_dir(package_root, layout);
            if (fs::is_directory(release_dir, ec) && !ec) {
                add_unique_dir(release_dir);
                added = true;
            }
            ec.clear();

            const auto src_dir = util::package_source_dir(package_root, layout);
            if (fs::is_directory(src_dir, ec) && !ec) {
                add_unique_dir(src_dir);
                added = true;
            }

            /// Backward-compatible fallback for local/dev module trees.
            if (!added) add_unique_dir(package_root);
        };

        auto add_package_source_root = [&](const fs::path& package_root) {
            std::error_code ec;
            const auto src_dir = util::package_source_dir(package_root, layout);
            if (fs::is_directory(src_dir, ec) && !ec) {
                add_unique_dir(src_dir);
            } else {
                add_unique_dir(package_root);
            }
        };

        auto add_modules_from_lockfile = [&](const fs::path& lockfile_root) {
            const auto modules_root = util::workspace_modules_root(lockfile_root, layout);
            constexpr std::string_view kWorkspacePrefix = "workspace+";
            std::error_code ec;
            ec.clear();
            if (!fs::is_directory(modules_root, ec) || ec) return;

            auto lockfile = eta::package::read_lockfile(lockfile_root / "eta.lock");
            if (lockfile) {
                for (const auto& pkg : lockfile->packages) {
                    if (pkg.source == "root") continue;
                    if (pkg.source.starts_with(kWorkspacePrefix)) {
                        std::string rel = pkg.source.substr(kWorkspacePrefix.size());
                        fs::path member_root = lockfile_root;
                        if (!rel.empty() && rel != ".") {
                            member_root /= fs::path(std::move(rel));
                        }
                        add_package_layout_dirs(member_root);
                        continue;
                    }
                    const auto package_dir = modules_root / (pkg.name + "-" + pkg.version);
                    add_package_layout_dirs(package_dir);
                }
                return;
            }

            std::vector<fs::path> package_dirs;
            for (const auto& entry : fs::directory_iterator(modules_root, ec)) {
                if (ec) break;
                std::error_code entry_ec;
                if (entry.is_directory(entry_ec) && !entry_ec) {
                    package_dirs.push_back(entry.path());
                }
            }
            std::sort(package_dirs.begin(), package_dirs.end());
            for (const auto& package_dir : package_dirs) {
                add_package_layout_dirs(package_dir);
            }
        };

        auto add_workspace_member_roots = [&](const fs::path& workspace_manifest_path) {
            auto workspace_members = eta::package::resolve_workspace_members(workspace_manifest_path);
            if (!workspace_members) return;
            for (const auto& member : workspace_members->members) {
                add_package_layout_dirs(member.package_root);
            }
        };

        std::unordered_map<std::string, PackageCollectionCacheEntry> package_collection_cache;

        auto discover_package_roots =
            [&](const fs::path& raw_collection_root) -> const std::vector<fs::path>& {
            static const std::vector<fs::path> empty_roots;
            if (raw_collection_root.empty()) return empty_roots;

            std::error_code ec;
            if (!fs::is_directory(raw_collection_root, ec) || ec) return empty_roots;

            const auto collection_root = util::canonicalize_path(raw_collection_root);
            const auto cache_key = util::canonical_path_key(collection_root);

            bool has_mtime = false;
            fs::file_time_type observed_mtime{};
            ec.clear();
            const auto mtime = fs::last_write_time(collection_root, ec);
            if (!ec) {
                has_mtime = true;
                observed_mtime = mtime;
            }

            auto cache_it = package_collection_cache.find(cache_key);
            if (cache_it != package_collection_cache.end()) {
                const bool cache_valid =
                    (has_mtime == cache_it->second.has_mtime)
                    && (!has_mtime || cache_it->second.mtime == observed_mtime);
                if (cache_valid) return cache_it->second.package_roots;
            }

            PackageCollectionCacheEntry cache_entry;
            cache_entry.has_mtime = has_mtime;
            cache_entry.mtime = observed_mtime;

            std::unordered_set<std::string> seen_package_roots;
            std::function<void(const fs::path&, std::size_t)> scan_dir;
            scan_dir = [&](const fs::path& raw_dir, std::size_t depth) {
                std::error_code dir_ec;
                if (!fs::is_directory(raw_dir, dir_ec) || dir_ec) return;

                const auto dir = util::canonicalize_path(raw_dir);
                const auto manifest_path = dir / "eta.toml";
                dir_ec.clear();
                if (fs::is_regular_file(manifest_path, dir_ec) && !dir_ec) {
                    const auto package_key = util::canonical_path_key(dir);
                    if (seen_package_roots.insert(package_key).second) {
                        cache_entry.package_roots.push_back(dir);
                    }
                    return;
                }

                if (depth >= kPackageCollectionDepthLimit) return;

                std::vector<fs::path> children;
                for (const auto& child : fs::directory_iterator(
                         dir,
                         fs::directory_options::skip_permission_denied,
                         dir_ec)) {
                    if (dir_ec) break;
                    std::error_code child_ec;
                    if (child.is_directory(child_ec) && !child_ec) {
                        children.push_back(child.path());
                    }
                }
                if (dir_ec) return;

                std::sort(children.begin(),
                          children.end(),
                          [](const fs::path& lhs, const fs::path& rhs) {
                              return util::canonical_path_key(lhs)
                                  < util::canonical_path_key(rhs);
                          });

                for (const auto& child : children) {
                    scan_dir(child, depth + 1u);
                }
            };

            scan_dir(collection_root, 0u);

            auto [inserted_it, _] = package_collection_cache.insert_or_assign(
                cache_key, std::move(cache_entry));
            return inserted_it->second.package_roots;
        };

        auto add_package_collection_dirs = [&](const fs::path& collection_root) {
            const auto& package_roots = discover_package_roots(collection_root);
            for (const auto& package_root : package_roots) {
                add_package_layout_dirs(package_root);
            }
        };

        auto apply_module_path_entry = [&](const ModulePathEntry& entry) {
            switch (entry.kind) {
                case ModulePathEntryKind::Dir:
                    add_unique_dir(entry.path);
                    return;
                case ModulePathEntryKind::Pkg:
                    add_package_layout_dirs(entry.path);
                    return;
                case ModulePathEntryKind::Pkgs:
                    add_package_collection_dirs(entry.path);
                    return;
                case ModulePathEntryKind::Auto:
                    break;
            }

            std::error_code ec;
            if (!fs::is_directory(entry.path, ec) || ec) return;

            const auto canonical_path = util::canonicalize_path(entry.path);
            const auto manifest_path = canonical_path / "eta.toml";
            ec.clear();
            if (fs::is_regular_file(manifest_path, ec) && !ec) {
                add_package_layout_dirs(canonical_path);
                return;
            }

            const auto& package_roots = discover_package_roots(canonical_path);
            if (!package_roots.empty()) {
                for (const auto& package_root : package_roots) {
                    add_package_layout_dirs(package_root);
                }
                return;
            }

            add_unique_dir(canonical_path);
        };

        if (discovery_start.empty()) {
            std::error_code ec;
            discovery_start = fs::current_path(ec);
            if (ec) discovery_start.clear();
        }

        if (!discovery_start.empty()) {
            bool configured_from_context = false;
            if (auto discovered = eta::package::discover_manifest_context(discovery_start);
                discovered) {
                if (discovered->workspace_manifest_path.has_value()) {
                    const auto workspace_manifest = *discovered->workspace_manifest_path;
                    const auto workspace_root = workspace_manifest.parent_path();
                    add_modules_from_lockfile(workspace_root);
                    add_workspace_member_roots(workspace_manifest);
                    configured_from_context = true;
                }

                if (discovered->package_manifest_path.has_value()) {
                    add_package_source_root(discovered->package_manifest_path->parent_path());
                    if (!discovered->workspace_manifest_path.has_value()) {
                        add_modules_from_lockfile(discovered->package_manifest_path->parent_path());
                    }
                    configured_from_context = true;
                }
            }

            if (!configured_from_context) {
                if (auto manifest_path = eta::package::find_nearest_manifest_path(discovery_start)) {
                    const auto project_root = manifest_path->parent_path();
                    add_package_source_root(project_root);
                    add_modules_from_lockfile(project_root);
                }
            }
        }

        auto apply_path_list = [&](std::string_view path_list) {
            for (const auto& raw_entry : split_path_entries(path_list)) {
                auto parsed_entry = parse_module_path_entry(raw_entry);
                if (!parsed_entry.has_value()) continue;
                apply_module_path_entry(*parsed_entry);
            }
        };

        if (!cli_path.empty()) {
            apply_path_list(cli_path);
        } else {
            const char* env = std::getenv("ETA_MODULE_PATH");
            if (env && env[0] != '\0') {
                apply_path_list(env);
            }
        }

        /// Append build-tree stdlib when injected by CMake for developer binaries.
#ifdef ETA_STDLIB_DIR
        add_unique_dir(fs::path(ETA_STDLIB_DIR));
#endif
        /// Append the bundled stdlib directory relative to the executable.
        auto stdlib = bundled_stdlib_dir(layout);
        if (stdlib) {
            add_unique_dir(*stdlib);
        }
        return resolver;
    }

    /**
     * Locate the bundled stdlib directory relative to the running executable.
     * Returns nullopt if the directory does not exist on disk.
     */
    static std::optional<fs::path> bundled_stdlib_dir(
        const util::RuntimeLayoutConfig& layout = util::kDefaultRuntimeLayoutConfig) {
        std::error_code ec;
        auto exe_path = util::current_executable_path();
        if (!exe_path.has_value()) return std::nullopt;
        const auto stdlib = util::bundled_stdlib_root_from_executable(*exe_path, layout);
        if (fs::is_directory(stdlib, ec)) {
            return stdlib;
        }
        return std::nullopt;
    }

    /**
     * @brief Map a dotted module name to a relative path.
     *
     * "std.core" -> "std/core.eta"
     */
    static fs::path module_to_relative(const std::string& module_name,
                                       std::string_view extension = ".eta") {
        std::string rel = module_name;
        std::replace(rel.begin(), rel.end(), '.', '/');
        rel += extension;
        return fs::path{rel};
    }

    /**
     * @brief Resolve all module candidates in search-order.
     *
     * Per root, `.etac` is preferred over `.eta`.
     */
    [[nodiscard]] std::vector<fs::path> resolve_all(const std::string& module_name) const {
        const auto rel_eta = module_to_relative(module_name, ".eta");
        const auto rel_etac = module_to_relative(module_name, ".etac");

        std::vector<fs::path> matches;
        matches.reserve(dirs_.size());
        for (const auto& dir : dirs_) {
            std::error_code ec;
            const std::array<fs::path, 2> candidates = prefer_source_
                ? std::array<fs::path, 2>{dir / rel_eta, dir / rel_etac}
                : std::array<fs::path, 2>{dir / rel_etac, dir / rel_eta};
            for (const auto& candidate : candidates) {
                if (fs::is_regular_file(candidate, ec) && !ec) {
                    matches.push_back(util::canonicalize_path(candidate));
                    break;
                }
                ec.clear();
            }
        }
        return matches;
    }

    /**
     * @brief Resolve a module name to an absolute file path.
     * Returns nullopt if the file is not found in any search directory.
     */
    [[nodiscard]] std::optional<fs::path> resolve(const std::string& module_name) const {
        auto matches = resolve_all(module_name);
        if (matches.empty()) return std::nullopt;
        return matches.front();
    }

    /**
     * @brief Resolve a specific filename (e.g. "prelude.eta") relative to search directories.
     */
    [[nodiscard]] std::optional<fs::path> find_file(const std::string& filename) const {
        for (const auto& dir : dirs_) {
            auto candidate = dir / filename;
            std::error_code ec;
            if (fs::is_regular_file(candidate, ec) && !ec) {
                return util::canonicalize_path(candidate);
            }
        }
        return std::nullopt;
    }

    [[nodiscard]] const std::vector<fs::path>& dirs() const noexcept { return dirs_; }
    [[nodiscard]] bool empty() const noexcept { return dirs_.empty(); }
    [[nodiscard]] bool strict_shadow_scan() const noexcept { return strict_shadow_scan_; }
    [[nodiscard]] bool prefer_source() const noexcept { return prefer_source_; }

    void add_dir(fs::path dir) { dirs_.push_back(std::move(dir)); }
    void set_strict_shadow_scan(bool enabled) noexcept { strict_shadow_scan_ = enabled; }
    void set_prefer_source(bool enabled) noexcept { prefer_source_ = enabled; }

private:
    static constexpr std::size_t kPackageCollectionDepthLimit = 3u;

    enum class ModulePathEntryKind {
        Auto,
        Dir,
        Pkg,
        Pkgs,
    };

    struct ModulePathEntry {
        ModulePathEntryKind kind{ModulePathEntryKind::Auto};
        fs::path path;
    };

    struct PackageCollectionCacheEntry {
        bool has_mtime{false};
        fs::file_time_type mtime{};
        std::vector<fs::path> package_roots;
    };

    [[nodiscard]] static std::string trim_ascii(std::string_view text) {
        const auto first = text.find_first_not_of(" \t\r\n");
        if (first == std::string_view::npos) return {};
        const auto last = text.find_last_not_of(" \t\r\n");
        return std::string(text.substr(first, last - first + 1u));
    }

    [[nodiscard]] static std::vector<std::string> split_path_entries(std::string_view path_str) {
        std::vector<std::string> entries;
        if (path_str.empty()) return entries;

#ifdef _WIN32
        constexpr char sep = ';';
#else
        constexpr char sep = ':';
#endif

        std::string text(path_str);
        std::string::size_type start = 0;
        while (start <= text.size()) {
            auto pos = text.find(sep, start);
            if (pos == std::string::npos) pos = text.size();
            auto part = trim_ascii(std::string_view(text).substr(start, pos - start));
            if (!part.empty()) {
                entries.push_back(std::move(part));
            }
            if (pos == text.size()) break;
            start = pos + 1u;
        }

        return entries;
    }

    [[nodiscard]] static std::optional<ModulePathEntry> parse_module_path_entry(
        std::string_view raw_entry) {
        constexpr std::string_view kDirPrefix = "dir+";
        constexpr std::string_view kPkgPrefix = "pkg+";
        constexpr std::string_view kPkgsPrefix = "pkgs+";

        const auto entry = trim_ascii(raw_entry);
        if (entry.empty()) return std::nullopt;

        auto make_entry = [](ModulePathEntryKind kind, std::string_view path_text)
                              -> std::optional<ModulePathEntry> {
            const auto trimmed_path = trim_ascii(path_text);
            if (trimmed_path.empty()) return std::nullopt;
            return ModulePathEntry{kind, fs::path(trimmed_path)};
        };

        auto starts_with = [](const std::string& value, std::string_view prefix) {
            return value.size() >= prefix.size()
                && std::equal(prefix.begin(), prefix.end(), value.begin());
        };

        if (starts_with(entry, kPkgsPrefix)) {
            return make_entry(ModulePathEntryKind::Pkgs, entry.substr(kPkgsPrefix.size()));
        }
        if (starts_with(entry, kPkgPrefix)) {
            return make_entry(ModulePathEntryKind::Pkg, entry.substr(kPkgPrefix.size()));
        }
        if (starts_with(entry, kDirPrefix)) {
            return make_entry(ModulePathEntryKind::Dir, entry.substr(kDirPrefix.size()));
        }
        return ModulePathEntry{ModulePathEntryKind::Auto, fs::path(entry)};
    }

    std::vector<fs::path> dirs_;
    bool strict_shadow_scan_{false};
    bool prefer_source_{false};
};

} ///< namespace eta::interpreter

