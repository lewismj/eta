#pragma once

#include <string>
#include <vector>

#include "eta/runtime/prof/frame_id.h"
#include "eta/runtime/prof/speedscope.h"

namespace eta::runtime::prof {

/**
 * @brief Render a Chrome trace JSON document from sampled stacks.
 */
[[nodiscard]] std::string write_chrome_trace_json(
    const FrameIdInterner& interner,
    const std::vector<SpeedscopeThreadProfile>& profiles);

} ///< namespace eta::runtime::prof

