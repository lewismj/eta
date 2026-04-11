#pragma once

#include <vector>
#include <span>

#include <eta/runtime/nanbox.h>
#include <eta/runtime/types/types.h>
#include <eta/runtime/memory/heap_visit.h>
#include <eta/runtime/memory/heap.h>
#include <eta/runtime/memory/cons_pool.h>

namespace eta::runtime::memory::gc {
    using namespace eta::runtime::nanbox;
    using heap::Heap;

    struct GCStats {
        std::size_t bytes_before{};
        std::size_t bytes_after{};
        std::size_t objects_freed{};
    };

    /**
     * @brief Lambda-based visitor for heap object traversal
     *
     * This replaces the manual MarkVisitor class by using the centralized
     * visit_heap_object dispatcher with a generic callback. This ensures
     * that when new heap-allocated types are added, they only need to be
     * updated in heap_visit.h, not in multiple visitor implementations.
     */
    template<typename Callback>
    struct LambdaHeapVisitor final : heap::HeapVisitor<void> {
        Callback& callback;
        explicit LambdaHeapVisitor(Callback& cb) : callback(cb) {}

        void visit_cons(const types::Cons& c) override {
            callback(c.car);
            callback(c.cdr);
        }


        void visit_closure(const types::Closure& c) override {
            for (auto v : c.upvals) callback(v);
            if (c.func) {
                for (auto v : c.func->constants) callback(v);
            }
        }

        void visit_vector(const types::Vector& vec) override {
            for (auto elem : vec.elements) callback(elem);
        }

        void visit_continuation(const types::Continuation& c) override {
            for (auto v : c.stack) callback(v);
            for (const auto& frame : c.frames) {
                callback(frame.closure);
                callback(frame.extra);
            }
            for (const auto& wind : c.winding_stack) {
                callback(wind.before);
                callback(wind.body);
                callback(wind.after);
            }
        }

        void visit_multiple_values(const types::MultipleValues& mv) override {
            for (auto v : mv.vals) callback(v);
        }

        void visit_logic_var(const types::LogicVar& lv) override {
            if (lv.binding.has_value()) callback(*lv.binding);
        }


        void visit_tape(const types::Tape& /*t*/) override {
            // Tape entries contain only doubles and uint32_t indices — no LispVal references.
        }

        void visit_primitive(const types::Primitive& p) override {
            for (auto v : p.gc_roots) callback(v);
        }

        void visit_fact_table(const types::FactTable& ft) override {
            for (const auto& col : ft.columns)
                for (auto v : col) callback(v);
        }

        void visit_leaf(heap::ObjectKind, const void*) override { /* no edges */ }
    };

    /**
     * @brief Visit all LispVal references within a heap object
     * @param entry The heap entry to visit
     * @param callback A callable that takes LispVal&
     */
    template<typename Callback>
    void visit_heap_refs(const heap::HeapEntry& entry, Callback&& callback) {
        LambdaHeapVisitor<std::remove_reference_t<Callback>> visitor{callback};
        heap::visit_heap_object<void>(entry, visitor);
    }

    class MarkSweepGC final {
    public:
        // Callback-based root enumeration entry point
        template <typename EnumerateRoots>
        void collect(Heap& heap, EnumerateRoots enumerate_roots, GCStats* out_stats = nullptr) {
            if (out_stats) out_stats->bytes_before = heap.total_bytes();
            clear_marks(heap);
            mark_from_roots(heap, enumerate_roots);

            // Pause allocations during sweep to prevent unmarked new objects
            // from being immediately deallocated (stop-the-world)
            heap.pause_for_gc();
            const auto freed = sweep(heap);
            heap.resume_after_gc();

            if (out_stats) {
                out_stats->objects_freed = freed;
                out_stats->bytes_after = heap.total_bytes();
            }
        }

        // Iterator/range convenience
        template <typename It>
        void collect(Heap& heap, It first, It last, GCStats* out_stats = nullptr) {
            collect(heap, [&](auto&& visit) {
                for (; first != last; ++first) visit(*first);
            }, out_stats);
        }

        // span convenience
        void collect(Heap& heap, const std::span<const LispVal> roots, GCStats* out_stats = nullptr) {
            collect(heap, [&](auto&& visit) {
                for (auto v : roots) visit(v);
            }, out_stats);
        }

        // Expose phases for white-box testing
        void clear_marks(Heap& heap) const;

        // returns objects freed
        static std::size_t sweep(Heap& heap);

    private:
        // Shared utility: push heap object ID to worklist if the value is a heap reference
        static void push_if_heap_ref(LispVal v, std::vector<heap::ObjectId>& worklist) {
            if (ops::is_boxed(v) && ops::tag(v) == Tag::HeapObject) {
                worklist.push_back(static_cast<heap::ObjectId>(ops::payload(v)));
            }
        }

        // templated mark phase using centralized visitor
        template <typename EnumerateRoots>
        void mark_from_roots(Heap& heap, EnumerateRoots enumerate_roots) const {
            using namespace eta::runtime::nanbox;

            std::vector<heap::ObjectId> work;
            work.reserve(1024);

            // Enumerate and scan roots - directly check for heap refs
            enumerate_roots([&](LispVal v) {
                push_if_heap_ref(v, work);
            });

            // Use lambda-based visitor for marking
            auto push_ref = [&work](LispVal v) {
                push_if_heap_ref(v, work);
            };

            auto& pool = heap.cons_pool();
            heap::HeapEntry entry{};
            while (!work.empty()) {
                const auto id = work.back();
                work.pop_back();

                // ── Fast path: pool-owned cons cell ──
                // try_mark returns Cons* only if newly marked (skips already-marked
                // and freed slots).  Avoids try_get / with_entry / HeapVisitor dispatch.
                if (auto* cons = pool.try_mark(id)) {
                    push_if_heap_ref(cons->car, work);
                    push_if_heap_ref(cons->cdr, work);
                    continue;
                }
                // Pool owns the ID but try_mark returned nullptr → already marked
                // or freed slot.  Either way, nothing to do.
                if (pool.owns(id)) continue;

                // ── General-heap path (unchanged) ──
                if (!heap.try_get(id, entry)) continue; // stale id
                if ((entry.header.flags & heap::MARK_BIT) != 0) continue; // already marked

                // set mark bit
                heap.with_entry(id, [&](heap::HeapEntry& e) {
                    e.header.flags |= heap::MARK_BIT;
                });

                // Use centralized dispatch via lambda visitor
                visit_heap_refs(entry, push_ref);
            }
        }
    };
}
