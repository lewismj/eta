#pragma once

#include <expected>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace eta::package {

namespace fs = std::filesystem;

/**
 * @brief One dependency edge in a locked package entry.
 */
struct LockedDependency {
    std::string name;
    std::string version;
};

/**
 * @brief One `[[package]]` row in `eta.lock`.
 */
struct LockfilePackage {
    std::string name;
    std::string version;
    std::string source;
    std::vector<LockedDependency> dependencies;
};

/**
 * @brief Parsed lockfile payload.
 */
struct Lockfile {
    int schema_version{1};
    std::vector<LockfilePackage> packages;
};

/**
 * @brief Lockfile parser/validator error.
 */
struct LockfileError {
    enum class Code {
        IoError,
        ParseError,
        MissingRequiredField,
        InvalidValue,
        UnsupportedValue,
    };

    Code code{Code::ParseError};
    std::string message;
    std::size_t line{0};
};

using LockfileResult = std::expected<Lockfile, LockfileError>;

/**
 * @brief Parse lockfile TOML from a string.
 */
LockfileResult parse_lockfile(std::string_view text);

/**
 * @brief Read lockfile TOML from disk.
 */
LockfileResult read_lockfile(const fs::path& lockfile_path);

/**
 * @brief Serialize lockfile in deterministic order.
 */
std::string write_lockfile(const Lockfile& lockfile);

/**
 * @brief Serialize lockfile and write to disk.
 */
std::expected<void, LockfileError> write_lockfile_file(const Lockfile& lockfile,
                                                       const fs::path& lockfile_path);

} // namespace eta::package
