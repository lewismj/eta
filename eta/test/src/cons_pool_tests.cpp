#include <boost/test/unit_test.hpp>

#include <eta/runtime/memory/heap.h>
#include <eta/runtime/memory/cons_pool.h>
#include <eta/runtime/memory/mark_sweep_gc.h>
#include <eta/runtime/factory.h>

using namespace eta::runtime::memory::heap;
using namespace eta::runtime::memory::gc;
using namespace eta::runtime::nanbox;
using namespace eta::runtime::nanbox::ops;
using namespace eta::runtime::memory::factory;
using namespace eta::runtime::types;

namespace {
    template <typename T, typename E>
    T expect_ok(const std::expected<T,E>& r) {
        BOOST_REQUIRE(r.has_value());
        return *r;
    }
}

BOOST_AUTO_TEST_SUITE(cons_pool_tests)

/// basic allocation

BOOST_AUTO_TEST_CASE(alloc_returns_valid_id) {
    Heap heap(1ull << 20);
    auto& pool = heap.cons_pool();

    auto result = pool.alloc(Nil, Nil);
    BOOST_REQUIRE(result.has_value());
    BOOST_TEST(*result > 0);
}

BOOST_AUTO_TEST_CASE(alloc_stores_car_cdr) {
    Heap heap(1ull << 20);
    auto& pool = heap.cons_pool();

    auto car = encode(42.0).value();
    auto cdr = encode(3.14).value();

    auto id = expect_ok(pool.alloc(car, cdr));
    auto* cons = pool.try_get(id);
    BOOST_REQUIRE(cons != nullptr);
    BOOST_TEST(cons->car == car);
    BOOST_TEST(cons->cdr == cdr);
}

BOOST_AUTO_TEST_CASE(multiple_allocs_unique_ids) {
    Heap heap(1ull << 20);
    auto& pool = heap.cons_pool();

    auto id1 = expect_ok(pool.alloc(Nil, Nil));
    auto id2 = expect_ok(pool.alloc(Nil, Nil));
    auto id3 = expect_ok(pool.alloc(Nil, Nil));

    BOOST_TEST(id1 != id2);
    BOOST_TEST(id2 != id3);
    BOOST_TEST(id1 != id3);
}

/// ownership

BOOST_AUTO_TEST_CASE(owns_returns_true_for_pool_ids) {
    Heap heap(1ull << 20);
    auto& pool = heap.cons_pool();

    auto id = expect_ok(pool.alloc(Nil, Nil));
    BOOST_TEST(pool.owns(id));
}

BOOST_AUTO_TEST_CASE(owns_returns_false_for_general_heap_ids) {
    Heap heap(1ull << 20);
    auto& pool = heap.cons_pool();

    /// Force allocation on the general heap (bypass pool)
    auto gen_id = expect_ok(
        heap.allocate<Cons, ObjectKind::Cons>(Cons{.car = Nil, .cdr = Nil}));
    BOOST_TEST(!pool.owns(gen_id));
}

/// free-list recycling

BOOST_AUTO_TEST_CASE(free_list_recycle) {
    Heap heap(1ull << 20);
    auto& pool = heap.cons_pool();

    /// Allocate several cells
    std::vector<ObjectId> ids;
    for (int i = 0; i < 10; ++i) {
        ids.push_back(expect_ok(pool.alloc(Nil, Nil)));
    }

    auto live_before = pool.stats().live_count;

    /// Free two specific slots
    pool.free_slot(ids[3]);
    pool.free_slot(ids[7]);
    BOOST_TEST(pool.stats().live_count == live_before - 2);

    auto id_a = expect_ok(pool.alloc(Nil, Nil));
    auto id_b = expect_ok(pool.alloc(Nil, Nil));

    BOOST_TEST((id_a == ids[7] || id_a == ids[3]));
    BOOST_TEST((id_b == ids[7] || id_b == ids[3]));
    BOOST_TEST(id_a != id_b);
    BOOST_TEST(pool.stats().live_count == live_before);
}

