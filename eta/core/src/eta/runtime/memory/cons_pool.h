#pragma once

#include <atomic>
#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <vector>

#include <eta/runtime/memory/heap.h>
#include <eta/runtime/types/cons.h>

namespace eta::runtime::memory::heap {
    using namespace eta::runtime::nanbox;

    /**
     * Bit 1 in ObjectHeader.flags marks a pool slot as allocated (live).
     * Bit 0 is MARK_BIT (defined in heap.h).
     */
    constexpr uint8_t POOL_ALLOCATED_BIT = 1u << 1;

    struct PoolStats {
        std::size_t capacity{};
        std::size_t live_count{};
        std::size_t free_count{};
        std::size_t bytes{};
    };

    /**
     * Dedicated slab-based pool for cons cells.
     *
     * Allocates cons cells from contiguous slabs with an intrusive free-list,
     * eliminating the hash-map overhead and general-allocator cost for the
     * single most-frequently-allocated object type.
     *
     * ObjectIds are claimed in contiguous ranges from the Heap's atomic
     * counter so pool IDs coexist with general heap IDs without collision.
     */
    class ConsPool {
    public:
        struct ConsSlot {
            ObjectHeader header{};
            ObjectId     id{0};
            union {
                types::Cons  cell;       ///< car + cdr when live  (16 bytes)
                ConsSlot*    next_free;  ///< free-list link when free (8 bytes)
            };

            ConsSlot() : next_free{nullptr} {}
        };

        explicit ConsPool(std::size_t initial_capacity, std::atomic<ObjectId>& id_source)
            : id_source_(id_source)
        {
            grow(initial_capacity);
        }

        /// allocation

        /// Pop free-list or grow; return ObjectId for the new cons cell.
        std::expected<ObjectId, HeapError> alloc(LispVal car, LispVal cdr) {
            if (!free_head_) {
                const auto new_cap = total_capacity_ > 0 ? total_capacity_ : 8192;
                if (!grow(new_cap)) {
                    return std::unexpected(HeapError::FailedToAllocateMemory);
                }
            }

            ConsSlot* slot = free_head_;
            free_head_ = slot->next_free;

            slot->header.kind  = ObjectKind::Cons;
            slot->header.flags = POOL_ALLOCATED_BIT;   ///< allocated, not marked
            slot->cell.car = car;
            slot->cell.cdr = cdr;

            ++live_count_;
            return slot->id;
        }

        /// lookup

        types::Cons* try_get(ObjectId id) {
            auto* slot = find_slot(id);
            if (!slot || !(slot->header.flags & POOL_ALLOCATED_BIT)) return nullptr;
            return &slot->cell;
        }

        const types::Cons* try_get(ObjectId id) const {
            auto* slot = find_slot(id);
            if (!slot || !(slot->header.flags & POOL_ALLOCATED_BIT)) return nullptr;
            return &slot->cell;
        }

        /// Fill a HeapEntry from a pool slot (for Heap::try_get integration).
        bool try_get_entry(ObjectId id, HeapEntry& out) const {
            const auto* slot = find_slot(id);
            if (!slot || !(slot->header.flags & POOL_ALLOCATED_BIT)) return false;
            out.header     = slot->header;
            out.ptr        = const_cast<types::Cons*>(&slot->cell);
            out.size       = sizeof(types::Cons);
            out.destructor = nullptr;
            return true;
        }

        /// Is this ID in any slab's range?
        bool owns(ObjectId id) const {
            return find_slot(id) != nullptr;
        }

        /// deallocation

        /// Return a live slot to the free-list.
        void free_slot(ObjectId id) {
            auto* slot = find_slot(id);
            if (!slot || !(slot->header.flags & POOL_ALLOCATED_BIT)) return;
            slot->header.flags = 0;          ///< clear allocated + mark
            slot->next_free = free_head_;
            free_head_ = slot;
            --live_count_;
        }

        /// GC support

        /// Zero all mark bits on allocated slots (dense sweep).
        void clear_marks() {
            for (auto& slab : slabs_) {
                for (std::size_t i = 0; i < slab.capacity; ++i) {
                    auto& slot = slab.slots[i];
                    if (slot.header.flags & POOL_ALLOCATED_BIT) {
                        slot.header.flags &= static_cast<uint8_t>(~MARK_BIT);
                    }
                }
            }
        }

        /// Set the mark bit on a pool slot.
        void mark(ObjectId id) {
            if (auto* slot = find_slot(id)) {
                slot->header.flags |= MARK_BIT;
            }
        }

