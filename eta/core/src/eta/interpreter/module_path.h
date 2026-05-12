#pragma once

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

#include "eta/package/discovery.h"
#include "eta/package/lockfile.h"
#include "eta/package/resolver.h"
#include "eta/util/path.h"

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

#ifdef _WIN32
        constexpr char sep = ';';
#else
        constexpr char sep = ':';
#endif
        std::string s{path_str};
        std::string::size_type start = 0;
        while (start < s.size()) {
            auto pos = s.find(sep, start);
            if (pos == std::string::npos) pos = s.size();
            auto part = s.substr(start, pos - start);
            if (!part.empty()) {
                dirs.emplace_back(part);
            }
            start = pos + 1;
        }
        return ModulePathResolver{std::move(dirs), false, false};
    }

    /**
     * Construct from CLI --path argument, falling back to ETA_MODULE_PATH env var.
     * Injects project-local package roots when the current working directory (or
     * one of its ancestors) contains `eta.toml`.
     * Also appends compile-time and bundled stdlib fallbacks as last-resort
     * search paths so the prelude is found automatically.
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
                                                  bool strict_shadow_scan = false) {
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
            const auto release_dir = package_root / "target" / "release";
            if (fs::is_directory(release_dir, ec) && !ec) {
                add_unique_dir(release_dir);
                added = true;
            }
            ec.clear();

            const auto src_dir = package_root / "src";
            if (fs::is_directory(src_dir, ec) && !ec) {
                add_unique_dir(src_dir);
                added = true;
            }

            /// Backward-compatible fallback for local/dev module trees.
            if (!added) add_unique_dir(package_root);
        };

        auto add_package_source_root = [&](const fs::path& package_root) {
            std::error_code ec;
            const auto src_dir = package_root / "src";
            if (fs::is_directory(src_dir, ec) && !ec) {
                add_unique_dir(src_dir);
            } else {
                add_unique_dir(package_root);
            }
        };

        auto add_modules_from_lockfile = [&](const fs::path& lockfile_root) {
            const auto modules_root = lockfile_root / ".eta" / "modules";
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

        ModulePathResolver configured_paths{{}, false};
        if (!cli_path.empty()) {
            configured_paths = from_path_string(cli_path);
        } else {
            const char* env = std::getenv("ETA_MODULE_PATH");
            if (env && env[0] != '\0') {
                configured_paths = from_path_string(env);
            }
        }
        for (const auto& dir : configured_paths.dirs()) add_unique_dir(dir);

        /// Append build-tree stdlib when injected by CMake for developer binaries.
#ifdef ETA_STDLIB_DIR
        add_unique_dir(fs::path(ETA_STDLIB_DIR));
#endif
        /// Append the bundled stdlib directory relative to the executable.
        auto stdlib = bundled_stdlib_dir();
        if (stdlib) {
            add_unique_dir(*stdlib);
        }
        return resolver;
    }

    /**
     * Locate the bundled stdlib directory relative to the running executable.
     * Returns nullopt if the directory does not exist on disk.
     */
    static std::optional<fs::path> bundled_stdlib_dir() {
        std::error_code ec;
        auto exe_path = util::current_executable_path();
        if (!exe_path.has_value()) return std::nullopt;
        auto stdlib = exe_path->parent_path().parent_path() / "stdlib";
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
    std::vector<fs::path> dirs_;
    bool strict_shadow_scan_{false};
    bool prefer_source_{false};
};

} ///< namespace eta::interpreter