BOOST_AUTO_TEST_CASE(try_get_returns_null_for_freed_slot) {
    Heap heap(1ull << 20);
    auto& pool = heap.cons_pool();

    auto id = expect_ok(pool.alloc(Nil, Nil));
    BOOST_REQUIRE(pool.try_get(id) != nullptr);

    pool.free_slot(id);
    BOOST_TEST(pool.try_get(id) == nullptr);
}

/// stats

BOOST_AUTO_TEST_CASE(stats_reflect_allocations) {
    Heap heap(1ull << 20);
    auto& pool = heap.cons_pool();

    auto before = pool.stats();

    expect_ok(pool.alloc(Nil, Nil));
    expect_ok(pool.alloc(Nil, Nil));
    expect_ok(pool.alloc(Nil, Nil));

    auto after = pool.stats();
    BOOST_TEST(after.live_count == before.live_count + 3);
    BOOST_TEST(after.free_count == before.free_count - 3);
    BOOST_TEST(after.bytes == after.live_count * sizeof(Cons));
}

/// slab growth

BOOST_AUTO_TEST_CASE(slab_growth_on_exhaustion) {
    Heap heap(1ull << 24);
    auto& pool = heap.cons_pool();

    auto initial_cap = pool.stats().capacity;

    /// Exhaust the initial slab
    for (std::size_t i = 0; i < initial_cap; ++i) {
        BOOST_REQUIRE(pool.alloc(Nil, Nil).has_value());
    }

    /// Next alloc triggers slab growth
    auto r = pool.alloc(Nil, Nil);
    BOOST_REQUIRE(r.has_value());

    BOOST_TEST(pool.stats().capacity > initial_cap);
    BOOST_TEST(pool.try_get(*r) != nullptr);
}

/// mark / sweep (pool-level)

BOOST_AUTO_TEST_CASE(clear_marks_clears_all) {
    Heap heap(1ull << 20);
    auto& pool = heap.cons_pool();

    auto id1 = expect_ok(pool.alloc(Nil, Nil));
    auto id2 = expect_ok(pool.alloc(Nil, Nil));

    pool.mark(id1);
    pool.mark(id2);
    pool.clear_marks();

    /// After clearing marks, sweep should free everything
    auto freed = pool.sweep();
    BOOST_TEST(freed == 2);
}

BOOST_AUTO_TEST_CASE(sweep_frees_unmarked_only) {
    Heap heap(1ull << 20);
    auto& pool = heap.cons_pool();

    auto id1 = expect_ok(pool.alloc(Nil, Nil));
    auto id2 = expect_ok(pool.alloc(Nil, Nil));
    auto id3 = expect_ok(pool.alloc(Nil, Nil));

    /// Mark only id2
    pool.mark(id2);

    auto freed = pool.sweep();
    BOOST_TEST(freed == 2);  ///< id1 and id3 freed

    BOOST_TEST(pool.try_get(id1) == nullptr);
    BOOST_TEST(pool.try_get(id2) != nullptr);
    BOOST_TEST(pool.try_get(id3) == nullptr);
}

/// try_mark

BOOST_AUTO_TEST_CASE(try_mark_returns_cons_on_first_call) {
    Heap heap(1ull << 20);
    auto& pool = heap.cons_pool();

    auto car = encode(1.0).value();
    auto cdr = encode(2.0).value();
    auto id = expect_ok(pool.alloc(car, cdr));

    /// First try_mark succeeds and returns Cons*
    auto* cons = pool.try_mark(id);
    BOOST_REQUIRE(cons != nullptr);
    BOOST_TEST(cons->car == car);
    BOOST_TEST(cons->cdr == cdr);

    /// Second try_mark returns nullptr (already marked)
    BOOST_TEST(pool.try_mark(id) == nullptr);
}

BOOST_AUTO_TEST_CASE(try_mark_returns_null_for_freed_slot) {
    Heap heap(1ull << 20);
    auto& pool = heap.cons_pool();

    auto id = expect_ok(pool.alloc(Nil, Nil));
    pool.free_slot(id);

    BOOST_TEST(pool.try_mark(id) == nullptr);
}

