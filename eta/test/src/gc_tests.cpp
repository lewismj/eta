#include <boost/test/unit_test.hpp>

#include <eta/runtime/memory/heap.h>
#include <eta/runtime/memory/mark_sweep_gc.h>
#include <eta/runtime/factory.h>
#include <eta/runtime/port.h>

#include <eta/nng/nng_socket_ptr.h>
#include <eta/nng/nng_factory.h>

#ifndef ETA_TORCH_DEBUG_SKIP
#include <eta/torch/tensor_ptr.h>
#include <eta/torch/torch_factory.h>
#endif

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

/// existing basic tests

BOOST_AUTO_TEST_CASE(collects_unreachable_cons_cells) {
    Heap heap(1ull << 20);
    MarkSweepGC gc;
    std::vector<LispVal> roots;

    /// Allocate 5 cons cells, none referenced in roots
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

    /// Build a chain of 3 cons cells and keep head in roots
    auto c3 = expect_ok(make_cons(heap, Nil));
    auto c2 = expect_ok(make_cons(heap, c3));
    auto c1 = expect_ok(make_cons(heap, c2));
    roots.push_back(c1);

    GCStats stats{};
    gc.collect(heap, roots.begin(), roots.end(), &stats);

    BOOST_TEST(stats.objects_freed == 0);
}

BOOST_AUTO_TEST_CASE(mixed_closure_graph) {
    Heap heap(1ull << 20);
    MarkSweepGC gc;
    std::vector<LispVal> roots;

    /// Build some cons values and a closure referencing them as upvals
    auto a = expect_ok(make_cons(heap, Nil));
    auto b = expect_ok(make_cons(heap, a));
    auto c = expect_ok(make_cons(heap, b));

    std::vector<LispVal> upvals{ a, b, c };
    /// Create a closure with no function (nullptr) but with upvals
    auto closure = expect_ok(make_closure(heap, nullptr, upvals));

    /// Root only the closure; all referenced nodes should be retained
    roots.push_back(closure);

    GCStats stats{};
    gc.collect(heap, roots.begin(), roots.end(), &stats);
    BOOST_TEST(stats.objects_freed == 0);
}

BOOST_AUTO_TEST_CASE(fixnum_heap_leaf) {
    Heap heap(1ull << 20);
    MarkSweepGC gc;
    std::vector<LispVal> roots;

    /// Force out-of-range to allocate Fixnum on heap via factory
    auto big = expect_ok(make_fixnum(heap, std::numeric_limits<int64_t>::max()));
    roots.push_back(big);

    GCStats stats{};
    gc.collect(heap, roots.begin(), roots.end(), &stats);
    /// Should not traverse interior edges (leaf)
    BOOST_TEST(stats.objects_freed == 0);
}

BOOST_AUTO_TEST_CASE(stale_id_root_does_not_crash) {
    Heap heap(1ull << 20);
    MarkSweepGC gc;
    std::vector<LispVal> roots;

    auto c = expect_ok(make_cons(heap, Nil));

    /// Extract object id from boxed value and deallocate manually
    const auto obj_id = static_cast<ObjectId>(payload(c));
    BOOST_REQUIRE(heap.deallocate(obj_id).has_value());

    /// Keep stale boxed value as root
    roots.push_back(c);

    GCStats stats{};
    gc.collect(heap, roots.begin(), roots.end(), &stats);
    BOOST_TEST(true); ///< no crash
}

BOOST_AUTO_TEST_CASE(clear_marks_resets_mark_bits) {
    Heap heap(1ull << 20);
    MarkSweepGC gc;

    /// Create a few objects
    for (int i = 0; i < 3; ++i) (void) expect_ok(make_cons(heap, Nil));

    /// Manually set marks via with_entry
    heap.for_each_entry([&](ObjectId id, HeapEntry& e){ e.header.flags |= MARK_BIT; });

    /// Now clear
    gc.clear_marks(heap);

    bool any_marked = false;
    heap.for_each_entry([&](ObjectId, HeapEntry& e){ if (e.header.flags & MARK_BIT) any_marked = true; });
    BOOST_TEST(!any_marked);
}

BOOST_AUTO_TEST_CASE(dead_finalized_object_is_queued_and_rescued) {
    Heap heap(1ull << 20);
    MarkSweepGC gc;
    std::vector<LispVal> roots;

    auto obj = expect_ok(make_vector(heap, {}));
    auto proc = expect_ok(make_primitive(
        heap,
        [](const std::vector<LispVal>&) -> std::expected<LispVal, eta::runtime::error::RuntimeError> {
            return Nil;
        },
        1,
        false
    ));

    const auto obj_id = static_cast<ObjectId>(payload(obj));
    const auto proc_id = static_cast<ObjectId>(payload(proc));
    BOOST_REQUIRE(heap.register_finalizer(obj_id, proc).has_value());

    GCStats stats{};
    gc.collect(heap, roots.begin(), roots.end(), &stats);

    BOOST_TEST(stats.objects_freed == 0u);
    BOOST_TEST(heap.pending_finalizer_count() == 1u);
    BOOST_TEST(!heap.fetch_finalizer(obj_id).has_value());

    HeapEntry entry{};
    BOOST_TEST(heap.try_get(obj_id, entry));
    BOOST_TEST(heap.try_get(proc_id, entry));

    auto pending = heap.dequeue_pending_finalizer();
    BOOST_REQUIRE(pending.has_value());
    BOOST_TEST(pending->obj == obj);
    BOOST_TEST(pending->proc == proc);
}

