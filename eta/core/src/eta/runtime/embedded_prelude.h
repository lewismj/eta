#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

namespace eta::runtime {

#if defined(ETA_HAS_EMBEDDED_PRELUDE)
extern const std::uint8_t eta_embedded_prelude[];
extern const std::size_t eta_embedded_prelude_size;
#endif

/**
 * @brief Return the embedded prelude bytecode blob for this binary.
 *
 * When embedding is disabled for a target, this returns an empty span.
 */
[[nodiscard]] inline std::span<const std::uint8_t> embedded_prelude_blob() noexcept {
#if defined(ETA_HAS_EMBEDDED_PRELUDE)
    return {eta_embedded_prelude, eta_embedded_prelude_size};
#else
    return {};
#endif
}

} ///< namespace eta::runtime

