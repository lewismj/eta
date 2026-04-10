#include "heap.h"
#include "cons_pool.h"

namespace eta::runtime::memory::heap {

    // ── construction / destruction ───────────────────────────────

    Heap::Heap(const size_t max_heap_soft_limit)
        : max_heap_soft_limit_(max_heap_soft_limit)
    {
        // ConsPool claims a contiguous ID range from next_id.
        // 8192 initial slots ≈ 256 KB of slab memory.
        cons_pool_ = std::make_unique<ConsPool>(8192, next_id);
    }

    Heap::~Heap() {
        // Pool entries are POD (types::Cons) — no destructors to call.
        // The unique_ptr<ConsPool> destructor frees slab memory automatically.

        // General-heap entries may have non-trivial destructors.
        for (auto& [heap_objects, stats] : shards) {
            heap_objects.visit_all([](const auto& kv) {
                if (const auto& entry = kv.second; entry.ptr && entry.destructor) {
                    try {
                        entry.destructor(entry.ptr);
                    } catch (...) {
                        // Policy: never allow stored destructors to invoke std::terminate
                    }
                }
            });
        }
    }

    // ── static helpers ──────────────────────────────────────────

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

    // ── pool-accelerated cons allocation ────────────────────────

    std::expected<ObjectId, HeapError> Heap::alloc_cons(LispVal car, LispVal cdr) {
        // Reject allocations during GC
        [[unlikely]] if (gc_in_progress_.load(std::memory_order_acquire)) {
            return std::unexpected(HeapError::GCInProgress);
        }

        // Soft-limit check (same logic as the general allocate path)
        const auto current_total = total_heap_bytes.load(std::memory_order_relaxed);
        if (current_total + sizeof(types::Cons) > max_heap_soft_limit_) {
            if (gc_callback_) {
                gc_callback_();
                if (total_heap_bytes.load(std::memory_order_relaxed) + sizeof(types::Cons) > max_heap_soft_limit_) {
                    return std::unexpected(HeapError::SoftHeapLimitExceeded);
                }
            } else {
                return std::unexpected(HeapError::SoftHeapLimitExceeded);
            }
        }

        auto result = cons_pool_->alloc(car, cdr);
        if (result.has_value()) {
            total_heap_bytes.fetch_add(sizeof(types::Cons), std::memory_order_relaxed);
        }
        return result;
    }

    // ── deallocation (pool-aware) ───────────────────────────────

    std::expected<void, HeapError> Heap::deallocate(const ObjectId id) {
        // Fast path: pool-owned cons cell
        if (cons_pool_ && cons_pool_->owns(id)) {
            if (!cons_pool_->try_get(id)) {
                return std::unexpected(HeapError::ObjectIdNotFound);
            }
            cons_pool_->free_slot(id);
            total_heap_bytes.fetch_sub(sizeof(types::Cons), std::memory_order_relaxed);
            return {};
        }

        // General-heap path (unchanged)
        const size_t shard_index = select_shard(id);
        auto& [heap_objects, stats] = shards[shard_index];

        HeapEntry entry;
        const bool found = heap_objects.visit(id, [&](const auto& kv) {
            entry = kv.second;
        });

        [[unlikely]] if (!found) {
            return std::unexpected(HeapError::ObjectIdNotFound);
        }

        [[unlikely]] if (entry.ptr == nullptr) {
            return std::unexpected(HeapError::NullPtrReference);
        }

        // Update stats before removal
        stats.num_objects.fetch_sub(1, std::memory_order_relaxed);
        stats.heap_bytes.fetch_sub(entry.size, std::memory_order_relaxed);
        total_heap_bytes.fetch_sub(entry.size, std::memory_order_relaxed);

        // CRITICAL: Erase from map BEFORE calling destructor to prevent
        // other threads from accessing freed memory via try_get()
        heap_objects.erase(id);

        // Now safe to destroy - no other thread can find this entry
        try {
            if (entry.destructor) {
                entry.destructor(entry.ptr);
            } else {
                ::operator delete(entry.ptr);
            }
        } catch (...) {
            // Destructor threw, but entry is already removed from map.
            // Memory may be leaked, but we maintain safety.
            return std::unexpected(HeapError::FailedToDeallocateMemory);
        }

        return {};
    }

    // ── iteration (pool + general) ──────────────────────────────

    void Heap::for_each_entry(const std::function<void(ObjectId, HeapEntry&)>& fn) {
        // Pool entries first (dense array walk)
        if (cons_pool_) {
            cons_pool_->for_each_live_entry(fn);
        }

        // Then general-heap entries
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

    // ── lookup (pool-aware) ─────────────────────────────────────

    bool Heap::try_get(ObjectId id, HeapEntry& out) const {
        // Fast path: pool-owned cons cell
        if (cons_pool_ && cons_pool_->owns(id)) {
            return cons_pool_->try_get_entry(id, out);
        }

        // General-heap path
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
        // Fast path: pool-owned cons cell
        if (cons_pool_ && cons_pool_->owns(id)) {
            return cons_pool_->with_entry(id, fn);
        }

        // General-heap path
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

    // ── pool accessor ───────────────────────────────────────────

    ConsPool& Heap::cons_pool() { return *cons_pool_; }
    const ConsPool& Heap::cons_pool() const { return *cons_pool_; }

    // ── pool sweep (updates total_heap_bytes) ────────────────────

    std::size_t Heap::sweep_cons_pool() {
        auto freed = cons_pool_->sweep();
        total_heap_bytes.fetch_sub(freed * sizeof(types::Cons), std::memory_order_relaxed);
        return freed;
    }
}