BOOST_AUTO_TEST_CASE(finalizer_table_is_ephemeron_like_for_keys) {
    Heap heap(1ull << 20);
    MarkSweepGC gc;
    std::vector<LispVal> roots;

    auto obj = expect_ok(make_vector(heap, {}));
    auto proc = expect_ok(make_primitive(
        heap,
        [](const std::vector<LispVal>&) -> std::expected<LispVal, eta::runtime::error::RuntimeError> {
            return Nil;
        },
        1,
        false
    ));

    const auto obj_id = static_cast<ObjectId>(payload(obj));
    BOOST_REQUIRE(heap.register_finalizer(obj_id, proc).has_value());

    /// Root only the finalizer procedure, not the key object.
    roots.push_back(proc);

    GCStats stats{};
    gc.collect(heap, roots.begin(), roots.end(), &stats);

    BOOST_TEST(stats.objects_freed == 0u);
    BOOST_TEST(heap.pending_finalizer_count() == 1u);
    BOOST_TEST(!heap.fetch_finalizer(obj_id).has_value());

    auto pending = heap.dequeue_pending_finalizer();
    BOOST_REQUIRE(pending.has_value());
    BOOST_TEST(pending->obj == obj);
    BOOST_TEST(pending->proc == proc);
}

BOOST_AUTO_TEST_CASE(pending_finalizers_survive_subsequent_gc_cycles) {
    Heap heap(1ull << 20);
    MarkSweepGC gc;
    std::vector<LispVal> roots;

    auto obj = expect_ok(make_vector(heap, {}));
    auto proc = expect_ok(make_primitive(
        heap,
        [](const std::vector<LispVal>&) -> std::expected<LispVal, eta::runtime::error::RuntimeError> {
            return Nil;
        },
        1,
        false
    ));

    const auto obj_id = static_cast<ObjectId>(payload(obj));
    const auto proc_id = static_cast<ObjectId>(payload(proc));
    BOOST_REQUIRE(heap.register_finalizer(obj_id, proc).has_value());

    GCStats first{};
    gc.collect(heap, roots.begin(), roots.end(), &first);
    BOOST_TEST(first.objects_freed == 0u);
    BOOST_TEST(heap.pending_finalizer_count() == 1u);

    GCStats second{};
    gc.collect(heap, roots.begin(), roots.end(), &second);
    BOOST_TEST(second.objects_freed == 0u);
    BOOST_TEST(heap.pending_finalizer_count() == 1u);

    HeapEntry entry{};
    BOOST_TEST(heap.try_get(obj_id, entry));
    BOOST_TEST(heap.try_get(proc_id, entry));
}

BOOST_AUTO_TEST_CASE(guardian_track_gc_enqueues_ready_object) {
    Heap heap(1ull << 20);
    MarkSweepGC gc;
    std::vector<LispVal> roots;

    auto guardian = expect_ok(make_guardian(heap));
    auto obj = expect_ok(make_vector(heap, {}));

    const auto guardian_id = static_cast<ObjectId>(payload(guardian));
    const auto obj_id = static_cast<ObjectId>(payload(obj));
    BOOST_REQUIRE(heap.guardian_track(guardian_id, obj_id).has_value());
    roots.push_back(guardian);

    GCStats first{};
    gc.collect(heap, roots.begin(), roots.end(), &first);

    BOOST_TEST(first.objects_freed == 0u);
    BOOST_TEST(heap.guardian_tracking_snapshot().empty());

    HeapEntry entry{};
    BOOST_TEST(heap.try_get(obj_id, entry));

    auto collected = heap.dequeue_guardian_ready(guardian_id);
    BOOST_REQUIRE(collected.has_value());
    BOOST_TEST(*collected == obj);
    BOOST_TEST(!heap.dequeue_guardian_ready(guardian_id).has_value());

    GCStats second{};
    gc.collect(heap, roots.begin(), roots.end(), &second);
    BOOST_TEST(second.objects_freed == 1u);
    BOOST_TEST(!heap.try_get(obj_id, entry));
}

BOOST_AUTO_TEST_CASE(dead_guardian_does_not_retain_tracked_objects) {
    Heap heap(1ull << 20);
    MarkSweepGC gc;
    std::vector<LispVal> roots;

    auto guardian = expect_ok(make_guardian(heap));
    auto obj = expect_ok(make_vector(heap, {}));

    const auto guardian_id = static_cast<ObjectId>(payload(guardian));
    const auto obj_id = static_cast<ObjectId>(payload(obj));
    BOOST_REQUIRE(heap.guardian_track(guardian_id, obj_id).has_value());

    GCStats stats{};
    gc.collect(heap, roots.begin(), roots.end(), &stats);

    BOOST_TEST(stats.objects_freed == 2u);

    HeapEntry entry{};
    BOOST_TEST(!heap.try_get(guardian_id, entry));
    BOOST_TEST(!heap.try_get(obj_id, entry));
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

    /// 1. Vector
    auto v_el = expect_ok(make_cons(heap, Nil));
    auto vec = expect_ok(make_vector(heap, {v_el}));

    /// 2. Closure
    auto c_up = expect_ok(make_cons(heap, Nil));
    auto closure = expect_ok(make_closure(heap, nullptr, {c_up}));

    /// 3. Continuation
    auto cont_stack_el = expect_ok(make_cons(heap, Nil));
    auto cont_frame_closure = expect_ok(make_closure(heap, nullptr, {}));
    std::vector<eta::runtime::vm::Frame> frames = {
        {.func = nullptr, .pc = 0, .fp = 0, .closure = cont_frame_closure}
    };
    std::vector<eta::runtime::vm::WindFrame> winding_stack = {};
    auto cont = expect_ok(make_continuation(heap, {cont_stack_el}, frames, winding_stack));

    /// Root them all
    roots.push_back(vec);
    roots.push_back(closure);
    roots.push_back(cont);

    GCStats stats{};
    gc.collect(heap, roots.begin(), roots.end(), &stats);

    /**
     * Everything should be retained.
     * Objects:
     * 1. v_el (cons)
     * 2. vec (vector)
     * 3. c_up (cons)
     * 4. closure (closure)
     * 5. cont_stack_el (cons)
     * 6. cont_frame_closure (closure)
     * 7. cont (continuation)
     */
    BOOST_TEST(stats.objects_freed == 0);

    /// Now unroot them
    roots.clear();
    gc.collect(heap, roots.begin(), roots.end(), &stats);
    BOOST_TEST(stats.objects_freed == 7);
}

