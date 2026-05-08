#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>

#include "eta/runtime/prof/aggregator.h"
#include "eta/runtime/prof/frame_id.h"

namespace eta::runtime::prof {

/**
 * @brief Render a textual top-N profiler summary.
 */
[[nodiscard]] std::string render_pretty_report(
    const Aggregator& aggregator,
    const FrameIdInterner& interner,
    const std::unordered_map<std::string, std::uint64_t>& counters,
    std::size_t top_n = 20);

/**
 * @brief Render a JSON profiler summary.
 */
[[nodiscard]] std::string render_json_report(
    const Aggregator& aggregator,
    const FrameIdInterner& interner,
    const std::unordered_map<std::string, std::uint64_t>& counters,
    std::size_t top_n = 20);

} ///< namespace eta::runtime::prof