        /**
         * Combined check-and-mark.  Returns Cons* if newly marked
         * (was unmarked and allocated).  Returns nullptr if already marked,
         * not allocated, or not in this pool's range.
         */
        types::Cons* try_mark(ObjectId id) {
            auto* slot = find_slot(id);
            if (!slot || !(slot->header.flags & POOL_ALLOCATED_BIT)) return nullptr;
            if (slot->header.flags & MARK_BIT) return nullptr; ///< already marked
            slot->header.flags |= MARK_BIT;
            return &slot->cell;
        }

        /// Walk all live slots; push unmarked onto free-list; return freed count.
        std::size_t sweep() {
            std::size_t freed = 0;
            for (auto& slab : slabs_) {
                for (std::size_t i = 0; i < slab.capacity; ++i) {
                    auto& slot = slab.slots[i];
                    if ((slot.header.flags & POOL_ALLOCATED_BIT) &&
                        !(slot.header.flags & MARK_BIT)) {
                        slot.header.flags = 0;
                        slot.next_free = free_head_;
                        free_head_ = &slot;
                        --live_count_;
                        ++freed;
                    }
                }
            }
            return freed;
        }

        /// stats / iteration

        PoolStats stats() const {
            return {
                .capacity   = total_capacity_,
                .live_count = live_count_,
                .free_count = total_capacity_ - live_count_,
                .bytes      = live_count_ * sizeof(types::Cons),
            };
        }

        /**
         * Iterate all live slots, presenting each as a HeapEntry.
         * Any header.flags mutation the callback makes is written back.
         */
        void for_each_live_entry(const std::function<void(ObjectId, HeapEntry&)>& fn) {
            for (auto& slab : slabs_) {
                for (std::size_t i = 0; i < slab.capacity; ++i) {
                    auto& slot = slab.slots[i];
                    if (slot.header.flags & POOL_ALLOCATED_BIT) {
                        HeapEntry entry{
                            .header     = slot.header,
                            .ptr        = &slot.cell,
                            .size       = sizeof(types::Cons),
                            .destructor = nullptr,
                        };
                        fn(slot.id, entry);
                        /// write-back: callback may only mutate header.flags
                        slot.header.flags = entry.header.flags;
                    }
                }
            }
        }

        /**
         * Provide mutable HeapEntry access for a single pool slot.
         * Writes back header.flags after the callback returns.
         */
        bool with_entry(ObjectId id, const std::function<void(HeapEntry&)>& fn) {
            auto* slot = find_slot(id);
            if (!slot || !(slot->header.flags & POOL_ALLOCATED_BIT)) return false;
            HeapEntry entry{
                .header     = slot->header,
                .ptr        = &slot->cell,
                .size       = sizeof(types::Cons),
                .destructor = nullptr,
            };
            fn(entry);
            slot->header.flags = entry.header.flags;
            return true;
        }

    private:
        struct Slab {
            std::unique_ptr<ConsSlot[]> slots;
            std::size_t capacity;
            ObjectId    base_id;       ///< first ObjectId in this slab
        };

        /// Allocate a new slab and claim a contiguous ID range.
        bool grow(std::size_t capacity) {
            const auto base_id = id_source_.fetch_add(capacity, std::memory_order_relaxed);

            if (base_id + capacity > nanbox::constants::PAYLOAD_MASK) {
                return false;
            }

            auto slots = std::make_unique<ConsSlot[]>(capacity);

            /// Assign IDs and link into the free-list.
            for (std::size_t i = 0; i < capacity; ++i) {
                auto& slot   = slots[i];
                slot.header.kind  = ObjectKind::Cons;
                slot.header.flags = 0;                ///< not allocated
                slot.id      = base_id + i;
                slot.next_free = (i + 1 < capacity) ? &slots[i + 1] : free_head_;
            }
            free_head_ = &slots[0];

            total_capacity_ += capacity;

            slabs_.push_back(Slab{
                .slots    = std::move(slots),
                .capacity = capacity,
                .base_id  = base_id,
            });
            return true;
        }

        ConsSlot* find_slot(ObjectId id) const {
            for (const auto& slab : slabs_) {
                if (id >= slab.base_id && id < slab.base_id + slab.capacity) {
                    return &slab.slots[id - slab.base_id];
                }
            }
            return nullptr;
        }

        std::vector<Slab>     slabs_;
        ConsSlot*             free_head_      = nullptr;
        std::size_t           live_count_     = 0;
        std::size_t           total_capacity_ = 0;
        std::atomic<ObjectId>& id_source_;
    };
}