/// per-type GC traversal: LogicVar

BOOST_AUTO_TEST_CASE(retains_bound_logic_var_target) {
    Heap heap(1ull << 20);
    MarkSweepGC gc;
    std::vector<LispVal> roots;

    /// Create a cons cell and bind it to a LogicVar
    auto inner = expect_ok(make_cons(heap, Nil));
    auto lv = expect_ok(make_logic_var(heap));

    /// Bind the logic var to the inner cons cell
    auto lv_id = static_cast<ObjectId>(payload(lv));
    auto* lv_ptr = heap.try_get_as<ObjectKind::LogicVar, eta::runtime::types::LogicVar>(lv_id);
    BOOST_REQUIRE(lv_ptr != nullptr);
    lv_ptr->binding = inner;

    /// Root only the logic var
    roots.push_back(lv);

    GCStats stats{};
    gc.collect(heap, roots.begin(), roots.end(), &stats);
    /// inner cons must be retained via the binding
    BOOST_TEST(stats.objects_freed == 0);
}

BOOST_AUTO_TEST_CASE(collects_unbound_logic_var) {
    Heap heap(1ull << 20);
    MarkSweepGC gc;
    std::vector<LispVal> roots;

    /// Unreachable unbound logic var
    (void)expect_ok(make_logic_var(heap));

    GCStats stats{};
    gc.collect(heap, roots.begin(), roots.end(), &stats);
    BOOST_TEST(stats.objects_freed == 1);
}

BOOST_AUTO_TEST_CASE(collects_logic_var_binding_chain) {
    Heap heap(1ull << 20);
    MarkSweepGC gc;
    std::vector<LispVal> roots;

    /// LogicVar -> cons -> cons chain; root lv, all reachable
    auto c2 = expect_ok(make_cons(heap, Nil));
    auto c1 = expect_ok(make_cons(heap, c2));
    auto lv = expect_ok(make_logic_var(heap));

    auto lv_id = static_cast<ObjectId>(payload(lv));
    auto* lv_ptr = heap.try_get_as<ObjectKind::LogicVar, eta::runtime::types::LogicVar>(lv_id);
    BOOST_REQUIRE(lv_ptr != nullptr);
    lv_ptr->binding = c1;

    roots.push_back(lv);

    GCStats stats{};
    gc.collect(heap, roots.begin(), roots.end(), &stats);
    BOOST_TEST(stats.objects_freed == 0);

    roots.clear();
    gc.collect(heap, roots.begin(), roots.end(), &stats);
    BOOST_TEST(stats.objects_freed == 3);
}

/// per-type GC traversal: MultipleValues

BOOST_AUTO_TEST_CASE(retains_multiple_values_elements) {
    Heap heap(1ull << 20);
    MarkSweepGC gc;
    std::vector<LispVal> roots;

    auto a = expect_ok(make_cons(heap, Nil));
    auto b = expect_ok(make_cons(heap, Nil));
    auto mv = expect_ok(make_multiple_values(heap, {a, b}));

    roots.push_back(mv);

    GCStats stats{};
    gc.collect(heap, roots.begin(), roots.end(), &stats);
    BOOST_TEST(stats.objects_freed == 0);

    roots.clear();
    gc.collect(heap, roots.begin(), roots.end(), &stats);
    BOOST_TEST(stats.objects_freed == 3); ///< mv + a + b
}

/// per-type GC traversal: Primitive with gc_roots

BOOST_AUTO_TEST_CASE(retains_primitive_gc_roots) {
    Heap heap(1ull << 20);
    MarkSweepGC gc;
    std::vector<LispVal> roots;

    auto captured = expect_ok(make_cons(heap, Nil));

    /// Create a primitive with a captured heap object in gc_roots
    auto prim = expect_ok(make_primitive(
        heap,
        [](const std::vector<LispVal>&) -> std::expected<LispVal, eta::runtime::error::RuntimeError> {
            return Nil;
        },
        0,          ///< arity
        false,      ///< has_rest
        {captured}  ///< gc_roots
    ));

    roots.push_back(prim);

    GCStats stats{};
    gc.collect(heap, roots.begin(), roots.end(), &stats);
    BOOST_TEST(stats.objects_freed == 0);

    roots.clear();
    gc.collect(heap, roots.begin(), roots.end(), &stats);
    BOOST_TEST(stats.objects_freed == 2); ///< prim + captured cons
}

/// per-type GC traversal: FactTable

