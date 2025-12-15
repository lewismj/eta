#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <expected>
#include <ostream>
#include <boost/unordered/concurrent_flat_map.hpp>

#include <eta/arch.h>
#include <eta/runtime/nanbox.h>

namespace eta::runtime::memory::heap {
    using namespace eta::runtime::nanbox;

    using ObjectId = std::uint64_t;

    enum class ObjectKind : std::uint8_t {
        Unknown, //!! Unmatched Kind.
        Cons,
        Lambda,
        Fixnum,
    };

    enum class HeapError : std::uint8_t {
        HeapObjectIdExhausted,
        FailedToAllocateMemory,
        FailedToDeallocateMemory,
        ObjectIdNotFound,
        NullPtrReference,
        SoftHeapLimitExceeded,
        UnexpectedObjectKind,
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

    class Heap {
    public:
        explicit Heap(const size_t max_heap_soft_limit)
            : max_heap_soft_limit_(static_cast<std::size_t>(max_heap_soft_limit))
        {
        }

        //! Destroy the heap. Here we don't bother updating stats at this point, since
        //! we're terminating the heap entirely.
        ~Heap() {
            for (auto& [heap_objects, stats] : shards) {
                heap_objects.visit_all([](const auto& kv) {
                    if (const auto& entry = kv.second; entry.ptr && entry.destructor) {
                       try {
                           entry.destructor(entry.ptr);
                       } catch (...) {
                           //! Policy is to not allow any stored destructors at this point
                           //! to invoke std::terminate
                       }

                    }
                });
            }
        }

        Heap(const Heap&) = delete;
        Heap& operator=(const Heap&) = delete;
        Heap(Heap&&) = delete;
        Heap& operator=(Heap&&) = delete;

        constexpr static std::size_t NUM_SHARDS = 16;
        static_assert((NUM_SHARDS & (NUM_SHARDS - 1)) == 0, "NUM_SHARDS must be a power of 2");

        template<typename T, ObjectKind Kind, typename ... Args>
        std::expected<ObjectId, HeapError> allocate(Args&&... args) {

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
            [[unlikely]] if (current_total + sizeof(T) > max_heap_soft_limit_) {
                return std::unexpected(HeapError::SoftHeapLimitExceeded);
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

        std::expected<void, HeapError> deallocate(const ObjectId id) {
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

            stats.num_objects.fetch_sub(1, std::memory_order_relaxed);
            stats.heap_bytes.fetch_sub(entry.size, std::memory_order_relaxed);
            total_heap_bytes.fetch_sub(entry.size, std::memory_order_relaxed);

            try {
                if (entry.destructor) {
                    entry.destructor(entry.ptr);
                } else {
                    ::operator delete(entry.ptr);
                }
            } catch (...) {
                // Ensure the entry is removed even if destructor throws, then surface an error.
                heap_objects.erase(id);
                return std::unexpected(HeapError::FailedToDeallocateMemory);
            }

            heap_objects.erase(id);
            return {};
        }

        // Total bytes currently allocated across all shards.
        // Exposes the aggregate atomic, useful for statistics (e.g., GC stats).
        size_t total_bytes() const { return total_heap_bytes.load(std::memory_order_relaxed); }

        // Visit all entries in the heap. The callback may ONLY mutate entry.header.flags.
        // Other fields are considered read-only in the callback context.
        void for_each_entry(const std::function<void(ObjectId, HeapEntry&)>& fn);

        // Snapshot a HeapEntry for a given ObjectId. Returns false if not found.
        bool try_get(ObjectId id, HeapEntry& out) const;

        // Provide limited, controlled mutable access to a single entry. The callable must
        // only mutate entry.header.flags. Returns false if the id is not found.
        bool with_entry(ObjectId id, const std::function<void(HeapEntry&)>& fn);

    private:

        struct ShardStats {
            std::atomic<std::size_t> num_objects{ 0 };
            std::atomic<std::size_t> heap_bytes{ 0 };
        };

        //! 'Global Stat'.
        cache_align std::atomic<std::size_t> total_heap_bytes{ 0 };

        struct cache_align Shard {
            boost::unordered::concurrent_flat_map<ObjectId, HeapEntry> heap_objects;
            ShardStats stats;
        };

        static uint64_t split_mix_64(uint64_t x) noexcept;
        static size_t select_shard(ObjectId id) noexcept;

        size_t max_heap_soft_limit_;
        std::array<Shard, NUM_SHARDS> shards;
        std::atomic<ObjectId> next_id{ 1 };

    };

}