#pragma once

#include <expected>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace eta::package {

namespace fs = std::filesystem;

/**
 * @brief Supported manifest dependency source kinds.
 */
enum class ManifestDependencyKind {
    Path,
    Git,
    Tarball,
};

/**
 * @brief Dependency declared in `[dependencies]` or `[dev-dependencies]`.
 */
struct ManifestDependency {
    std::string name;
    ManifestDependencyKind kind{ManifestDependencyKind::Path};
    fs::path path;
    std::string git;
    std::string rev;
    std::string tarball;
    std::string sha256;
};

/**
 * @brief Parsed package manifest (`eta.toml`) fields required in S1.
 */
struct Manifest {
    fs::path manifest_path;
    std::string name;
    std::string version;
    std::string license;
    std::string compatibility_eta;
    std::vector<ManifestDependency> dependencies;
    std::vector<ManifestDependency> dev_dependencies;
};

/**
 * @brief Manifest parser/validator error.
 */
struct ManifestError {
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

using ManifestResult = std::expected<Manifest, ManifestError>;

/**
 * @brief Parse an `eta.toml` payload.
 *
 * The parser accepts the packaging subset:
 * - `[package]`: `name`, `version`, `license`
 * - `[compatibility]`: `eta`
 * - `[dependencies]` / `[dev-dependencies]`:
 *   - `name = { path = "../dep" }`
 *   - `name = { git = "...", rev = "<40-hex>" }`
 *   - `name = { tarball = "...", sha256 = "<64-hex>" }`
 */
ManifestResult parse_manifest(std::string_view text,
                              const fs::path& source_path = {});

/**
 * @brief Read and parse `eta.toml` from disk.
 */
ManifestResult read_manifest(const fs::path& manifest_path);

/**
 * @brief Serialize a manifest in deterministic key/dependency order.
 */
std::string write_manifest(const Manifest& manifest);

/**
 * @brief Serialize a manifest and write it to disk.
 */
std::expected<void, ManifestError> write_manifest_file(const Manifest& manifest,
                                                       const fs::path& manifest_path);

} // namespace eta::package