BOOST_AUTO_TEST_CASE(retains_fact_table_column_values) {
    Heap heap(1ull << 20);
    MarkSweepGC gc;
    std::vector<LispVal> roots;

    auto ft = expect_ok(make_fact_table(heap, {"col_a", "col_b"}));
    auto ft_id = static_cast<ObjectId>(payload(ft));
    auto* ft_ptr = heap.try_get_as<ObjectKind::FactTable, eta::runtime::types::FactTable>(ft_id);
    BOOST_REQUIRE(ft_ptr != nullptr);

    /// Insert rows containing heap-allocated cons cells
    auto v1 = expect_ok(make_cons(heap, Nil));
    auto v2 = expect_ok(make_cons(heap, Nil));
    auto v3 = expect_ok(make_cons(heap, Nil));

    BOOST_REQUIRE(ft_ptr->add_row({v1, v2}));
    BOOST_REQUIRE(ft_ptr->add_row({v3, Nil}));

    roots.push_back(ft);

    GCStats stats{};
    gc.collect(heap, roots.begin(), roots.end(), &stats);
    /// ft + v1 + v2 + v3 all retained
    BOOST_TEST(stats.objects_freed == 0);

    roots.clear();
    gc.collect(heap, roots.begin(), roots.end(), &stats);
    BOOST_TEST(stats.objects_freed == 4); ///< ft + v1 + v2 + v3
}

/// per-type GC traversal: Closure with func->constants

BOOST_AUTO_TEST_CASE(retains_closure_func_constants) {
    Heap heap(1ull << 20);
    MarkSweepGC gc;
    std::vector<LispVal> roots;

    /// Build a minimal BytecodeFunction whose constants vector has a heap ref
    auto const_cons = expect_ok(make_cons(heap, Nil));

    eta::runtime::vm::BytecodeFunction func;
    func.name = "test_func";
    func.constants.push_back(const_cons);

    auto closure = expect_ok(make_closure(heap, &func, {}));
    roots.push_back(closure);

    GCStats stats{};
    gc.collect(heap, roots.begin(), roots.end(), &stats);
    /// closure + const_cons both retained
    BOOST_TEST(stats.objects_freed == 0);
}

/// leaf types as roots

BOOST_AUTO_TEST_CASE(bytevector_leaf_retained) {
    Heap heap(1ull << 20);
    MarkSweepGC gc;
    std::vector<LispVal> roots;

    auto bv = expect_ok(make_bytevector(heap, {0x01, 0x02, 0x03}));
    roots.push_back(bv);

    GCStats stats{};
    gc.collect(heap, roots.begin(), roots.end(), &stats);
    BOOST_TEST(stats.objects_freed == 0);
}

BOOST_AUTO_TEST_CASE(port_leaf_retained) {
    Heap heap(1ull << 20);
    MarkSweepGC gc;
    std::vector<LispVal> roots;

    auto port = std::make_shared<eta::runtime::StringPort>(
        eta::runtime::StringPort::Mode::Output);
    auto port_val = expect_ok(make_port(heap, port));
    roots.push_back(port_val);

    GCStats stats{};
    gc.collect(heap, roots.begin(), roots.end(), &stats);
    BOOST_TEST(stats.objects_freed == 0);
}

BOOST_AUTO_TEST_CASE(tape_leaf_retained) {
    Heap heap(1ull << 20);
    MarkSweepGC gc;
    std::vector<LispVal> roots;

    auto tape = expect_ok(make_tape(heap));
    roots.push_back(tape);

    GCStats stats{};
    gc.collect(heap, roots.begin(), roots.end(), &stats);
    BOOST_TEST(stats.objects_freed == 0);
}

BOOST_AUTO_TEST_CASE(unreachable_leaf_types_collected) {
    Heap heap(1ull << 20);
    MarkSweepGC gc;
    std::vector<LispVal> roots;

    (void)expect_ok(make_bytevector(heap, {0xAA}));
    (void)expect_ok(make_tape(heap));
    auto port = std::make_shared<eta::runtime::StringPort>(
        eta::runtime::StringPort::Mode::Output);
    (void)expect_ok(make_port(heap, port));

    GCStats stats{};
    gc.collect(heap, roots.begin(), roots.end(), &stats);
    BOOST_TEST(stats.objects_freed == 3);
}

/// continuation sub-fields: winding_stack

BOOST_AUTO_TEST_CASE(retains_continuation_winding_stack_refs) {
    Heap heap(1ull << 20);
    MarkSweepGC gc;
    std::vector<LispVal> roots;

    /// Create closures for wind frame before/body/after
    auto before_closure = expect_ok(make_closure(heap, nullptr, {}));
    auto body_closure   = expect_ok(make_closure(heap, nullptr, {}));
    auto after_closure  = expect_ok(make_closure(heap, nullptr, {}));

    std::vector<eta::runtime::vm::Frame> frames = {};
    std::vector<eta::runtime::vm::WindFrame> winding = {
        {.before = before_closure, .body = body_closure, .after = after_closure}
    };
    auto cont = expect_ok(make_continuation(heap, {}, frames, winding));

    roots.push_back(cont);

    GCStats stats{};
    gc.collect(heap, roots.begin(), roots.end(), &stats);
    /// cont + 3 closures all retained
    BOOST_TEST(stats.objects_freed == 0);

    roots.clear();
    gc.collect(heap, roots.begin(), roots.end(), &stats);
    BOOST_TEST(stats.objects_freed == 4); ///< cont + 3 closures
}

/// continuation sub-fields: frame.extra

BOOST_AUTO_TEST_CASE(retains_continuation_frame_extra) {
    Heap heap(1ull << 20);
    MarkSweepGC gc;
    std::vector<LispVal> roots;

    auto extra_cons = expect_ok(make_cons(heap, Nil));
    auto frame_closure = expect_ok(make_closure(heap, nullptr, {}));

    std::vector<eta::runtime::vm::Frame> frames = {
        {.func = nullptr, .pc = 0, .fp = 0, .closure = frame_closure,
         .kind = eta::runtime::vm::FrameKind::Normal, .extra = extra_cons}
    };
    std::vector<eta::runtime::vm::WindFrame> winding = {};
    auto cont = expect_ok(make_continuation(heap, {}, frames, winding));

    roots.push_back(cont);

    GCStats stats{};
    gc.collect(heap, roots.begin(), roots.end(), &stats);
    /// cont + frame_closure + extra_cons all retained
    BOOST_TEST(stats.objects_freed == 0);

    roots.clear();
    gc.collect(heap, roots.begin(), roots.end(), &stats);
    BOOST_TEST(stats.objects_freed == 3);
}

