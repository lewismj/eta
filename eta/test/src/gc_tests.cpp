#include <boost/test/unit_test.hpp>

#include <eta/runtime/memory/heap.h>
#include <eta/runtime/memory/mark_sweep_gc.h>
#include <eta/runtime/factory.h>

using namespace eta::runtime::memory;
using namespace eta::runtime::memory::heap;
using namespace eta::runtime::memory::gc;
using namespace eta::runtime::nanbox;
using namespace eta::runtime::nanbox::ops;
using namespace eta::runtime::memory::factory;

namespace {
    template <typename T, typename E>
    T expect_ok(const std::expected<T,E>& r) {
        BOOST_REQUIRE(r.has_value());
        return *r;
    }
}

BOOST_AUTO_TEST_SUITE(gc_tests)

BOOST_AUTO_TEST_CASE(collects_unreachable_cons_cells) {
    Heap heap(1ull << 20);
    MarkSweepGC gc;
    std::vector<LispVal> roots;

    // Allocate 5 cons cells, none referenced in roots
    std::vector<LispVal> tmp;
    for (int i = 0; i < 5; ++i) {
        auto c = expect_ok(make_cons(heap, Nil));
        tmp.push_back(c);
    }

    GCStats stats{};
    gc.collect(heap, roots.begin(), roots.end(), &stats);

    BOOST_TEST(stats.objects_freed == 5);
}

BOOST_AUTO_TEST_CASE(retains_reachable_chain) {
    Heap heap(1ull << 20);
    MarkSweepGC gc;
    std::vector<LispVal> roots;

    // Build a chain of 3 cons cells and keep head in roots
    auto c3 = expect_ok(make_cons(heap, Nil));
    auto c2 = expect_ok(make_cons(heap, c3));
    auto c1 = expect_ok(make_cons(heap, c2));
    roots.push_back(c1);

    GCStats stats{};
    gc.collect(heap, roots.begin(), roots.end(), &stats);

    BOOST_TEST(stats.objects_freed == 0);
}

BOOST_AUTO_TEST_CASE(mixed_interpreted_procedure_graph) {
    Heap heap(1ull << 20);
    MarkSweepGC gc;
    std::vector<LispVal> roots;

    // Build some cons values and a lambda referencing them
    auto a = expect_ok(make_cons(heap, Nil));
    auto b = expect_ok(make_cons(heap, a));
    auto c = expect_ok(make_cons(heap, b));

    std::vector<LispVal> formals{ a, b };
    std::vector<LispVal> upvals{ c };
    auto proc = expect_ok(make_interpreted_procedure(heap, formals, b, upvals));

    // Root only the procedure; all referenced nodes should be retained
    roots.push_back(proc);

    GCStats stats{};
    gc.collect(heap, roots.begin(), roots.end(), &stats);
    BOOST_TEST(stats.objects_freed == 0);
}

BOOST_AUTO_TEST_CASE(fixnum_heap_leaf) {
    Heap heap(1ull << 20);
    MarkSweepGC gc;
    std::vector<LispVal> roots;

    // Force out-of-range to allocate Fixnum on heap via factory
    auto big = expect_ok(make_fixnum(heap, std::numeric_limits<int64_t>::max()));
    roots.push_back(big);

    GCStats stats{};
    gc.collect(heap, roots.begin(), roots.end(), &stats);
    // Should not traverse interior edges (leaf)
    BOOST_TEST(stats.objects_freed == 0);
}

BOOST_AUTO_TEST_CASE(stale_id_root_does_not_crash) {
    Heap heap(1ull << 20);
    MarkSweepGC gc;
    std::vector<LispVal> roots;

    auto c = expect_ok(make_cons(heap, Nil));

    // Extract object id from boxed value and deallocate manually
    const auto obj_id = static_cast<ObjectId>(payload(c));
    BOOST_REQUIRE(heap.deallocate(obj_id).has_value());

    // Keep stale boxed value as root
    roots.push_back(c);

    GCStats stats{};
    gc.collect(heap, roots.begin(), roots.end(), &stats);
    BOOST_TEST(true); // no crash
}

BOOST_AUTO_TEST_CASE(clear_marks_resets_mark_bits) {
    Heap heap(1ull << 20);
    MarkSweepGC gc;

    // Create a few objects
    for (int i = 0; i < 3; ++i) (void) expect_ok(make_cons(heap, Nil));

    // Manually set marks via with_entry
    heap.for_each_entry([&](ObjectId id, HeapEntry& e){ e.header.flags |= MARK_BIT; });

    // Now clear
    gc.clear_marks(heap);

    bool any_marked = false;
    heap.for_each_entry([&](ObjectId, HeapEntry& e){ if (e.header.flags & MARK_BIT) any_marked = true; });
    BOOST_TEST(!any_marked);
}

BOOST_AUTO_TEST_CASE(stats_sanity) {
    Heap heap(1ull << 20);
    MarkSweepGC gc;
    std::vector<LispVal> roots;

    auto c1 = expect_ok(make_cons(heap, Nil));
    (void)c1;
    auto c2 = expect_ok(make_cons(heap, Nil));
    (void)c2;

    GCStats stats{};
    const auto before = heap.total_bytes();
    gc.collect(heap, roots.begin(), roots.end(), &stats);
    BOOST_TEST(stats.bytes_before == before);
    BOOST_TEST(stats.bytes_after <= before);
}

BOOST_AUTO_TEST_CASE(retains_reachable_closure_vector_continuation) {
    Heap heap(1ull << 20);
    MarkSweepGC gc;
    std::vector<LispVal> roots;

    // 1. Vector
    auto v_el = expect_ok(make_cons(heap, Nil));
    auto vec = expect_ok(make_vector(heap, {v_el}));

    // 2. Closure
    auto c_up = expect_ok(make_cons(heap, Nil));
    auto closure = expect_ok(make_closure(heap, nullptr, {c_up}));

    // 3. Continuation
    auto cont_stack_el = expect_ok(make_cons(heap, Nil));
    auto cont_frame_closure = expect_ok(make_closure(heap, nullptr, {}));
    std::vector<eta::runtime::vm::Frame> frames = {
        {.func = nullptr, .pc = 0, .fp = 0, .closure = cont_frame_closure}
    };
    auto cont = expect_ok(make_continuation(heap, {cont_stack_el}, frames));

    // Root them all
    roots.push_back(vec);
    roots.push_back(closure);
    roots.push_back(cont);

    GCStats stats{};
    gc.collect(heap, roots.begin(), roots.end(), &stats);

    // Everything should be retained.
    // 6 objects: v_el, vec, c_up, closure, cont_stack_el, cont_frame_closure, cont.
    // wait, cont_frame_closure is 7th object.
    // Objects:
    // 1. v_el (cons)
    // 2. vec (vector)
    // 3. c_up (cons)
    // 4. closure (closure)
    // 5. cont_stack_el (cons)
    // 6. cont_frame_closure (closure)
    // 7. cont (continuation)
    BOOST_TEST(stats.objects_freed == 0);

    // Now unroot them
    roots.clear();
    gc.collect(heap, roots.begin(), roots.end(), &stats);
    BOOST_TEST(stats.objects_freed == 7);
}

BOOST_AUTO_TEST_SUITE_END()
