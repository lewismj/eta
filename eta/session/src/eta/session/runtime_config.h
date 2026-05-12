#pragma once

#include <cctype>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdlib>

namespace eta::session {

/**
 * @brief Parse a human-readable heap size from an environment variable.
 *
 * Supported suffixes (case-insensitive): K (KiB), M (MiB), G (GiB).
 * Examples: "512K", "4M", "2G".
 *
 * @param env_var     Name of the environment variable to read.
 * @param default_val Returned when the variable is absent, empty, or invalid.
 */
[[nodiscard]] inline std::size_t parse_heap_env_var(
    const char* env_var,
    std::size_t default_val) noexcept {
    const char* value = std::getenv(env_var);
    if (value == nullptr || value[0] == '\0') return default_val;

    char* end = nullptr;
    errno = 0;
    const unsigned long long raw = std::strtoull(value, &end, 10);
    if (end == value || errno == ERANGE || raw == 0) return default_val;

    std::uint64_t multiplier = 1;
    if (end != nullptr && *end != '\0') {
        switch (*end) {
            case 'K':
            case 'k':
                multiplier = 1024ULL;
                ++end;
                break;
            case 'M':
            case 'm':
                multiplier = 1024ULL * 1024ULL;
                ++end;
                break;
            case 'G':
            case 'g':
                multiplier = 1024ULL * 1024ULL * 1024ULL;
                ++end;
                break;
            default:
                break;
        }
        while (*end != '\0' && std::isspace(static_cast<unsigned char>(*end)) != 0) ++end;
        if (*end != '\0') return default_val;
    }

    const std::uint64_t result = static_cast<std::uint64_t>(raw) * multiplier;
    constexpr std::uint64_t kSizeTMax = static_cast<std::uint64_t>(~std::size_t{0});
    if (result > kSizeTMax) return default_val;
    return static_cast<std::size_t>(result);
}

} // namespace eta::session