/// GC pause mechanism

BOOST_AUTO_TEST_CASE(pause_for_gc_rejects_allocations) {
    Heap heap(1ull << 20);

    /// Pause the heap as if GC were in progress
    heap.pause_for_gc();
    BOOST_TEST(heap.is_gc_paused());

    /// General-heap allocation should fail with GCInProgress
    auto result = heap.allocate<eta::runtime::types::Cons, ObjectKind::Cons>(
        eta::runtime::types::Cons{.car = Nil, .cdr = Nil});
    BOOST_REQUIRE(!result.has_value());
    BOOST_TEST(result.error() == HeapError::GCInProgress);

    /// Resume and verify allocation works again
    heap.resume_after_gc();
    BOOST_TEST(!heap.is_gc_paused());

    auto result2 = heap.allocate<eta::runtime::types::Cons, ObjectKind::Cons>(
        eta::runtime::types::Cons{.car = Nil, .cdr = Nil});
    BOOST_TEST(result2.has_value());
}

/// GC callback on soft-limit

BOOST_AUTO_TEST_CASE(gc_callback_frees_memory_on_soft_limit) {
    /**
     * Use a very small heap.  The soft-limit check is strict >, so set the
     * limit to exactly 3 cons cells so that the 4th allocation exceeds it.
     */
    const auto cons_sz = sizeof(eta::runtime::types::Cons);
    Heap heap(cons_sz * 3);
    MarkSweepGC gc;
    std::vector<LispVal> roots;

    for (int i = 0; i < 3; ++i)
        (void)expect_ok(make_cons(heap, Nil));

    /// Install a callback that runs GC (with empty roots, so everything is freed)
    bool callback_invoked = false;
    heap.set_gc_callback([&]() {
        callback_invoked = true;
        gc.collect(heap, roots.begin(), roots.end());
    });

    /**
     * This allocation would exceed the soft limit but the callback should free
     * the 3 unreachable cons cells first, making room.
     */
    auto result = make_cons(heap, Nil);
    BOOST_TEST(callback_invoked);
    BOOST_TEST(result.has_value());
}

/// multiple GC cycles

BOOST_AUTO_TEST_CASE(multi_cycle_stability) {
    Heap heap(1ull << 20);
    MarkSweepGC gc;
    std::vector<LispVal> roots;

    for (int cycle = 0; cycle < 5; ++cycle) {
        /// Free leftover roots from the previous cycle first
        roots.clear();
        GCStats cleanup{};
        gc.collect(heap, roots.begin(), roots.end(), &cleanup);

        /// Each cycle: allocate 20 cons cells, root only the first 5
        std::vector<LispVal> batch;
        for (int i = 0; i < 20; ++i) {
            auto c = expect_ok(make_cons(heap, Nil));
            batch.push_back(c);
            if (i < 5) roots.push_back(c);
        }

        GCStats stats{};
        gc.collect(heap, roots.begin(), roots.end(), &stats);
        BOOST_TEST(stats.objects_freed == 15u);
    }

    /// Final full collect with empty roots should free the remaining 5
    roots.clear();
    GCStats stats{};
    gc.collect(heap, roots.begin(), roots.end(), &stats);
    BOOST_TEST(stats.objects_freed == 5u);
}

BOOST_AUTO_TEST_CASE(allocate_between_collections) {
    Heap heap(1ull << 20);
    MarkSweepGC gc;
    std::vector<LispVal> roots;

    /// Cycle 1: allocate and collect everything
    for (int i = 0; i < 10; ++i) (void)expect_ok(make_cons(heap, Nil));
    GCStats stats{};
    gc.collect(heap, roots.begin(), roots.end(), &stats);
    BOOST_TEST(stats.objects_freed == 10u);
    BOOST_TEST(heap.total_bytes() == 0u);

    /// Cycle 2: allocate new objects, root some, collect
    auto rooted = expect_ok(make_cons(heap, Nil));
    roots.push_back(rooted);
    for (int i = 0; i < 5; ++i) (void)expect_ok(make_cons(heap, Nil));

    gc.collect(heap, roots.begin(), roots.end(), &stats);
    BOOST_TEST(stats.objects_freed == 5u);
    BOOST_TEST(heap.total_bytes() > 0u);
}

/// cyclic graph

BOOST_AUTO_TEST_CASE(cyclic_cons_does_not_loop) {
    Heap heap(1ull << 20);
    MarkSweepGC gc;
    std::vector<LispVal> roots;

    /// Create two cons cells and wire them into a cycle: a.cdr -> b, b.cdr -> a
    auto a = expect_ok(make_cons(heap, Nil));
    auto b = expect_ok(make_cons(heap, Nil));

    /// Patch cdr of a to point to b, and cdr of b to point to a
    auto a_id = static_cast<ObjectId>(payload(a));
    auto b_id = static_cast<ObjectId>(payload(b));

    auto* a_cons = heap.cons_pool().try_get(a_id);
    auto* b_cons = heap.cons_pool().try_get(b_id);
    BOOST_REQUIRE(a_cons != nullptr);
    BOOST_REQUIRE(b_cons != nullptr);
    a_cons->cdr = b;
    b_cons->cdr = a;

    roots.push_back(a);

    GCStats stats{};
    gc.collect(heap, roots.begin(), roots.end(), &stats);
    BOOST_TEST(stats.objects_freed == 0);

    roots.clear();
    gc.collect(heap, roots.begin(), roots.end(), &stats);
    BOOST_TEST(stats.objects_freed == 2);
}

