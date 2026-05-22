#pragma once

#include <cstdint>

namespace eta::runtime::types {

/**
 * @brief Actor process identifier.
 *
 * `node_id` remains zero for local-only actor systems.
 */
struct Pid {
    std::uint64_t node_id{0};
    std::uint64_t actor_id{0};
    std::uint32_t incarnation{0};
};

[[nodiscard]] inline bool operator==(const Pid& lhs, const Pid& rhs) noexcept {
    return lhs.node_id == rhs.node_id
        && lhs.actor_id == rhs.actor_id
        && lhs.incarnation == rhs.incarnation;
}

[[nodiscard]] inline bool operator!=(const Pid& lhs, const Pid& rhs) noexcept {
    return !(lhs == rhs);
}

} // namespace eta::runtime::types
