#pragma once

#include <cstdint>

#include "eta/runtime/nanbox.h"

namespace eta::runtime::types::tape_ref {

    using eta::runtime::nanbox::LispVal;
    using eta::runtime::nanbox::Tag;
    namespace ops = eta::runtime::nanbox::ops;

    /**
     * @brief Decoded TapeRef payload fields.
     */
    struct Parts {
        uint32_t tape_id{0};
        uint32_t generation{0};
        uint32_t node_index{0};
    };

    /**
     * TapeRef payload layout (47 bits total):
     *
     * [ tape-id:13 | generation:10 | node-index:24 ]
     */
    constexpr uint32_t NODE_INDEX_BITS = 24;
    constexpr uint32_t GENERATION_BITS = 10;
    constexpr uint32_t TAPE_ID_BITS = 13;

    constexpr uint32_t NODE_INDEX_SHIFT = 0;
    constexpr uint32_t GENERATION_SHIFT = NODE_INDEX_BITS;
    constexpr uint32_t TAPE_ID_SHIFT = NODE_INDEX_BITS + GENERATION_BITS;

    constexpr uint64_t NODE_INDEX_MASK = (1ull << NODE_INDEX_BITS) - 1ull;
    constexpr uint64_t GENERATION_MASK = (1ull << GENERATION_BITS) - 1ull;
    constexpr uint64_t TAPE_ID_MASK = (1ull << TAPE_ID_BITS) - 1ull;

    constexpr uint32_t MAX_NODE_INDEX = static_cast<uint32_t>(NODE_INDEX_MASK);
    constexpr uint32_t MAX_GENERATION = static_cast<uint32_t>(GENERATION_MASK);
    constexpr uint32_t MAX_TAPE_ID = static_cast<uint32_t>(TAPE_ID_MASK);

    constexpr uint32_t normalize_generation(uint32_t generation) {
        if (generation == 0) return 1;
        if (generation > MAX_GENERATION) {
            return static_cast<uint32_t>((generation % MAX_GENERATION) + 1);
        }
        return generation;
    }

    constexpr uint32_t next_generation(uint32_t generation) {
        const uint32_t current = normalize_generation(generation);
        return (current >= MAX_GENERATION) ? 1u : (current + 1u);
    }

    constexpr uint64_t encode_payload(uint32_t tape_id, uint32_t generation, uint32_t node_index) {
        return ((static_cast<uint64_t>(tape_id) & TAPE_ID_MASK) << TAPE_ID_SHIFT)
            | ((static_cast<uint64_t>(normalize_generation(generation)) & GENERATION_MASK) << GENERATION_SHIFT)
            | ((static_cast<uint64_t>(node_index) & NODE_INDEX_MASK) << NODE_INDEX_SHIFT);
    }

    constexpr Parts decode_payload(uint64_t payload) {
        Parts out{};
        out.tape_id = static_cast<uint32_t>((payload >> TAPE_ID_SHIFT) & TAPE_ID_MASK);
        out.generation = static_cast<uint32_t>((payload >> GENERATION_SHIFT) & GENERATION_MASK);
        out.node_index = static_cast<uint32_t>((payload >> NODE_INDEX_SHIFT) & NODE_INDEX_MASK);
        out.generation = normalize_generation(out.generation);
        return out;
    }

    inline bool is_tape_ref(LispVal value) {
        return ops::is_boxed(value) && ops::tag(value) == Tag::TapeRef;
    }

    inline LispVal make(uint32_t tape_id, uint32_t generation, uint32_t node_index) {
        return ops::box(Tag::TapeRef, encode_payload(tape_id, generation, node_index));
    }

    inline Parts decode(LispVal value) {
        return decode_payload(ops::payload(value));
    }

}  // namespace eta::runtime::types::tape_ref