/// deep chain stress

BOOST_AUTO_TEST_CASE(deep_cons_chain_retained) {
    Heap heap(1ull << 22);
    MarkSweepGC gc;
    std::vector<LispVal> roots;

    constexpr int DEPTH = 1000;
    auto tail = Nil;
    LispVal head = Nil;
    for (int i = 0; i < DEPTH; ++i) {
        head = expect_ok(make_cons(heap, tail));
        tail = head;
    }

    roots.push_back(head);

    GCStats stats{};
    gc.collect(heap, roots.begin(), roots.end(), &stats);
    BOOST_TEST(stats.objects_freed == 0);

    roots.clear();
    gc.collect(heap, roots.begin(), roots.end(), &stats);
    BOOST_TEST(stats.objects_freed == static_cast<std::size_t>(DEPTH));
}

/// vector with nested heap objects

BOOST_AUTO_TEST_CASE(vector_with_nested_vectors) {
    Heap heap(1ull << 20);
    MarkSweepGC gc;
    std::vector<LispVal> roots;

    auto inner1 = expect_ok(make_cons(heap, Nil));
    auto inner2 = expect_ok(make_cons(heap, Nil));
    auto inner_vec = expect_ok(make_vector(heap, {inner1, inner2}));
    auto outer_vec = expect_ok(make_vector(heap, {inner_vec}));

    roots.push_back(outer_vec);

    GCStats stats{};
    gc.collect(heap, roots.begin(), roots.end(), &stats);
    /// outer_vec -> inner_vec -> {inner1, inner2}: all 4 retained
    BOOST_TEST(stats.objects_freed == 0);

    roots.clear();
    gc.collect(heap, roots.begin(), roots.end(), &stats);
    BOOST_TEST(stats.objects_freed == 4);
}

/// mixed type graph

BOOST_AUTO_TEST_CASE(mixed_type_interconnected_graph) {
    Heap heap(1ull << 20);
    MarkSweepGC gc;
    std::vector<LispVal> roots;

    /// Build a graph: vector -> closure -> cons -> logic_var(bound to cons)
    auto leaf_cons = expect_ok(make_cons(heap, Nil));

    auto lv = expect_ok(make_logic_var(heap));
    auto lv_id = static_cast<ObjectId>(payload(lv));
    auto* lv_ptr = heap.try_get_as<ObjectKind::LogicVar, eta::runtime::types::LogicVar>(lv_id);
    BOOST_REQUIRE(lv_ptr != nullptr);
    lv_ptr->binding = leaf_cons;

    auto cons_with_lv = expect_ok(make_cons(heap, lv));
    auto closure = expect_ok(make_closure(heap, nullptr, {cons_with_lv}));
    auto vec = expect_ok(make_vector(heap, {closure}));

    /// Also create unreachable garbage
    (void)expect_ok(make_cons(heap, Nil));
    (void)expect_ok(make_cons(heap, Nil));

    roots.push_back(vec);

    GCStats stats{};
    gc.collect(heap, roots.begin(), roots.end(), &stats);
    /// 2 unreachable cons freed; vec+closure+cons_with_lv+lv+leaf_cons retained
    BOOST_TEST(stats.objects_freed == 2);
}

/// multiple roots sharing subgraph

BOOST_AUTO_TEST_CASE(shared_subgraph_from_multiple_roots) {
    Heap heap(1ull << 20);
    MarkSweepGC gc;
    std::vector<LispVal> roots;

    /// Shared node
    auto shared = expect_ok(make_cons(heap, Nil));

    /// Two vectors both reference the shared cons
    auto vec1 = expect_ok(make_vector(heap, {shared}));
    auto vec2 = expect_ok(make_vector(heap, {shared}));

    roots.push_back(vec1);
    roots.push_back(vec2);

    GCStats stats{};
    gc.collect(heap, roots.begin(), roots.end(), &stats);
    BOOST_TEST(stats.objects_freed == 0);

    roots.erase(roots.begin()); ///< remove vec1
    gc.collect(heap, roots.begin(), roots.end(), &stats);
    BOOST_TEST(stats.objects_freed == 1); ///< only vec1 freed
}

/// span convenience overload

BOOST_AUTO_TEST_CASE(collect_with_span) {
    Heap heap(1ull << 20);
    MarkSweepGC gc;

    auto a = expect_ok(make_cons(heap, Nil));
    (void)expect_ok(make_cons(heap, Nil)); ///< unreachable

    std::vector<LispVal> roots_vec = {a};
    std::span<const LispVal> roots_span(roots_vec);

    GCStats stats{};
    gc.collect(heap, roots_span, &stats);
    BOOST_TEST(stats.objects_freed == 1);
}

/// callback-based root enumeration

BOOST_AUTO_TEST_CASE(collect_with_callback_enumeration) {
    Heap heap(1ull << 20);
    MarkSweepGC gc;

    auto a = expect_ok(make_cons(heap, Nil));
    auto b = expect_ok(make_cons(heap, Nil));
    (void)expect_ok(make_cons(heap, Nil)); ///< unreachable

    GCStats stats{};
    gc.collect(heap, [&](auto&& visit) {
        visit(a);
        visit(b);
    }, &stats);

    BOOST_TEST(stats.objects_freed == 1);
}

/// non-heap roots ignored gracefully

