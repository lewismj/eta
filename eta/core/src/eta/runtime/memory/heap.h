#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <deque>
#include <expected>
#include <mutex>
#include <memory>
#include <optional>
#include <ostream>
#include <functional>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>
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
        Guardian,
        MultipleValues,  ///< For (values ...) return
        Port,
        LogicVar,        ///< Unification logic variable
        Tape,            ///< AD tape (Wengert list for reverse-mode AD)
        Tensor,          ///< libtorch tensor (wraps torch::Tensor)
        NNModule,        ///< libtorch nn::Module (wraps shared_ptr<torch::nn::Module>)
        Optimizer,       ///< libtorch optimizer (wraps shared_ptr<torch::optim::Optimizer>)
        FactTable,       ///< Columnar fact table with per-column hash indexes
        HashMap,         ///< Immutable hash-map (open-addressing table)
        HashSet,         ///< Immutable hash-set
        CsvReader,       ///< CSV reader handle (opaque parser state + interned columns)
        CsvWriter,       ///< CSV writer handle (opaque stream + CSV options)
        Regex,           ///< Compiled regular expression object
        LogSink,         ///< Logging sink wrapper (spdlog sink + sink traits)
        LogLogger,       ///< Logging logger wrapper (spdlog logger + formatter mode)
        ProcessHandle,   ///< Native subprocess lifecycle handle + optional stdio ports
        NngSocket,       ///< nng socket (wraps NngSocketPtr from eta/nng/)
        CompoundTerm,    ///< Structured logic term: functor symbol + argument list
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
        ETA_ENUM_CASE(Guardian)
        ETA_ENUM_CASE(MultipleValues)
        ETA_ENUM_CASE(Port)
        ETA_ENUM_CASE(LogicVar)
        ETA_ENUM_CASE(Tape)
        ETA_ENUM_CASE(Tensor)
        ETA_ENUM_CASE(NNModule)
        ETA_ENUM_CASE(Optimizer)
        ETA_ENUM_CASE(FactTable)
        ETA_ENUM_CASE(HashMap)
        ETA_ENUM_CASE(HashSet)
        ETA_ENUM_CASE(CsvReader)
        ETA_ENUM_CASE(CsvWriter)
        ETA_ENUM_CASE(Regex)
        ETA_ENUM_CASE(LogSink)
        ETA_ENUM_CASE(LogLogger)
        ETA_ENUM_CASE(ProcessHandle)
        ETA_ENUM_CASE(NngSocket)
        ETA_ENUM_CASE(CompoundTerm)
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
        GCInProgress,  ///< Allocation rejected during GC
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


    /// In-heap GC mark bit lives in ObjectHeader.flags bit 0
    constexpr uint8_t MARK_BIT = 1u << 0;

    struct ObjectHeader {
        ObjectKind kind {};
        std::uint8_t flags : 3;
    };

    struct HeapEntry {
        ObjectHeader header{};
        void* ptr{};
        std::size_t size{};
        void (*destructor)(void*){}; ///< type-erased destructor
    };

    class ConsPool;

    class Heap {
    public:
        struct PendingFinalizer {
            LispVal obj{};
            LispVal proc{};
        };

        class ExternalRootFrame {
        public:
            explicit ExternalRootFrame(Heap& heap)
                : heap_(heap), saved_size_(heap.external_roots_.size()) {}

            ExternalRootFrame(const ExternalRootFrame&) = delete;
            ExternalRootFrame& operator=(const ExternalRootFrame&) = delete;

            ExternalRootFrame(ExternalRootFrame&& other) noexcept
                : heap_(other.heap_), saved_size_(other.saved_size_), active_(other.active_) {
                other.active_ = false;
            }

            ExternalRootFrame& operator=(ExternalRootFrame&&) = delete;

            ~ExternalRootFrame() {
                if (active_) heap_.external_roots_.resize(saved_size_);
            }

            void push(LispVal v) {
                heap_.external_roots_.push_back(v);
            }

        private:
            Heap& heap_;
            std::size_t saved_size_{};
            bool active_{true};
        };

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

            /// Reject allocations during GC to prevent unmarked objects being swept
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

            /// Single atomic load instead of iterating all shards
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

        /**
         * Pool-accelerated cons cell allocation.
         * Performs soft-limit check, then delegates to ConsPool.
         */
        std::expected<ObjectId, HeapError> alloc_cons(LispVal car, LispVal cdr);

        /**
         * Total bytes currently allocated across all shards.
         * Exposes the aggregate atomic, useful for statistics (e.g., GC stats).
         */
        size_t total_bytes() const { return total_heap_bytes.load(std::memory_order_relaxed); }

        /// Soft heap size limit (bytes).
        size_t soft_limit() const { return max_heap_soft_limit_; }

        /**
         * Visit all entries in the heap. The callback may ONLY mutate entry.header.flags.
         * Other fields are considered read-only in the callback context.
         */
        void for_each_entry(const std::function<void(ObjectId, HeapEntry&)>& fn);

        /// Snapshot a HeapEntry for a given ObjectId. Returns false if not found.
        bool try_get(ObjectId id, HeapEntry& out) const;

        /**
         * Provide limited, controlled mutable access to a single entry. The callable must
         * only mutate entry.header.flags. Returns false if the id is not found.
         */
        bool with_entry(ObjectId id, const std::function<void(HeapEntry&)>& fn);

        /**
         * Type-safe heap object accessor.
         * Returns pointer to the object if ID exists and kind matches, nullptr otherwise.
         * This is the canonical way to access typed heap objects - use instead of
         * manually checking try_get + kind comparison.
         */
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

        /**
         * GC pause mechanism - prevents new allocations during GC sweep
         * Call pause_for_gc() before sweep, resume_after_gc() after sweep completes
         */
        void pause_for_gc() { gc_in_progress_.store(true, std::memory_order_release); }
        void resume_after_gc() { gc_in_progress_.store(false, std::memory_order_release); }
        bool is_gc_paused() const { return gc_in_progress_.load(std::memory_order_acquire); }

        void set_gc_callback(std::function<void()> cb) { gc_callback_ = std::move(cb); }

        ExternalRootFrame make_external_root_frame() { return ExternalRootFrame(*this); }
        const std::vector<LispVal>& external_roots() const { return external_roots_; }

        /**
         * Register or replace a finalizer procedure for an object id.
         * Cons-pool objects are rejected to avoid ObjectId reuse hazards.
         */
        std::expected<void, HeapError> register_finalizer(ObjectId id, LispVal proc);

        /// Remove a finalizer registration. Returns true if an entry was erased.
        bool remove_finalizer(ObjectId id);

        /// Fetch a finalizer procedure for an object id, if present.
        std::optional<LispVal> fetch_finalizer(ObjectId id) const;

        /// Snapshot finalizer registrations for GC-side iteration.
        std::vector<std::pair<ObjectId, LispVal>> finalizer_table_snapshot() const;

        /// Append an object/procedure pair to the pending finalizer queue.
        void enqueue_pending_finalizer(LispVal obj, LispVal proc);

        /// Pop one pending finalizer pair from the front of the queue.
        std::optional<PendingFinalizer> dequeue_pending_finalizer();

        /// Number of pending finalizer pairs.
        std::size_t pending_finalizer_count() const;

        /// Snapshot pending finalizers for GC root marking.
        std::vector<PendingFinalizer> pending_finalizers_snapshot() const;

        /**
         * Register a weak tracking relation from guardian -> object.
         * Both ids must refer to live general-heap objects.
         */
        std::expected<void, HeapError> guardian_track(ObjectId guardian_id, ObjectId tracked_object_id);

        /// Remove one guardian tracking relation. Returns true when removed.
        bool remove_guardian_tracking(ObjectId guardian_id, ObjectId tracked_object_id);

        /**
         * Snapshot guardian tracking relations as (guardian_id, object_id) pairs.
         * Used by GC weak processing.
         */
        std::vector<std::pair<ObjectId, ObjectId>> guardian_tracking_snapshot() const;

        /// Append a ready object to a guardian queue. Returns false for stale/non-guardian ids.
        bool enqueue_guardian_ready(ObjectId guardian_id, LispVal object);

        /// Pop one object from a guardian ready queue; empty/non-guardian -> nullopt.
        std::optional<LispVal> dequeue_guardian_ready(ObjectId guardian_id);

        /**
         * Sweep the cons pool and adjust total_heap_bytes.
         * Returns the number of freed pool cells.
         */
        std::size_t sweep_cons_pool();

        /// Access the dedicated cons-cell pool (for GC / DAP).
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
        std::vector<LispVal> external_roots_;
        mutable std::mutex finalizer_mutex_;
        std::unordered_map<ObjectId, LispVal> finalizer_table_;
        std::deque<PendingFinalizer> pending_finalizers_;
        mutable std::mutex guardian_mutex_;
        std::unordered_map<ObjectId, std::unordered_set<ObjectId>> guardian_to_tracked_;
        std::unordered_map<ObjectId, std::unordered_set<ObjectId>> tracked_to_guardians_;

        void erase_pending_finalizers_for_object_unsafe(ObjectId id);
        void erase_guardian_tracking_for_object_unsafe(ObjectId id);
        void erase_guardian_tracking_for_guardian_unsafe(ObjectId id);

    };

}
