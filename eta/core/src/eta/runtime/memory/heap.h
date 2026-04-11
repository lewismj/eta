#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <expected>
#include <memory>
#include <ostream>
#include <functional>
#include <boost/unordered/concurrent_flat_map.hpp>

#include <eta/arch.h>
#include <eta/runtime/nanbox.h>
#include "enum_utils.h"

namespace eta::runtime::memory::heap {
    using namespace eta::runtime::nanbox;

    using ObjectId = std::uint64_t;

    enum class ObjectKind : std::uint8_t {
        Unknown,
        Cons,
        Fixnum,
        Vector,
        ByteVector,
        Closure,
        Continuation,
        Primitive,
        MultipleValues,  // For (values ...) return
        Port,
        LogicVar,        // Unification logic variable
        Tape,            // AD tape (Wengert list for reverse-mode AD)
        Tensor,          // libtorch tensor (wraps torch::Tensor)
        NNModule,        // libtorch nn::Module (wraps shared_ptr<torch::nn::Module>)
        Optimizer,       // libtorch optimizer (wraps shared_ptr<torch::optim::Optimizer>)
        FactTable,       // Columnar fact table with per-column hash indexes
    };

    ETA_ENUM_TO_STRING_BEGIN(ObjectKind)
        ETA_ENUM_CASE(Unknown)
        ETA_ENUM_CASE(Cons)
        ETA_ENUM_CASE(Fixnum)
        ETA_ENUM_CASE(Vector)
        ETA_ENUM_CASE(ByteVector)
        ETA_ENUM_CASE(Closure)
        ETA_ENUM_CASE(Continuation)
        ETA_ENUM_CASE(Primitive)
        ETA_ENUM_CASE(MultipleValues)
        ETA_ENUM_CASE(Port)
        ETA_ENUM_CASE(LogicVar)
        ETA_ENUM_CASE(Tape)
        ETA_ENUM_CASE(Tensor)
        ETA_ENUM_CASE(NNModule)
        ETA_ENUM_CASE(Optimizer)
        ETA_ENUM_CASE(FactTable)
    ETA_ENUM_TO_STRING_END("Unknown")

    inline std::ostream& operator<<(std::ostream& os, const ObjectKind k) {
        return os << to_string(k);
    }

    enum class HeapError : std::uint8_t {
        HeapObjectIdExhausted,
        FailedToAllocateMemory,
        FailedToDeallocateMemory,
        ObjectIdNotFound,
        NullPtrReference,
        SoftHeapLimitExceeded,
        UnexpectedObjectKind,
        GCInProgress,  // Allocation rejected during GC
    };

    constexpr const char* to_string(const HeapError e) noexcept {
        using enum HeapError;
        switch (e) {
            case HeapObjectIdExhausted: return "HeapError::HeapExhausted";
            case FailedToAllocateMemory: return "HeapError::FailedToAllocateMemory";
            case FailedToDeallocateMemory: return "HeapError::FailedToDeallocateMemory";
            case ObjectIdNotFound: return "HeapError::ObjectIdNotFound";
            case NullPtrReference: return "HeapError::NullPtrReference";
            case SoftHeapLimitExceeded: return "HeapError::SoftHeapLimitExceeded";
            case UnexpectedObjectKind: return "HeapError::UnexpectedObjectKind";
            case GCInProgress: return "HeapError::GCInProgress";
            default:
                return "HeapError::Unknown";
        }
    }


    inline std::ostream& operator<<(std::ostream& os, const HeapError e) {
        return os << to_string(e);
    }


    // In-heap GC mark bit lives in ObjectHeader.flags bit 0
    constexpr uint8_t MARK_BIT = 1u << 0;

    struct ObjectHeader {
        ObjectKind kind {};
        std::uint8_t flags : 3;
    };

    struct HeapEntry {
        ObjectHeader header{};
        void* ptr{};
        std::size_t size{};
        void (*destructor)(void*){}; // type-erased destructor
    };

    // Forward-declare — full definition in cons_pool.h
    class ConsPool;

    class Heap {
    public:
        explicit Heap(size_t max_heap_soft_limit);
        ~Heap();

        Heap(const Heap&) = delete;
        Heap& operator=(const Heap&) = delete;
        Heap(Heap&&) = delete;
        Heap& operator=(Heap&&) = delete;

        constexpr static std::size_t NUM_SHARDS = 16;
        static_assert((NUM_SHARDS & (NUM_SHARDS - 1)) == 0, "NUM_SHARDS must be a power of 2");

