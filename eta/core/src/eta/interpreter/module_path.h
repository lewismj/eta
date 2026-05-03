#pragma once

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

#include "eta/package/lockfile.h"

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
                                bool strict_shadow_scan = false)
        : dirs_(std::move(dirs)),
          strict_shadow_scan_(strict_shadow_scan) {}

    /**
     * Construct from a PATH-style string (semicolon-delimited on Windows,
     * colon-delimited on POSIX).
     */
    static ModulePathResolver from_path_string(std::string_view path_str) {
        std::vector<fs::path> dirs;
        if (path_str.empty()) return ModulePathResolver{dirs, false};

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
        return ModulePathResolver{std::move(dirs), false};
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
            auto normalized = canonicalize_path(raw_dir);
            if (seen.insert(canonical_key(normalized)).second) {
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

        auto add_project_roots = [&](const fs::path& manifest_path) {
            const auto project_root = manifest_path.parent_path();

            std::error_code ec;
            const auto src_dir = project_root / "src";
            if (fs::is_directory(src_dir, ec) && !ec) {
                add_unique_dir(src_dir);
            } else {
                add_unique_dir(project_root);
            }

            const auto modules_root = project_root / ".eta" / "modules";
            ec.clear();
            if (!fs::is_directory(modules_root, ec) || ec) return;

            auto lockfile = eta::package::read_lockfile(project_root / "eta.lock");
            if (lockfile) {
                for (const auto& pkg : lockfile->packages) {
                    if (pkg.source == "root") continue;
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

        if (discovery_start.empty()) {
            std::error_code ec;
            discovery_start = fs::current_path(ec);
            if (ec) discovery_start.clear();
        }

        if (!discovery_start.empty()) {
            if (auto manifest_path = find_manifest_path(discovery_start)) {
                add_project_roots(*manifest_path);
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
#ifdef _WIN32
        /// Windows: use GetModuleFileName
        wchar_t buf[4096];
        DWORD len = GetModuleFileNameW(nullptr, buf, sizeof(buf) / sizeof(buf[0]));
        if (len == 0 || len >= sizeof(buf) / sizeof(buf[0])) return std::nullopt;
        fs::path exe_path(buf);
#else
        /// POSIX: /proc/self/exe (Linux), or argv[0] fallback
        fs::path exe_path = fs::read_symlink("/proc/self/exe", ec);
        if (ec) return std::nullopt;
#endif
        auto prefix = exe_path.parent_path().parent_path();
        auto stdlib = prefix / "stdlib";
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
        const auto rel_etac = module_to_relative(module_name, ".etac");
        const auto rel_eta = module_to_relative(module_name, ".eta");

        std::vector<fs::path> matches;
        matches.reserve(dirs_.size());
        for (const auto& dir : dirs_) {
            std::error_code ec;

            auto etac_candidate = dir / rel_etac;
            if (fs::is_regular_file(etac_candidate, ec) && !ec) {
                matches.push_back(canonicalize_path(etac_candidate));
                continue;
            }
            ec.clear();

            auto eta_candidate = dir / rel_eta;
            if (fs::is_regular_file(eta_candidate, ec) && !ec) {
                matches.push_back(canonicalize_path(eta_candidate));
                continue;
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
            if (fs::is_regular_file(candidate, ec) && !ec) return canonicalize_path(candidate);
        }
        return std::nullopt;
    }

    [[nodiscard]] const std::vector<fs::path>& dirs() const noexcept { return dirs_; }
    [[nodiscard]] bool empty() const noexcept { return dirs_.empty(); }
    [[nodiscard]] bool strict_shadow_scan() const noexcept { return strict_shadow_scan_; }

    void add_dir(fs::path dir) { dirs_.push_back(std::move(dir)); }
    void set_strict_shadow_scan(bool enabled) noexcept { strict_shadow_scan_ = enabled; }

private:
    [[nodiscard]] static fs::path canonicalize_path(const fs::path& path) {
        std::error_code ec;
        const auto canonical = fs::weakly_canonical(path, ec);
        if (!ec) return canonical;
        return path.lexically_normal();
    }

    [[nodiscard]] static std::string canonical_key(const fs::path& path) {
        auto normalized = canonicalize_path(path).generic_string();
#ifdef _WIN32
        std::transform(normalized.begin(),
                       normalized.end(),
                       normalized.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
#endif
        return normalized;
    }

    [[nodiscard]] static std::optional<fs::path> find_manifest_path(fs::path start_dir) {
        start_dir = canonicalize_path(start_dir);
        while (true) {
            const auto candidate = start_dir / "eta.toml";
            std::error_code ec;
            if (fs::is_regular_file(candidate, ec) && !ec) {
                return canonicalize_path(candidate);
            }
            const auto parent = start_dir.parent_path();
            if (parent.empty() || parent == start_dir) break;
            start_dir = parent;
        }
        return std::nullopt;
    }

    std::vector<fs::path> dirs_;
    bool strict_shadow_scan_{false};
};

} ///< namespace eta::interpreter

