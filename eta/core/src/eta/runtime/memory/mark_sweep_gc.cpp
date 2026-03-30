#include <eta/runtime/memory/mark_sweep_gc.h>
#include <vector>

namespace eta::runtime::memory::gc {
    using namespace eta::runtime::memory::heap;

    void MarkSweepGC::clear_marks(Heap& heap) const {
        heap.for_each_entry([&](ObjectId, heap::HeapEntry& e) {
            e.header.flags &= static_cast<uint8_t>(~heap::MARK_BIT);
        });
    }

    std::size_t MarkSweepGC::sweep(heap::Heap& heap) {
        // Single-pass sweep: collect ALL unmarked IDs in one iteration, then deallocate.
        // This is O(TotalObjects) rather than O(Garbage × TotalObjects).
        //
        // Note: We cannot deallocate during for_each_entry iteration as it may
        // invalidate iterators on the concurrent map. We use a dynamically-sized
        // vector to collect all garbage IDs in a single pass.

        std::vector<heap::ObjectId> garbage_ids;

        // Reserve some initial capacity to reduce reallocations
        // Typical GC might free 10-50% of objects
        garbage_ids.reserve(1024);

        // Single pass: collect all unmarked objects
        heap.for_each_entry([&](heap::ObjectId id, heap::HeapEntry& e) {
            if ((e.header.flags & heap::MARK_BIT) == 0) {
                garbage_ids.push_back(id);
            }
        });

        // Deallocate all collected garbage
        std::size_t total_freed = 0;
        for (const auto& id : garbage_ids) {
            if (heap.deallocate(id).has_value()) {
                ++total_freed;
            }
        }

        return total_freed;
    }
}
