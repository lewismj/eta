#pragma once

#include <vector>
#include <span>
#include <unordered_set>

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
            /// Attribute values are reachable through the var.
            for (const auto& [_k, v] : lv.attrs) callback(v);
        }


        void visit_tape(const types::Tape& /*t*/) override {
        }

        void visit_primitive(const types::Primitive& p) override {
            for (auto v : p.gc_roots) callback(v);
        }

        void visit_guardian(const types::Guardian& g) override {
            for (auto v : g.ready_queue) callback(v);
        }

        void visit_fact_table(const types::FactTable& ft) override {
            for (const auto& col : ft.columns)
                for (auto v : col) callback(v);
            for (auto v : ft.rule_column) callback(v);
        }

        void visit_compound_term(const types::CompoundTerm& ct) override {
            callback(ct.functor);
            for (auto v : ct.args) callback(v);
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
        /// Callback-based root enumeration entry point
        template <typename EnumerateRoots>
        void collect(Heap& heap, EnumerateRoots enumerate_roots, GCStats* out_stats = nullptr) {
            if (out_stats) out_stats->bytes_before = heap.total_bytes();
            clear_marks(heap);
            mark_from_roots(heap, enumerate_roots);
            mark_pending_finalizers(heap);
            mark_live_finalizer_procs_fixpoint(heap);

            const auto dead_finalizer_keys = snapshot_dead_finalizer_keys(heap);
            const auto dead_guarded_objects = snapshot_dead_guarded_objects(heap);
            enqueue_dead_finalizers(heap, dead_finalizer_keys);
            enqueue_dead_guardians(heap, dead_guarded_objects);

            mark_pending_finalizers(heap);
            mark_live_finalizer_procs_fixpoint(heap);

            /**
             * Pause allocations during sweep to prevent unmarked new objects
             * from being immediately deallocated (stop-the-world)
             */
            heap.pause_for_gc();
            const auto freed = sweep(heap);
            heap.resume_after_gc();

            if (out_stats) {
                out_stats->objects_freed = freed;
                out_stats->bytes_after = heap.total_bytes();
            }
        }

        /// Iterator/range convenience
        template <typename It>
        void collect(Heap& heap, It first, It last, GCStats* out_stats = nullptr) {
            collect(heap, [&](auto&& visit) {
                for (; first != last; ++first) visit(*first);
            }, out_stats);
        }

        /// span convenience
        void collect(Heap& heap, const std::span<const LispVal> roots, GCStats* out_stats = nullptr) {
            collect(heap, [&](auto&& visit) {
                for (auto v : roots) visit(v);
            }, out_stats);
        }

        /// Expose phases for white-box testing
        void clear_marks(Heap& heap) const;

        /// returns objects freed
        static std::size_t sweep(Heap& heap);

    private:
        /// Shared utility: push heap object ID to worklist if the value is a heap reference
        static void push_if_heap_ref(LispVal v, std::vector<heap::ObjectId>& worklist) {
            if (ops::is_boxed(v) && ops::tag(v) == Tag::HeapObject) {
                worklist.push_back(static_cast<heap::ObjectId>(ops::payload(v)));
            }
        }

        static bool is_marked(const Heap& heap, heap::ObjectId id) {
            if (heap.cons_pool().owns(id)) {
                heap::HeapEntry entry{};
                if (!heap.cons_pool().try_get_entry(id, entry)) return false;
                return (entry.header.flags & heap::MARK_BIT) != 0;
            }

            heap::HeapEntry entry{};
            if (!heap.try_get(id, entry)) return false;
            return (entry.header.flags & heap::MARK_BIT) != 0;
        }

        bool mark_worklist(Heap& heap, std::vector<heap::ObjectId>& work) const {
            bool marked_any = false;

            auto& pool = heap.cons_pool();
            heap::HeapEntry entry{};
            while (!work.empty()) {
                const auto id = work.back();
                work.pop_back();

                /**
                 * Fast path: pool-owned cons cell.
                 * try_mark returns Cons* only if newly marked (skips already-marked
                 * and freed slots).
                 */
                if (auto* cons = pool.try_mark(id)) {
                    marked_any = true;
                    push_if_heap_ref(cons->car, work);
                    push_if_heap_ref(cons->cdr, work);
                    continue;
                }
                /// already marked or freed slot. Either way, nothing to do.
                if (pool.owns(id)) continue;

                if (!heap.try_get(id, entry)) continue; ///< stale id
                if ((entry.header.flags & heap::MARK_BIT) != 0) continue; ///< already marked

                if (!heap.with_entry(id, [&](heap::HeapEntry& e) {
                    e.header.flags |= heap::MARK_BIT;
                })) {
                    continue;
                }

                marked_any = true;
                auto push_ref = [&work](LispVal v) {
                    push_if_heap_ref(v, work);
                };
                visit_heap_refs(entry, push_ref);
            }

            return marked_any;
        }

        template <typename EnumerateValues>
        bool mark_from_values(Heap& heap, EnumerateValues enumerate_values) const {
            std::vector<heap::ObjectId> work;
            work.reserve(1024);

            enumerate_values([&](LispVal v) {
                push_if_heap_ref(v, work);
            });

            return mark_worklist(heap, work);
        }

        bool mark_value(Heap& heap, LispVal value) const {
            return mark_from_values(heap, [&](auto&& visit) {
                visit(value);
            });
        }

        /// templated mark phase using centralized visitor
        template <typename EnumerateRoots>
        void mark_from_roots(Heap& heap, EnumerateRoots enumerate_roots) const {
            (void)mark_from_values(heap, enumerate_roots);
        }

        void mark_pending_finalizers(Heap& heap) const {
            auto pending = heap.pending_finalizers_snapshot();
            if (pending.empty()) return;

            (void)mark_from_values(heap, [&](auto&& visit) {
                for (const auto& finalizer : pending) {
                    visit(finalizer.obj);
                    visit(finalizer.proc);
                }
            });
        }

        void mark_live_finalizer_procs_fixpoint(Heap& heap) const {
            bool changed = true;
            while (changed) {
                changed = false;
                auto finalizers = heap.finalizer_table_snapshot();
                for (const auto& [obj_id, proc] : finalizers) {
                    if (!is_marked(heap, obj_id)) continue;
                    if (mark_value(heap, proc)) {
                        changed = true;
                    }
                }
            }
        }

        std::unordered_set<heap::ObjectId> snapshot_dead_finalizer_keys(Heap& heap) const {
            auto finalizers = heap.finalizer_table_snapshot();
            std::unordered_set<heap::ObjectId> dead_keys;
            dead_keys.reserve(finalizers.size());

            for (const auto& [obj_id, _proc] : finalizers) {
                if (!is_marked(heap, obj_id)) {
                    dead_keys.insert(obj_id);
                }
            }

            return dead_keys;
        }

        std::unordered_set<heap::ObjectId> snapshot_dead_guarded_objects(Heap& heap) const {
            auto tracked_pairs = heap.guardian_tracking_snapshot();
            std::unordered_set<heap::ObjectId> dead_keys;
            dead_keys.reserve(tracked_pairs.size());

            for (const auto& [_guardian_id, obj_id] : tracked_pairs) {
                if (!is_marked(heap, obj_id)) {
                    dead_keys.insert(obj_id);
                }
            }

            return dead_keys;
        }

        void enqueue_dead_finalizers(Heap& heap,
                                     const std::unordered_set<heap::ObjectId>& dead_keys) const {
            auto finalizers = heap.finalizer_table_snapshot();
            for (const auto& [obj_id, proc] : finalizers) {
                if (dead_keys.find(obj_id) == dead_keys.end()) continue;

                heap::HeapEntry entry{};
                if (!heap.try_get(obj_id, entry)) {
                    (void)heap.remove_finalizer(obj_id);
                    continue;
                }

                const auto boxed_obj = ops::box(Tag::HeapObject, obj_id);
                heap.enqueue_pending_finalizer(boxed_obj, proc);
                (void)heap.remove_finalizer(obj_id);

                /**
                 * Keep the finalized object and procedure graphs alive until the VM
                 * drains the pending queue.
                 */
                (void)mark_value(heap, boxed_obj);
                (void)mark_value(heap, proc);
            }
        }

        void enqueue_dead_guardians(Heap& heap,
                                    const std::unordered_set<heap::ObjectId>& dead_keys) const {
            auto tracked_pairs = heap.guardian_tracking_snapshot();
            for (const auto& [guardian_id, tracked_id] : tracked_pairs) {
                if (!is_marked(heap, guardian_id)) {
                    (void)heap.remove_guardian_tracking(guardian_id, tracked_id);
                    continue;
                }

                if (dead_keys.find(tracked_id) == dead_keys.end()) continue;

                heap::HeapEntry entry{};
                if (!heap.try_get(tracked_id, entry)) {
                    (void)heap.remove_guardian_tracking(guardian_id, tracked_id);
                    continue;
                }

                const auto boxed_obj = ops::box(Tag::HeapObject, tracked_id);
                if (!heap.enqueue_guardian_ready(guardian_id, boxed_obj)) {
                    (void)heap.remove_guardian_tracking(guardian_id, tracked_id);
                    continue;
                }

                /**
                 * Guardian delivery is at-most-once per track call.
                 * Remove weak links once queued.
                 */
                (void)heap.remove_guardian_tracking(guardian_id, tracked_id);

                /// Keep the delivered object alive until user code collects it.
                (void)mark_value(heap, boxed_obj);
            }
        }
    };
}
