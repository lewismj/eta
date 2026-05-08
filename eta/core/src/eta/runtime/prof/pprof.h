#pragma once

#include <expected>
#include <string>

#include "eta/runtime/prof/archive.h"

namespace eta::runtime::prof {

/**
 * @brief Render a pprof payload when pprof support is enabled.
 */
[[nodiscard]] std::expected<std::string, std::string> write_pprof_profile(
    const ArchiveSession& session);

} ///< namespace eta::runtime::prof