BOOST_AUTO_TEST_CASE(non_heap_roots_ignored) {
    Heap heap(1ull << 20);
    MarkSweepGC gc;

    /// Roots that are immediate values, not heap pointers
    auto flo = encode(3.14).value();
    auto fix = encode(42).value();
    std::vector<LispVal> roots = {Nil, True, False, flo, fix};

    (void)expect_ok(make_cons(heap, Nil)); ///< unreachable

    GCStats stats{};
    gc.collect(heap, roots.begin(), roots.end(), &stats);
    /// Only the unreachable cons freed; immediate roots don't crash anything
    BOOST_TEST(stats.objects_freed == 1);
}


/**
 * Verifies that an unreachable NngSocket heap entry is swept by GC and its
 * type-erased destructor (which calls nng_close) is invoked.  closed=true
 * prevents nng_close from being called on an uninitialised socket handle.
 */
BOOST_AUTO_TEST_CASE(nng_socket_unreachable_swept) {
    Heap heap(1ull << 20);
    MarkSweepGC gc;
    std::vector<LispVal> roots;

    eta::nng::NngSocketPtr sp{};
    sp.closed = true; ///< guard: do not call nng_close on an uninitialised fd
    auto sock_val = expect_ok(eta::nng::factory::make_nng_socket(heap, std::move(sp)));
    const auto sock_id = static_cast<ObjectId>(payload(sock_val));

    GCStats stats{};
    gc.collect(heap, roots.begin(), roots.end(), &stats);
    BOOST_TEST(stats.objects_freed == 1);

    /// The heap entry must be absent after the sweep.
    HeapEntry e{};
    BOOST_TEST(!heap.try_get(sock_id, e));
}

/// Verifies that a rooted NngSocket survives GC while an unrooted one is freed.
BOOST_AUTO_TEST_CASE(nng_socket_rooted_retained) {
    Heap heap(1ull << 20);
    MarkSweepGC gc;
    std::vector<LispVal> roots;

    auto make_closed_sp = []() {
        eta::nng::NngSocketPtr sp{};
        sp.closed = true;
        return sp;
    };

    auto rooted_sock  = expect_ok(eta::nng::factory::make_nng_socket(heap, make_closed_sp()));
    auto garbage_sock = expect_ok(eta::nng::factory::make_nng_socket(heap, make_closed_sp()));
    (void)garbage_sock; ///< not rooted

    roots.push_back(rooted_sock);

    GCStats stats{};
    gc.collect(heap, roots.begin(), roots.end(), &stats);
    BOOST_TEST(stats.objects_freed == 1); ///< only garbage_sock freed

    /// Rooted socket is still accessible.
    const auto id = static_cast<ObjectId>(payload(rooted_sock));
    auto* retained_ptr = heap.try_get_as<ObjectKind::NngSocket, eta::nng::NngSocketPtr>(id);
    BOOST_TEST(retained_ptr != nullptr);
}


/**
 * Verifies that objects pushed into an ExternalRootFrame survive GC while the
 * frame is live, and are swept after the frame goes out of scope.
 */
BOOST_AUTO_TEST_CASE(external_root_frame_roots_object) {
    Heap heap(1ull << 20);
    MarkSweepGC gc;

    auto cons1 = expect_ok(make_cons(heap, Nil));

    {
        auto frame = heap.make_external_root_frame();
        frame.push(cons1);

        GCStats stats1{};
        gc.collect(heap, heap.external_roots().begin(), heap.external_roots().end(), &stats1);
        BOOST_TEST(stats1.objects_freed == 0);
    } ///< frame destructor: external_roots resized back to 0

    std::vector<LispVal> empty;
    GCStats stats2{};
    gc.collect(heap, empty.begin(), empty.end(), &stats2);
    BOOST_TEST(stats2.objects_freed == 1);
}

/**
 * Verifies that nested ExternalRootFrames form a stack: the inner frame pops
 * only its own roots on destruction, leaving the outer frame's roots intact.
 */
BOOST_AUTO_TEST_CASE(external_root_frame_nested_frames) {
    Heap heap(1ull << 20);
    MarkSweepGC gc;

    auto cons1 = expect_ok(make_cons(heap, Nil));
    auto cons2 = expect_ok(make_cons(heap, Nil));

    {
        auto outer = heap.make_external_root_frame();
        outer.push(cons1);

        {
            auto inner = heap.make_external_root_frame();
            inner.push(cons2);

            /// Both cons1 and cons2 are rooted: neither freed.
            GCStats s1{};
            gc.collect(heap, heap.external_roots().begin(), heap.external_roots().end(), &s1);
            BOOST_TEST(s1.objects_freed == 0);
        } ///< inner destructor: external_roots = [cons1]

        /// cons2 is now unrooted: freed.
        GCStats s2{};
        gc.collect(heap, heap.external_roots().begin(), heap.external_roots().end(), &s2);
        BOOST_TEST(s2.objects_freed == 1);
    } ///< outer destructor: external_roots = []

    /// cons1 is now unrooted: freed.
    std::vector<LispVal> empty;
    GCStats s3{};
    gc.collect(heap, empty.begin(), empty.end(), &s3);
    BOOST_TEST(s3.objects_freed == 1);
}

/**
 * Verifies ExternalRootFrame move semantics: the moved-from frame becomes
 * inactive and its destructor is a no-op; the moved-to frame retains
 * ownership and correctly unroots on destruction.
 */
