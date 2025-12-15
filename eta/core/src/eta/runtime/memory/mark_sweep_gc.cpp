#include <eta/runtime/memory/mark_sweep_gc.h>

namespace eta::runtime::memory::gc {
    using namespace eta::runtime::memory::heap;

    void MarkSweepGC::clear_marks(Heap& heap) const {
        heap.for_each_entry([&](ObjectId, heap::HeapEntry& e) {
            e.header.flags &= static_cast<uint8_t>(~heap::MARK_BIT);
        });
    }

    std::size_t MarkSweepGC::sweep(heap::Heap& heap) {
        std::vector<heap::ObjectId> to_free;
        to_free.reserve(1024);

        heap.for_each_entry([&](heap::ObjectId id, heap::HeapEntry& e) {
            if ((e.header.flags & heap::MARK_BIT) == 0) {
                to_free.push_back(id);
            }
        });

        std::size_t freed = 0;
        for (auto id : to_free) {
            if (heap.deallocate(id).has_value()) {
                ++freed;
            }
        }
        return freed;
    }
}
