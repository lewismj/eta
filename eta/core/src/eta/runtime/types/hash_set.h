#pragma once

#include <cstddef>
#include <cstdint>

#include <eta/runtime/types/hash_map.h>

namespace eta::runtime::types {

    /**
     * Immutable hash-set payload.
     *
     * Uses the same open-addressing table as HashMap; only keys are semantically
     * relevant to Eta code.
     */
    struct HashSet {
        HashMap table{};
    };

    inline HashSet make_empty_hash_set(const std::size_t requested_capacity, const std::uint64_t seed) {
        HashSet out;
        out.table = make_empty_hash_map(requested_capacity, seed);
        return out;
    }
}

