#include "eta/runtime/prof/aggregator.h"

namespace eta::runtime::prof {

std::size_t EdgeKeyHash::operator()(const EdgeKey& key) const noexcept {
    const auto p = static_cast<std::size_t>(key.parent);
    const auto c = static_cast<std::size_t>(key.child);
    return (p << 1) ^ (c + 0x9e3779b97f4a7c15ULL + (p << 6) + (p >> 2));
}

void Aggregator::record_flat(const FrameId frame_id,
                             const std::uint64_t self_ns,
                             const std::uint64_t inclusive_ns,
                             const std::uint64_t calls,
                             const std::uint64_t bytes_allocated) {
    std::lock_guard lock(mutex_);
    auto& slot = flat_[frame_id];
    slot.self_ns += self_ns;
    slot.inclusive_ns += inclusive_ns;
    slot.calls += calls;
    slot.bytes_allocated += bytes_allocated;
}

void Aggregator::record_alloc(const FrameId frame_id, const std::uint64_t bytes_allocated) {
    if (bytes_allocated == 0) return;
    std::lock_guard lock(mutex_);
    auto& slot = flat_[frame_id];
    slot.bytes_allocated += bytes_allocated;
}

void Aggregator::record_edge(const FrameId parent,
                             const FrameId child,
                             const std::uint64_t inclusive_ns,
                             const std::uint64_t calls) {
    std::lock_guard lock(mutex_);
    auto& slot = tree_[EdgeKey{parent, child}];
    slot.inclusive_ns += inclusive_ns;
    slot.calls += calls;
}

void Aggregator::clear() {
    std::lock_guard lock(mutex_);
    flat_.clear();
    tree_.clear();
}

std::unordered_map<FrameId, FlatStats> Aggregator::flat_snapshot() const {
    std::lock_guard lock(mutex_);
    return flat_;
}

std::unordered_map<EdgeKey, EdgeStats, EdgeKeyHash> Aggregator::tree_snapshot() const {
    std::lock_guard lock(mutex_);
    return tree_;
}

} ///< namespace eta::runtime::prof