/// GC integration (pool + full mark-sweep)

BOOST_AUTO_TEST_CASE(gc_retains_reachable_pooled_cons) {
    Heap heap(1ull << 20);
    MarkSweepGC gc;
    std::vector<LispVal> roots;

    auto c1 = expect_ok(make_cons(heap, Nil));
    auto c2 = expect_ok(make_cons(heap, c1));
    roots.push_back(c2);

    GCStats stats{};
    gc.collect(heap, roots.begin(), roots.end(), &stats);

    BOOST_TEST(stats.objects_freed == 0);

    /// Verify the cons cells are still accessible in the pool
    auto id2 = static_cast<ObjectId>(payload(c2));
    auto* cons2 = heap.cons_pool().try_get(id2);
    BOOST_REQUIRE(cons2 != nullptr);
    BOOST_TEST(cons2->car == c1);
}

BOOST_AUTO_TEST_CASE(gc_collects_unreachable_pooled_cons) {
    Heap heap(1ull << 20);
    MarkSweepGC gc;
    std::vector<LispVal> roots;

    for (int i = 0; i < 100; ++i) {
        (void)expect_ok(make_cons(heap, Nil));
    }

    auto pool_live_before = heap.cons_pool().stats().live_count;

    GCStats stats{};
    gc.collect(heap, roots.begin(), roots.end(), &stats);

    BOOST_TEST(stats.objects_freed == 100u);
    BOOST_TEST(heap.cons_pool().stats().live_count == pool_live_before - 100);
}

BOOST_AUTO_TEST_CASE(gc_mixed_pool_and_general_heap) {
    Heap heap(1ull << 20);
    MarkSweepGC gc;
    std::vector<LispVal> roots;

    auto cons1 = expect_ok(make_cons(heap, Nil));        ///< pool
    auto vec   = expect_ok(make_vector(heap, {cons1}));   ///< general heap
    auto cons2 = expect_ok(make_cons(heap, vec));         ///< pool

    (void)expect_ok(make_cons(heap, Nil));

    roots.push_back(cons2);

    GCStats stats{};
    gc.collect(heap, roots.begin(), roots.end(), &stats);

    /// Only the single unreachable cons should be freed
    BOOST_TEST(stats.objects_freed == 1u);
}

BOOST_AUTO_TEST_CASE(gc_bytes_accounting_after_pool_sweep) {
    Heap heap(1ull << 20);
    MarkSweepGC gc;
    std::vector<LispVal> roots;

    for (int i = 0; i < 50; ++i) {
        (void)expect_ok(make_cons(heap, Nil));
    }

    auto bytes_before = heap.total_bytes();
    BOOST_TEST(bytes_before >= 50 * sizeof(Cons));

    GCStats stats{};
    gc.collect(heap, roots.begin(), roots.end(), &stats);

    BOOST_TEST(stats.bytes_after < stats.bytes_before);
    BOOST_TEST(heap.total_bytes() == stats.bytes_after);
}

BOOST_AUTO_TEST_CASE(gc_pooled_cons_reachable_from_closure) {
    Heap heap(1ull << 20);
    MarkSweepGC gc;
    std::vector<LispVal> roots;

    /// Cons cells captured as upvals in a closure
    auto a = expect_ok(make_cons(heap, Nil));
    auto b = expect_ok(make_cons(heap, a));
    auto closure = expect_ok(make_closure(heap, nullptr, {a, b}));

    roots.push_back(closure);

    GCStats stats{};
    gc.collect(heap, roots.begin(), roots.end(), &stats);
    BOOST_TEST(stats.objects_freed == 0u);

    roots.clear();
    gc.collect(heap, roots.begin(), roots.end(), &stats);
    BOOST_TEST(stats.objects_freed == 3u);  ///< 2 cons + 1 closure
}

BOOST_AUTO_TEST_SUITE_END()

