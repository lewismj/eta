/// String, path and filesystem helpers shared by the CLI.

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <limits>
#include <regex>
#include <sstream>
#include <system_error>

#include "eta/cli/cli_internal.h"
#include "eta/util/path.h"

namespace eta::cli {

[[nodiscard]] bool is_valid_package_name(std::string_view value) {
    static const std::regex kPattern("^[a-z][a-z0-9_-]{0,63}$");
    return std::regex_match(value.begin(), value.end(), kPattern);
}

[[nodiscard]] std::string module_name_from_package_name(std::string_view package_name) {
    std::string module_name(package_name);
    std::replace(module_name.begin(), module_name.end(), '-', '_');
    return module_name;
}

[[nodiscard]] std::string lower_ascii(std::string_view text) {
    std::string lowered(text);
    std::transform(lowered.begin(),
                   lowered.end(),
                   lowered.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return lowered;
}

[[nodiscard]] fs::path canonicalize_path(const fs::path& path) {
    return eta::util::canonicalize_path(path);
}

[[nodiscard]] std::string path_key(const fs::path& path) {
    return eta::util::canonical_path_key(path);
}

[[nodiscard]] bool is_path_within(const fs::path& root, const fs::path& candidate) {
    const auto root_key = path_key(root);
    const auto candidate_key = path_key(candidate);
    if (candidate_key == root_key) return true;
    if (candidate_key.size() <= root_key.size()) return false;
    if (candidate_key.compare(0u, root_key.size(), root_key) != 0) return false;
    if (!root_key.empty() && (root_key.back() == '/' || root_key.back() == '\\')) return true;
    const char boundary = candidate_key[root_key.size()];
    return boundary == '/' || boundary == '\\';
}

[[nodiscard]] uint64_t fnv1a_64(std::string_view data) {
    constexpr uint64_t kOffset = 1469598103934665603ULL;
    constexpr uint64_t kPrime = 1099511628211ULL;
    uint64_t value = kOffset;
    for (const unsigned char c : data) {
        value ^= static_cast<uint64_t>(c);
        value *= kPrime;
    }
    return value;
}

[[nodiscard]] std::string to_hex_u64(uint64_t value) {
    std::ostringstream out;
    out << std::hex << std::nouppercase;
    out.width(16);
    out.fill('0');
    out << value;
    return out.str();
}

[[nodiscard]] bool looks_like_hex(std::string_view value, std::size_t expected_len) {
    if (value.size() != expected_len) return false;
    for (const char c : value) {
        if (!std::isxdigit(static_cast<unsigned char>(c))) return false;
    }
    return true;
}

[[nodiscard]] std::optional<fs::path> home_dir() {
#ifdef _WIN32
    const char* user_profile = std::getenv("USERPROFILE");
    if (user_profile != nullptr && user_profile[0] != '\0') {
        return fs::path(user_profile);
    }
    const char* home_drive = std::getenv("HOMEDRIVE");
    const char* home_path = std::getenv("HOMEPATH");
    if (home_drive != nullptr && home_path != nullptr
        && home_drive[0] != '\0' && home_path[0] != '\0') {
        return fs::path(std::string(home_drive) + std::string(home_path));
    }
#else
    const char* home = std::getenv("HOME");
    if (home != nullptr && home[0] != '\0') return fs::path(home);
#endif
    return std::nullopt;
}

[[nodiscard]] CliResult<fs::path> eta_cache_modules_root() {
    auto home = home_dir();
    if (!home.has_value()) {
        return std::unexpected("failed to resolve user home directory");
    }
    return canonicalize_path(*home / ".eta" / "cache" / "modules");
}

ErrorResult ensure_directory(const fs::path& dir) {
    std::error_code ec;
    fs::create_directories(dir, ec);
    if (ec) {
        return std::unexpected("failed to create directory '" + dir.string() + "': " + ec.message());
    }
    return {};
}

ErrorResult remove_path_if_exists(const fs::path& path) {
    std::error_code ec;
    if (!fs::exists(path, ec)) return {};
    if (ec) return std::unexpected("failed to stat path '" + path.string() + "': " + ec.message());

    fs::remove_all(path, ec);
    if (ec) {
        return std::unexpected("failed to remove path '" + path.string() + "': " + ec.message());
    }
    return {};
}

ErrorResult copy_directory_tree(const fs::path& source, const fs::path& destination) {
    std::error_code ec;
    if (!fs::is_directory(source, ec) || ec) {
        return std::unexpected("source directory does not exist: " + source.string());
    }

    if (auto rm = remove_path_if_exists(destination); !rm) return rm;
    if (auto mk = ensure_directory(destination.parent_path()); !mk) return mk;

    fs::copy(source,
             destination,
             fs::copy_options::recursive
                 | fs::copy_options::copy_symlinks
                 | fs::copy_options::overwrite_existing,
             ec);
    if (ec) {
        return std::unexpected("failed to copy '" + source.string() + "' to '"
                               + destination.string() + "': " + ec.message());
    }
    return {};
}

ErrorResult materialize_from_cache(const fs::path& cache_dir, const fs::path& destination) {
    if (auto rm = remove_path_if_exists(destination); !rm) return rm;
    if (auto mk = ensure_directory(destination.parent_path()); !mk) return mk;

    std::error_code ec;
    fs::create_directory_symlink(cache_dir, destination, ec);
    if (!ec) return {};

    return copy_directory_tree(cache_dir, destination);
}

[[nodiscard]] CliResult<std::size_t> parse_depth(std::string_view value) {
    if (value.empty()) return std::unexpected("depth must be a non-negative integer");
    std::size_t parsed = 0;
    for (const char c : value) {
        if (!std::isdigit(static_cast<unsigned char>(c))) {
            return std::unexpected("depth must be a non-negative integer");
        }
        parsed = parsed * 10u + static_cast<std::size_t>(c - '0');
    }
    return parsed;
}

[[nodiscard]] CliResult<std::uint32_t> parse_positive_u32(std::string_view value,
                                                          std::string_view field_name) {
    if (value.empty()) {
        return std::unexpected(std::string(field_name) + " must be a positive integer");
    }
    std::uint64_t parsed = 0;
    for (const char c : value) {
        if (!std::isdigit(static_cast<unsigned char>(c))) {
            return std::unexpected(std::string(field_name) + " must be a positive integer");
        }
        parsed = parsed * 10u + static_cast<std::uint64_t>(c - '0');
        if (parsed > static_cast<std::uint64_t>((std::numeric_limits<std::uint32_t>::max)())) {
            return std::unexpected(std::string(field_name) + " is too large");
        }
    }
    if (parsed == 0u) {
        return std::unexpected(std::string(field_name) + " must be a positive integer");
    }
    return static_cast<std::uint32_t>(parsed);
}

} ///< namespace eta::cli
