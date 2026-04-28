#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include <eta/runtime/nanbox.h>

namespace eta::runtime::types {
    using namespace eta::runtime::nanbox;

    enum class HashSlotState : std::uint8_t {
        Empty = 0,
        Occupied = 1,
        Tombstone = 2,
    };

    /**
     * Immutable hash-map payload stored on the Eta heap.
     *
     * `keys`, `values`, and `state` are parallel arrays of equal length.
     * Occupied slots hold one key/value pair; empty and tombstone slots are ignored.
     */
    struct HashMap {
        std::vector<LispVal> keys{};
        std::vector<LispVal> values{};
        std::vector<std::uint8_t> state{};
        std::size_t size{0};
        std::size_t tombstones{0};
        std::uint64_t seed{0};
    };

    inline std::size_t hash_table_capacity(std::size_t requested) {
        std::size_t cap = requested < 8 ? 8 : requested;
        std::size_t pow2 = 1;
        while (pow2 < cap) pow2 <<= 1;
        return pow2;
    }

    inline HashMap make_empty_hash_map(const std::size_t requested_capacity, const std::uint64_t seed) {
        const auto cap = hash_table_capacity(requested_capacity);
        HashMap out;
        out.keys.resize(cap, Nil);
        out.values.resize(cap, Nil);
        out.state.resize(cap, static_cast<std::uint8_t>(HashSlotState::Empty));
        out.seed = seed;
        return out;
    }

    inline bool hash_slot_occupied(const HashMap& map, const std::size_t index) {
        return index < map.state.size()
            && map.state[index] == static_cast<std::uint8_t>(HashSlotState::Occupied);
    }
}