        template<typename T, ObjectKind Kind, typename ... Args>
        std::expected<ObjectId, HeapError> allocate(Args&&... args) {

            // Reject allocations during GC to prevent unmarked objects being swept
            [[unlikely]] if (gc_in_progress_.load(std::memory_order_acquire)) {
                return std::unexpected(HeapError::GCInProgress);
            }

            //! Highlight unlikley we use over PAYLOAD_MASK number of heap objects.
            //! Anticipate that no process would use over PAYLOAD_MASK during its lifetime.
            auto id = next_id.fetch_add(1, std::memory_order_relaxed);
            [[unlikely]] if (id > nanbox::constants::PAYLOAD_MASK) {
                return std::unexpected(HeapError::HeapObjectIdExhausted);
            }
            const size_t shard_index = select_shard(id);
            auto& [heap_objects, stats] = shards[shard_index];

            // Single atomic load instead of iterating all shards
            const auto current_total = total_heap_bytes.load(std::memory_order_relaxed);
            if (current_total + sizeof(T) > max_heap_soft_limit_) {
                if (gc_callback_) {
                    gc_callback_();
                    if (total_heap_bytes.load(std::memory_order_relaxed) + sizeof(T) > max_heap_soft_limit_) {
                        return std::unexpected(HeapError::SoftHeapLimitExceeded);
                    }
                } else {
                    return std::unexpected(HeapError::SoftHeapLimitExceeded);
                }
            }

            void* memory = nullptr;
            T* obj = nullptr;
            memory = ::operator new(sizeof(T), static_cast<std::align_val_t>(alignof(T)), std::nothrow);
            [[unlikely]] if (!memory) {
                return std::unexpected(HeapError::FailedToAllocateMemory);
            }

            try {
                obj = new (memory) T(std::forward<Args>(args)...);
            }
            catch (...) {
                ::operator delete(memory, static_cast<std::align_val_t>(alignof(T)));
                return std::unexpected(HeapError::FailedToAllocateMemory);
            }

            auto entry = HeapEntry{
                .header = ObjectHeader{
                    .kind = Kind,
                    .flags = 0,
                },
                .ptr = static_cast<void*>(obj),
                .size = sizeof(T),
                .destructor = [](void* p) {
                    auto* typed_ptr = static_cast<T*>(p);
                    typed_ptr->~T();
                    ::operator delete(typed_ptr, static_cast<std::align_val_t>(alignof(T)));
                },
            };

            //! entry is trivally copyable, if that changes, use std::move.
            if (!heap_objects.emplace(id, entry) ) {
                entry.destructor(memory);
                return std::unexpected(HeapError::FailedToAllocateMemory);
            }

            stats.num_objects.fetch_add(1, std::memory_order_relaxed);
            stats.heap_bytes.fetch_add(sizeof(T), std::memory_order_relaxed);
            total_heap_bytes.fetch_add(sizeof(T), std::memory_order_relaxed);
            return id;
        }

        std::expected<void, HeapError> deallocate(ObjectId id);

        // Pool-accelerated cons cell allocation.
        // Performs soft-limit check, then delegates to ConsPool.
        std::expected<ObjectId, HeapError> alloc_cons(LispVal car, LispVal cdr);

        // Total bytes currently allocated across all shards.
        // Exposes the aggregate atomic, useful for statistics (e.g., GC stats).
        size_t total_bytes() const { return total_heap_bytes.load(std::memory_order_relaxed); }

        // Soft heap size limit (bytes).
        size_t soft_limit() const { return max_heap_soft_limit_; }

        // Visit all entries in the heap. The callback may ONLY mutate entry.header.flags.
        // Other fields are considered read-only in the callback context.
        void for_each_entry(const std::function<void(ObjectId, HeapEntry&)>& fn);

        // Snapshot a HeapEntry for a given ObjectId. Returns false if not found.
        bool try_get(ObjectId id, HeapEntry& out) const;

        // Provide limited, controlled mutable access to a single entry. The callable must
        // only mutate entry.header.flags. Returns false if the id is not found.
        bool with_entry(ObjectId id, const std::function<void(HeapEntry&)>& fn);

        // Type-safe heap object accessor.
        // Returns pointer to the object if ID exists and kind matches, nullptr otherwise.
        // This is the canonical way to access typed heap objects - use instead of
        // manually checking try_get + kind comparison.
        template<ObjectKind Kind, typename T>
        T* try_get_as(ObjectId id) {
            HeapEntry entry;
            if (!try_get(id, entry)) return nullptr;
            if (entry.header.kind != Kind) return nullptr;
            return static_cast<T*>(entry.ptr);
        }

        template<ObjectKind Kind, typename T>
        const T* try_get_as(ObjectId id) const {
            HeapEntry entry;
            if (!try_get(id, entry)) return nullptr;
            if (entry.header.kind != Kind) return nullptr;
            return static_cast<const T*>(entry.ptr);
        }

        // GC pause mechanism - prevents new allocations during GC sweep
        // Call pause_for_gc() before sweep, resume_after_gc() after sweep completes
        void pause_for_gc() { gc_in_progress_.store(true, std::memory_order_release); }
        void resume_after_gc() { gc_in_progress_.store(false, std::memory_order_release); }
        bool is_gc_paused() const { return gc_in_progress_.load(std::memory_order_acquire); }

        void set_gc_callback(std::function<void()> cb) { gc_callback_ = std::move(cb); }

        // Sweep the cons pool and adjust total_heap_bytes.
        // Returns the number of freed pool cells.
        std::size_t sweep_cons_pool();

        // Access the dedicated cons-cell pool (for GC / DAP).
        ConsPool& cons_pool();
        const ConsPool& cons_pool() const;

    private:

        struct ShardStats {
            std::atomic<std::size_t> num_objects{ 0 };
            std::atomic<std::size_t> heap_bytes{ 0 };
        };

        //! 'Global Stat'.
        cache_align std::atomic<std::size_t> total_heap_bytes{ 0 };

        //! GC pause flag - when true, allocations are rejected
        std::atomic<bool> gc_in_progress_{ false };

        std::function<void()> gc_callback_;

        struct cache_align Shard {
            boost::unordered::concurrent_flat_map<ObjectId, HeapEntry> heap_objects;
            ShardStats stats;
        };

        static uint64_t split_mix_64(uint64_t x) noexcept;
        static size_t select_shard(ObjectId id) noexcept;

        size_t max_heap_soft_limit_;
        std::array<Shard, NUM_SHARDS> shards;
        std::atomic<ObjectId> next_id{ 1 };
        std::unique_ptr<ConsPool> cons_pool_;

    };

}