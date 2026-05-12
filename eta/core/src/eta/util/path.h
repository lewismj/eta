#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace eta::util {

namespace fs = std::filesystem;

/**
 * @brief Canonicalize a filesystem path using weakly-canonical fallback.
 *
 * Returns weakly-canonical when available, otherwise a lexically-normal path.
 */
[[nodiscard]] fs::path canonicalize_path(const fs::path& path);

/**
 * @brief Build a stable lookup key for a filesystem path.
 *
 * The key is derived from @ref canonicalize_path. On Windows it is lowercased
 * and path separators are normalized to backslashes.
 */
[[nodiscard]] std::string canonical_path_key(const fs::path& path);

/**
 * @brief Resolve the current executable path for the running process.
 *
 * Returns nullopt when the platform probe is unavailable.
 */
[[nodiscard]] std::optional<fs::path> current_executable_path();

/**
 * @brief Build a sibling executable path next to the current executable.
 *
 * If the current executable path cannot be resolved, returns only the
 * executable basename suitable for PATH lookup.
 */
[[nodiscard]] fs::path sibling_executable_path(std::string_view basename);

} // namespace eta::util
