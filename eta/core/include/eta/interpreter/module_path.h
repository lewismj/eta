#pragma once

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

namespace eta::interpreter {

namespace fs = std::filesystem;

/**
 * @brief Resolves dotted module names to .eta file paths on disk.
 *
 * Module names use dot-separated components (e.g. "std.core") which map
 * to directory-separated file paths (e.g. "std/core.eta").
 *
 * Search order: each directory in the path list is tried in order.
 */
class ModulePathResolver {
public:
    /// Construct with explicit search directories.
    explicit ModulePathResolver(std::vector<fs::path> dirs = {})
        : dirs_(std::move(dirs)) {}

    /// Construct from a PATH-style string (semicolon-delimited on Windows,
    /// colon-delimited on POSIX).
    static ModulePathResolver from_path_string(std::string_view path_str) {
        std::vector<fs::path> dirs;
        if (path_str.empty()) return ModulePathResolver{dirs};

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
        return ModulePathResolver{std::move(dirs)};
    }

    /// Construct from CLI --path argument, falling back to ETA_MODULE_PATH env var.
    /// Always appends the bundled stdlib directory (<exe>/../stdlib/) as a
    /// last-resort search path so the prelude is found automatically.
    static ModulePathResolver from_args_or_env(const std::string& cli_path) {
        ModulePathResolver resolver{{}};
        if (!cli_path.empty()) {
            resolver = from_path_string(cli_path);
        } else {
            const char* env = std::getenv("ETA_MODULE_PATH");
            if (env && env[0] != '\0') {
                resolver = from_path_string(env);
            }
        }
        // Append the bundled stdlib directory relative to the executable.
        // Layout: <prefix>/bin/etai  →  <prefix>/stdlib/
        auto stdlib = bundled_stdlib_dir();
        if (stdlib) {
            resolver.add_dir(*stdlib);
        }
        return resolver;
    }

    /// Locate the bundled stdlib directory relative to the running executable.
    /// Returns nullopt if the directory does not exist on disk.
    static std::optional<fs::path> bundled_stdlib_dir() {
        std::error_code ec;
#ifdef _WIN32
        // Windows: use GetModuleFileName
        wchar_t buf[4096];
        DWORD len = GetModuleFileNameW(nullptr, buf, sizeof(buf) / sizeof(buf[0]));
        if (len == 0 || len >= sizeof(buf) / sizeof(buf[0])) return std::nullopt;
        fs::path exe_path(buf);
#else
        // POSIX: /proc/self/exe (Linux), or argv[0] fallback
        fs::path exe_path = fs::read_symlink("/proc/self/exe", ec);
        if (ec) return std::nullopt;
#endif
        // <prefix>/bin/etai  →  <prefix>/stdlib
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
    static fs::path module_to_relative(const std::string& module_name) {
        std::string rel = module_name;
        std::replace(rel.begin(), rel.end(), '.', '/');
        rel += ".eta";
        return fs::path{rel};
    }

    /**
     * @brief Resolve a module name to an absolute file path.
     * Returns nullopt if the file is not found in any search directory.
     */
    [[nodiscard]] std::optional<fs::path> resolve(const std::string& module_name) const {
        auto rel = module_to_relative(module_name);
        for (const auto& dir : dirs_) {
            auto candidate = dir / rel;
            if (fs::exists(candidate) && fs::is_regular_file(candidate)) {
                return fs::canonical(candidate);
            }
        }
        return std::nullopt;
    }

    /**
     * @brief Resolve a specific filename (e.g. "prelude.eta") relative to search directories.
     */
    [[nodiscard]] std::optional<fs::path> find_file(const std::string& filename) const {
        for (const auto& dir : dirs_) {
            auto candidate = dir / filename;
            if (fs::exists(candidate) && fs::is_regular_file(candidate)) {
                return fs::canonical(candidate);
            }
        }
        return std::nullopt;
    }

    [[nodiscard]] const std::vector<fs::path>& dirs() const noexcept { return dirs_; }
    [[nodiscard]] bool empty() const noexcept { return dirs_.empty(); }

    void add_dir(fs::path dir) { dirs_.push_back(std::move(dir)); }

private:
    std::vector<fs::path> dirs_;
};

} // namespace eta::interpreter

