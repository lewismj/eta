#pragma once

#include <chrono>
#include <cstdint>

namespace eta::runtime::prof {

/**
 * @brief Profiler wall-clock source used by runtime instrumentation.
 *
 * The profiler uses a monotonic clock for interval measurements.
 */
using ProfilerClock = std::chrono::steady_clock;

/**
 * @brief Return current monotonic timestamp in nanoseconds.
 */
[[nodiscard]] inline std::uint64_t now_ns() noexcept {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            ProfilerClock::now().time_since_epoch())
            .count());
}

} ///< namespace eta::runtime::prof

