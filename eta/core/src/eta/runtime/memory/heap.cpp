#include "heap.h"

namespace eta::runtime::memory::heap {

    uint64_t Heap::split_mix_64(uint64_t x) noexcept {
        x += 0x9e3779b97f4a7c15ULL;
        x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
        x = (x ^ (x >> 27)) * 0x94d049bb133111ebULL;
        x ^= (x >> 31);
        return x;
    }

    size_t Heap::select_shard(ObjectId id) noexcept {
        const uint64_t x = split_mix_64(id);
        return x & (NUM_SHARDS - 1);
    }

    void Heap::for_each_entry(const std::function<void(ObjectId, HeapEntry&)>& fn) {
        for (auto& shard : shards) {
            auto& map = shard.heap_objects;
            map.visit_all([&](const auto& kv) {
                const ObjectId id = kv.first;
                // Provide mutable access to the value without re-visiting by key
                auto& entry = const_cast<HeapEntry&>(kv.second);
                fn(id, entry);
            });
        }
    }

    bool Heap::try_get(ObjectId id, HeapEntry& out) const {
        const size_t shard_index = select_shard(id);
        const auto& [heap_objects, stats] = shards[shard_index];
        const auto& map = heap_objects;
        bool found = false;
        map.visit(id, [&](const auto& kv) {
            out = kv.second;
            found = true;
        });
        return found;
    }

    bool Heap::with_entry(ObjectId id, const std::function<void(HeapEntry&)>& fn) {
        const size_t shard_index = select_shard(id);
        auto& [heap_objects, stats] = shards[shard_index];
        auto& map = heap_objects;
        bool found = false;
        map.visit(id, [&](auto& kv) {
            fn(kv.second);
            found = true;
        });
        return found;
    }
}
