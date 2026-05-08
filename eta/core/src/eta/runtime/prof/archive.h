#pragma once

#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

#include "eta/runtime/prof/aggregator.h"
#include "eta/runtime/prof/frame_id.h"
#include "eta/runtime/prof/speedscope.h"

namespace eta::runtime::prof {

/**
 * @brief Archive payload mode.
 */
enum class ArchiveMode : std::uint8_t {
    Trace,
    Sample,
};

/**
 * @brief One flat-row archive entry keyed by dense frame id.
 */
struct ArchiveFlatRow {
    FrameId frame_id{0};
    FlatStats stats{};
};

/**
 * @brief One tree-row archive entry keyed by dense frame ids.
 */
struct ArchiveTreeRow {
    FrameId parent_frame_id{0};
    FrameId child_frame_id{0};
    EdgeStats stats{};
};

/**
 * @brief In-memory representation of an `eta-prof` archive.
 */
struct ArchiveSession {
    static constexpr std::uint32_t kVersion = 1;

    ArchiveMode mode{ArchiveMode::Trace};
    std::vector<FrameKey> frames;
    std::vector<ArchiveFlatRow> flat_rows;
    std::vector<ArchiveTreeRow> tree_rows;
    std::vector<SpeedscopeThreadProfile> sample_profiles;
    std::unordered_map<std::string, std::uint64_t> counters;
};

/**
 * @brief Convert archive mode to text.
 */
[[nodiscard]] const char* to_string(ArchiveMode mode) noexcept;

/**
 * @brief Parse archive mode from text.
 */
[[nodiscard]] std::expected<ArchiveMode, std::string> parse_archive_mode(std::string_view text);

/**
 * @brief Serialize archive payload to `eta-prof` JSON.
 */
[[nodiscard]] std::string write_eta_prof_archive(const ArchiveSession& session);

/**
 * @brief Parse `eta-prof` JSON into an archive payload.
 */
[[nodiscard]] std::expected<ArchiveSession, std::string> parse_eta_prof_archive(std::string_view input);

/**
 * @brief Merge archive payloads into one combined archive.
 */
[[nodiscard]] std::expected<ArchiveSession, std::string> merge_eta_prof_archives(
    std::span<const ArchiveSession> sessions);

/**
 * @brief Render a pretty report from an archive payload.
 */
[[nodiscard]] std::string render_pretty_archive_report(
    const ArchiveSession& session,
    std::size_t top_n = 20);

/**
 * @brief Render a JSON report from an archive payload.
 */
[[nodiscard]] std::string render_json_archive_report(
    const ArchiveSession& session,
    std::size_t top_n = 20);

/**
 * @brief Render speedscope JSON from an archive payload.
 */
[[nodiscard]] std::string render_speedscope_archive_report(const ArchiveSession& session);

/**
 * @brief Render Chrome trace JSON from an archive payload.
 */
[[nodiscard]] std::string render_chrome_archive_report(const ArchiveSession& session);

} ///< namespace eta::runtime::prof
