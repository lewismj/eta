#include <eta/runtime/memory/mark_sweep_gc.h>
#include <vector>

namespace eta::runtime::memory::gc {
    using namespace eta::runtime::memory::heap;

    void MarkSweepGC::clear_marks(Heap& heap) const {
        /// Pool entries: fast dense-array walk (no HeapEntry wrapper overhead)
        heap.cons_pool().clear_marks();

        /**
         * General-heap entries
         * (for_each_entry also visits pool entries but their marks are already
         */
        heap.for_each_entry([&](ObjectId, heap::HeapEntry& e) {
            e.header.flags &= static_cast<uint8_t>(~heap::MARK_BIT);
        });
    }

    std::size_t MarkSweepGC::sweep(heap::Heap& heap) {
        ///    adjusts Heap::total_heap_bytes in one call.
        std::size_t pool_freed = heap.sweep_cons_pool();

        /**
         * 2. General-heap sweep: collect unmarked IDs, then deallocate.
         *    Pool-owned IDs are skipped (already swept above).
         */
        std::vector<heap::ObjectId> garbage_ids;
        garbage_ids.reserve(1024);

        heap.for_each_entry([&](heap::ObjectId id, heap::HeapEntry& e) {
            if (heap.cons_pool().owns(id)) return;   ///< already handled
            if ((e.header.flags & heap::MARK_BIT) == 0) {
                garbage_ids.push_back(id);
            }
        });

        std::size_t total_freed = pool_freed;
        for (const auto& id : garbage_ids) {
            if (heap.deallocate(id).has_value()) {
                ++total_freed;
            }
        }
        return total_freed;
    }
}