BOOST_AUTO_TEST_CASE(external_root_frame_move_semantics) {
    Heap heap(1ull << 20);
    MarkSweepGC gc;

    auto cons1 = expect_ok(make_cons(heap, Nil));

    {
        auto frame1 = heap.make_external_root_frame();
        frame1.push(cons1);

        /// Move-construct frame2: frame1 becomes inactive (active_ = false).
        auto frame2 = std::move(frame1);

        /// frame1's destructor is now a no-op.  frame2 still guards cons1.
        GCStats stats1{};
        gc.collect(heap, heap.external_roots().begin(), heap.external_roots().end(), &stats1);
        BOOST_TEST(stats1.objects_freed == 0);

        /// Destructors run in reverse declaration order:
    }

    /// external_roots is empty; cons1 must now be swept.
    std::vector<LispVal> empty;
    GCStats stats2{};
    gc.collect(heap, empty.begin(), empty.end(), &stats2);
    BOOST_TEST(stats2.objects_freed == 1);
}


/**
 * Expands the mixed-type graph test to include NngSocket (always) and Tensor
 * (release builds only) as additional unreachable leaf objects alongside the
 * without child edges do not block traversal and are correctly swept.
 */
BOOST_AUTO_TEST_CASE(mixed_type_with_nng_and_tensor_leaves) {
    Heap heap(1ull << 20);
    MarkSweepGC gc;
    std::vector<LispVal> roots;

    /// --- Reachable graph: vector -> closure -> cons_with_lv -> lv(bound to leaf_cons) ---
    auto leaf_cons = expect_ok(make_cons(heap, Nil));

    auto lv = expect_ok(make_logic_var(heap));
    const auto lv_id = static_cast<ObjectId>(payload(lv));
    auto* lv_ptr = heap.try_get_as<ObjectKind::LogicVar, eta::runtime::types::LogicVar>(lv_id);
    BOOST_REQUIRE(lv_ptr != nullptr);
    lv_ptr->binding = leaf_cons;

    auto cons_with_lv = expect_ok(make_cons(heap, lv));
    auto closure      = expect_ok(make_closure(heap, nullptr, {cons_with_lv}));
    auto vec          = expect_ok(make_vector(heap, {closure}));
    roots.push_back(vec);

    /// --- Unreachable NngSocket leaf (closed=true: avoid nng_close on uninitialised fd) ---
    {
        eta::nng::NngSocketPtr sp{};
        sp.closed = true;
        (void)expect_ok(eta::nng::factory::make_nng_socket(heap, std::move(sp)));
    }

    /// --- Unreachable Tensor leaf (skipped in MSVC Debug due to ABI mismatch) ---
#ifndef ETA_TORCH_DEBUG_SKIP
    namespace tf = eta::torch_bindings::factory;
    (void)expect_ok(tf::make_tensor(heap, torch::zeros({1})));
    constexpr std::size_t GARBAGE_COUNT = 2; ///< NngSocket + Tensor
#else
    constexpr std::size_t GARBAGE_COUNT = 1; ///< NngSocket only
#endif

    GCStats stats{};
    gc.collect(heap, roots.begin(), roots.end(), &stats);
    /**
     * Reachable (retained): vec, closure, cons_with_lv, lv, leaf_cons
     * Garbage  (freed):     NngSocket + Tensor (if available)
     */
    BOOST_TEST(stats.objects_freed == GARBAGE_COUNT);
}


/**
 * Verifies that:
 *   (a) the GC callback fires when a succession of ByteVector allocations
 *       with large internal data payloads pushes past the soft limit;
 *   (b) the GC correctly invokes the type-erased ByteVector destructor, freeing
 *       the underlying std::vector<uint8_t> storage;
 *   (c) the single rooted ByteVector with a sentinel payload survives intact.
 */
BOOST_AUTO_TEST_CASE(large_bytevector_soft_limit_stress) {
    /**
     * ByteVector's tracked heap footprint is sizeof(ByteVector) regardless of
     * how many bytes its internal std::vector<uint8_t> actually holds.  We use
     * a 1 KiB internal payload to exercise the destructor's cleanup path.
     */
    constexpr std::size_t INTERNAL_SZ = 1024;
    const std::size_t bv_tracked = sizeof(eta::runtime::types::ByteVector);

    /// Soft limit: fits exactly 5 ByteVectors.  The 6th allocation triggers GC.
    Heap heap(bv_tracked * 5);
    MarkSweepGC gc;
    std::vector<LispVal> roots;

    bool callback_fired = false;
    heap.set_gc_callback([&]() {
        callback_fired = true;
        gc.collect(heap, roots.begin(), roots.end());
    });

    /// Rooted ByteVector with a recognisable sentinel payload.
    const std::vector<uint8_t> sentinel(INTERNAL_SZ, 0xCA);
    auto rooted_bv = expect_ok(make_bytevector(heap, sentinel));
    roots.push_back(rooted_bv);

    for (int i = 0; i < 4; ++i) {
        (void)expect_ok(make_bytevector(heap,
            std::vector<uint8_t>(INTERNAL_SZ, static_cast<uint8_t>(i))));
    }
    BOOST_TEST(!callback_fired);

    /// the 4 unreachable ByteVectors (and their 1 KiB buffers), then proceeds.
    (void)expect_ok(make_bytevector(heap, std::vector<uint8_t>(INTERNAL_SZ, 0xFF)));
    BOOST_TEST(callback_fired);

    /// Rooted ByteVector must still be accessible with its sentinel data intact.
    const auto bv_id = static_cast<ObjectId>(payload(rooted_bv));
    auto* bv_ptr = heap.try_get_as<ObjectKind::ByteVector,
                                    eta::runtime::types::ByteVector>(bv_id);
    BOOST_REQUIRE(bv_ptr != nullptr);
    BOOST_TEST(bv_ptr->data == sentinel);
}

BOOST_AUTO_TEST_SUITE_END()
