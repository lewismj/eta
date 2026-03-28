#pragma once

#include <vector>
#include <functional>
#include <span>

#include <eta/runtime/nanbox.h>
#include <eta/runtime/types/types.h>
#include <eta/runtime/memory/heap_visit.h>
#include <eta/runtime/memory/value_visit.h>
#include <eta/runtime/memory/heap.h>

namespace eta::runtime::memory::gc {
    using namespace eta::runtime::nanbox;
    using heap::Heap;

    struct GCStats {
        std::size_t bytes_before{};
        std::size_t bytes_after{};
        std::size_t objects_freed{};
    };

    class MarkSweepGC final {
    public:
        // Callback-based root enumeration entry point
        template <typename EnumerateRoots>
        void collect(Heap& heap, EnumerateRoots enumerate_roots, GCStats* out_stats = nullptr) {
            if (out_stats) out_stats->bytes_before = heap.total_bytes();
            clear_marks(heap);
            mark_from_roots(heap, enumerate_roots);
            const auto freed = sweep(heap);
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
        // templated mark phase (header-only)
        template <typename EnumerateRoots>
        void mark_from_roots(Heap& heap, EnumerateRoots enumerate_roots) const {
            using namespace eta::runtime::nanbox;

            std::vector<heap::ObjectId> work;
            work.reserve(1024);

            // Root scanner using ValueVisitor
            struct RootScan final : value_visit::ValueVisitor<void> {
                std::vector<heap::ObjectId>& worklist;
                explicit RootScan(std::vector<heap::ObjectId>& w) : worklist(w) {}
                void visit_fixnum(std::int64_t) override {}
                void visit_char(char32_t) override {}
                void visit_string(uint64_t) override {}
                void visit_symbol(uint64_t) override {}
                void visit_nan() override {}
                void visit_heapref(uint64_t id) override { worklist.push_back(static_cast<heap::ObjectId>(id)); }
                void visit_nil() override {}
            } root_scan{work};

            // Enumerate and scan roots
            enumerate_roots([&](LispVal v) {
                value_visit::visit_value<void>(v, root_scan);
            });

            // Mark visitor over heap objects
            struct MarkVisitor final : eta::runtime::memory::heap::HeapVisitor<void> {
                std::vector<heap::ObjectId>& worklist;
                explicit MarkVisitor(std::vector<heap::ObjectId>& w) : worklist(w) {}

                // helper to push children when they are heap refs
                void push_if_heap_obj(LispVal v) {
                    struct PushIfHeap final : value_visit::ValueVisitor<void> {
                        std::vector<heap::ObjectId>& wl;
                        explicit PushIfHeap(std::vector<heap::ObjectId>& w) : wl(w) {}
                        void visit_fixnum(std::int64_t) override {}
                        void visit_char(char32_t) override {}
                        void visit_string(uint64_t) override {}
                        void visit_symbol(uint64_t) override {}
                        void visit_nan() override {}
                        void visit_heapref(uint64_t id) override { wl.push_back(static_cast<heap::ObjectId>(id)); }
                        void visit_nil() override {}
                    } push{worklist};
                    value_visit::visit_value<void>(v, push);
                }

                void visit_cons(const eta::runtime::types::Cons& c) override {
                    push_if_heap_obj(c.car);
                    push_if_heap_obj(c.cdr);
                }

                void visit_lambda(const eta::runtime::types::Lambda& l) override {
                    push_if_heap_obj(l.body);
                    for (auto v : l.formals) push_if_heap_obj(v);
                    for (auto v : l.up_values) push_if_heap_obj(v);
                }

                void visit_closure(const eta::runtime::types::Closure& c) override {
                    for (auto v : c.upvals) push_if_heap_obj(v);
                }

                void visit_vector(const eta::runtime::types::Vector& v) override {
                    for (auto v : v.elements) push_if_heap_obj(v);
                }

                void visit_continuation(const eta::runtime::types::Continuation& c) override {
                    for (auto v : c.stack) push_if_heap_obj(v);
                    for (const auto& frame : c.frames) {
                        push_if_heap_obj(frame.closure);
                    }
                }

                void visit_leaf(heap::ObjectKind, const void*) override { /* no edges */ }
            } marker{work};

            heap::HeapEntry entry{};
            while (!work.empty()) {
                const auto id = work.back();
                work.pop_back();

                if (!heap.try_get(id, entry)) continue; // stale id
                if ((entry.header.flags & heap::MARK_BIT) != 0) continue; // already marked

                // set mark bit
                heap.with_entry(id, [&](heap::HeapEntry& e) {
                    e.header.flags |= heap::MARK_BIT;
                });

                // Centralized dispatch
                heap::visit_heap_object<void>(entry, marker);
            }
        }
    };
}
