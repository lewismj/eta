#pragma once

#include <cstdint>
#include <mutex>
#include <unordered_map>

#include "eta/runtime/prof/frame_id.h"

namespace eta::runtime::prof {

/**
 * @brief Flat per-frame aggregate counters.
 */
struct FlatStats {
    std::uint64_t self_ns{0};
    std::uint64_t inclusive_ns{0};
    std::uint64_t calls{0};
    std::uint64_t bytes_allocated{0};
};

/**
 * @brief Parent->child call edge aggregate counters.
 */
struct EdgeStats {
    std::uint64_t inclusive_ns{0};
    std::uint64_t calls{0};
};

/**
 * @brief Parent->child key for tree aggregation.
 */
struct EdgeKey {
    FrameId parent{0};
    FrameId child{0};

    [[nodiscard]] bool operator==(const EdgeKey& other) const noexcept {
        return parent == other.parent && child == other.child;
    }
};

/**
 * @brief Hash functor for EdgeKey.
 */
struct EdgeKeyHash {
    [[nodiscard]] std::size_t operator()(const EdgeKey& key) const noexcept;
};

/**
 * @brief Shared aggregate model for profiler reports.
 */
class Aggregator {
public:
    void record_flat(FrameId frame_id,
                     std::uint64_t self_ns,
                     std::uint64_t inclusive_ns,
                     std::uint64_t calls,
                     std::uint64_t bytes_allocated = 0);
    void record_alloc(FrameId frame_id, std::uint64_t bytes_allocated);
    void record_edge(FrameId parent, FrameId child, std::uint64_t inclusive_ns, std::uint64_t calls);
    void clear();

    [[nodiscard]] std::unordered_map<FrameId, FlatStats> flat_snapshot() const;
    [[nodiscard]] std::unordered_map<EdgeKey, EdgeStats, EdgeKeyHash> tree_snapshot() const;

private:
    mutable std::mutex mutex_;
    std::unordered_map<FrameId, FlatStats> flat_;
    std::unordered_map<EdgeKey, EdgeStats, EdgeKeyHash> tree_;
};

} ///< namespace eta::runtime::prof
