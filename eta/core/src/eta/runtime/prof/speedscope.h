#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "eta/runtime/prof/frame_id.h"

namespace eta::runtime::prof {

/**
 * @brief One sampled profile stream for speedscope export.
 */
struct SpeedscopeThreadProfile {
    std::string name;
    std::vector<std::uint64_t> timestamps_ns;
    std::vector<std::vector<FrameId>> samples;
};

/**
 * @brief Serialize profiler samples into a speedscope-compatible JSON string.
 */
[[nodiscard]] std::string write_speedscope_json(
    const FrameIdInterner& interner,
    const std::vector<SpeedscopeThreadProfile>& profiles);

} ///< namespace eta::runtime::prof

