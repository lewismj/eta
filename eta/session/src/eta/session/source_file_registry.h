/**
 * @file source_file_registry.h
 * @brief Source-file identifier registry used by diagnostics and debugging.
 */

#pragma once

#include <cstdint>
#include <filesystem>
#include <set>
#include <string>
#include <unordered_map>

#include "eta/diagnostic/diagnostic.h"

namespace eta::semantics {
class BytecodeFunctionRegistry;
}

namespace eta::session {

/**
 * @brief Tracks source-file IDs and lookup metadata for session diagnostics.
 */
class SourceFileRegistry {
public:
    /**
     * @brief Build a resolver callback that maps file IDs to file names.
     */
    [[nodiscard]] diagnostic::FileResolver file_resolver() const;

    /**
     * @brief Return the stored path for a file ID, or nullptr if unknown.
     */
    [[nodiscard]] const std::filesystem::path* path_for_file_id(uint32_t id) const noexcept;

    /**
     * @brief Ensure a path has a file ID and return that ID.
     */
    uint32_t ensure_file_id(const std::filesystem::path& path);

    /**
     * @brief Look up a file ID from a path, returning 0 when not registered.
     */
    [[nodiscard]] uint32_t file_id_for_path(const std::string& path) const;

    /**
     * @brief Collect executable source lines associated with a file.
     */
    [[nodiscard]] std::set<uint32_t> valid_lines_for(
        uint32_t file_id,
        const semantics::BytecodeFunctionRegistry& registry) const;

    /**
     * @brief Allocate or return the file ID for a raw path string.
     */
    uint32_t allocate_file_id(const std::string& raw_path);

private:
    uint32_t next_file_id_{1}; ///< 0 is reserved for REPL / anonymous input.
    std::unordered_map<uint32_t, std::filesystem::path> file_id_to_path_;
    std::unordered_map<std::string, uint32_t> path_to_file_id_;
};

} // namespace eta::session
