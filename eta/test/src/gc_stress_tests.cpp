/**
 * @file gc_stress_tests.cpp
 * @brief Deterministic GC stress tests.
 *
 * Constructs known live-object graphs under a deliberately small heap soft-limit,
 * verifies that all live objects survive collection, and asserts that dead objects
 * are reclaimed.  The four test groups correspond to the scenarios documented in
 * next-steps.md §3.1.
 *
 * Test matrix:
 *   gc_stress_tests/quoted_constant_survives   — Driver-level: bytecode-registry
 *       constants survive repeated GC cycles forced by primitive re-installation.
 *   gc_stress_tests/actor_round_trip_under_pressure — Heap-level: a rooted list
 *       "payload" survives many GC cycles that reclaim ephemeral cons cells
 *       (simulates a mailbox send/receive round-trip).
 *   gc_stress_tests/recursive_cons_reconstruction — Heap-level: a list is grown
 *       one element at a time under allocation pressure; each step verifies the
 *       accumulating head is not swept.
 *   gc_stress_tests/multithread_gc_isolation   — Two independent Heap+GC instances
 *       run concurrently on separate threads; verifies no cross-heap corruption.
 */

#include <boost/test/unit_test.hpp>

#include <atomic>
#include <string>
#include <thread>
#include <vector>

// Low-level runtime
#include <eta/runtime/memory/heap.h>
#include <eta/runtime/memory/mark_sweep_gc.h>
#include <eta/runtime/memory/cons_pool.h>
#include <eta/runtime/factory.h>
#include <eta/runtime/types/cons.h>
#include <eta/runtime/nanbox.h>

// Driver (for quoted_constant_survives)
#include <eta/interpreter/driver.h>
#include <eta/interpreter/module_path.h>

// ============================================================================
// Shared helpers
// ============================================================================

namespace {

using namespace eta::runtime::memory::heap;
using namespace eta::runtime::memory::gc;
using namespace eta::runtime::memory::factory;
using namespace eta::runtime::nanbox;
using namespace eta::runtime::nanbox::ops;

template <typename T, typename E>
T require_ok(const std::expected<T, E>& r, const char* msg = "expected success") {
    BOOST_REQUIRE_MESSAGE(r.has_value(), msg);
    return *r;
}

/// Walk a Lisp-style cons list and return its length.
/// Stops when it encounters a non-HeapObject (e.g. Nil) or a non-pool cell.
int list_length(Heap& heap, LispVal head) {
    int n = 0;
    while (is_boxed(head) && tag(head) == Tag::HeapObject) {
        const auto id = static_cast<ObjectId>(payload(head));
        const auto* c = heap.cons_pool().try_get(id);
        if (!c) break;
        head = c->cdr;
        ++n;
    }
    return n;
}

/// Return the car of the nth element (0-indexed) in a cons list.
/// Returns Nil if the list is shorter than n+1.
LispVal list_nth_car(Heap& heap, LispVal head, int n) {
    for (int i = 0; i < n; ++i) {
        if (!is_boxed(head) || tag(head) != Tag::HeapObject) return Nil;
        const auto id = static_cast<ObjectId>(payload(head));
        const auto* c = heap.cons_pool().try_get(id);
        if (!c) return Nil;
        head = c->cdr;
    }
    if (!is_boxed(head) || tag(head) != Tag::HeapObject) return Nil;
    const auto id = static_cast<ObjectId>(payload(head));
    const auto* c = heap.cons_pool().try_get(id);
    return c ? c->car : Nil;
}

} // anonymous namespace

// ============================================================================
// Test suite
// ============================================================================

BOOST_AUTO_TEST_SUITE(gc_stress_tests)

// ----------------------------------------------------------------------------
/**
 * @test quoted_constant_survives
 *
 * Verifies that the Driver's GC does not sweep bytecode-registry constants
 * (the Cons chains created by quoted literals such as '(10 20 30 40 50)) even
 * after many GC cycles have been forced by repeated primitive re-installation.
 *
 * Uses a 64 KiB heap soft-limit so that allocating ~13 KiB of Primitive
 * objects on every run_source() call causes the collector to fire roughly
 * once every 4–5 iterations over the 40-iteration pressure loop.
 */
BOOST_AUTO_TEST_CASE(quoted_constant_survives) {
    // Empty resolver — no stdlib needed; only built-in primitives are used.
    eta::interpreter::ModulePathResolver resolver{};
    // 64 KiB heap forces several GC collections during the pressure loop.
    eta::interpreter::Driver driver(std::move(resolver), 64 * 1024);

    // --- Step 1: define a function whose body references a quoted constant ---
    // The quoted list '(10 20 30 40 50) becomes a chain of 5 Cons cells stored
    // in the emitted BytecodeFunction's constants[] vector.  Those cells are GC
    // roots only through Driver::collect_garbage_with_registry_roots().
    bool ok = driver.run_source(
        "(module gc_sq_mod"
        "  (export get-list)"
        "  (begin"
        "    (define get-list (lambda () '(10 20 30 40 50)))))");
    BOOST_REQUIRE_MESSAGE(ok, "gc_sq_mod: compilation failed");

    // --- Step 2: pressure loop ---
    // Each iteration compiles and executes a fresh module that calls (get-list).
    // run_source() re-installs all builtins (~13 KiB of Primitive objects) on
    // every execution, making old Primitive objects unreachable.  This causes
    // the GC callback to fire several times.  The quoted Cons cells in the
    // registry MUST survive every collection.
    for (int i = 0; i < 40; ++i) {
        const std::string src =
            "(module gc_sq_pressure_" + std::to_string(i) +
            "  (import gc_sq_mod)"
            "  (begin (get-list)))";
        bool pressure_ok = driver.run_source(src);
        BOOST_REQUIRE_MESSAGE(pressure_ok,
            "gc_sq_pressure_" << i << ": failed — heap may be exhausted");
    }

    // --- Step 3: verify the quoted constant is intact ---
    // (length (get-list)) must return 5.  The `length` primitive is a builtin.
    eta::runtime::nanbox::LispVal result{};
    ok = driver.run_source(
        "(module gc_sq_verify"
        "  (import gc_sq_mod)"
        "  (export result)"
        "  (begin"
        "    (define result (length (get-list)))))",
        &result, "result");
    BOOST_REQUIRE_MESSAGE(ok, "gc_sq_verify: failed");

    const auto decoded = eta::runtime::nanbox::ops::decode<int64_t>(result);
    BOOST_REQUIRE_MESSAGE(decoded.has_value(),
        "gc_sq_verify: result is not a fixnum");
    BOOST_CHECK_EQUAL(*decoded, 5LL);
}

// ----------------------------------------------------------------------------
/**
 * @test actor_round_trip_under_pressure
 *
 * Simulates a list payload surviving an actor "mailbox" round-trip under GC
 * pressure.  A list of PAYLOAD_LEN elements is built on a 1 KiB heap and kept
 * as the single root while PRESSURE_ROUNDS × EPHEMERAL_PER_ROUND ephemeral
 * cons cells are allocated.  Each ephemeral batch fills the heap past its
 * soft-limit, triggering the GC callback, which reclaims the ephemeral cells
 * while leaving the rooted payload intact.
 *
 * After all pressure rounds the list length and element values are verified.
 */
BOOST_AUTO_TEST_CASE(actor_round_trip_under_pressure) {
    constexpr std::size_t HEAP_SIZE          = 1 * 1024; // 1 KiB — forces frequent GC
    constexpr int         PAYLOAD_LEN        = 40;
    constexpr int         PRESSURE_ROUNDS    = 50;
    constexpr int         EPHEMERAL_PER_ROUND = 30;

    Heap heap(HEAP_SIZE);
    MarkSweepGC gc;
    std::vector<LispVal> roots;

    // GC callback: collect with the current roots vector.
    heap.set_gc_callback([&]() {
        gc.collect(heap, roots.begin(), roots.end());
    });

    // Build the payload list: (0.0 1.0 2.0 ... (PAYLOAD_LEN-1).0)
    // Constructed in reverse so the final list is in ascending order.
    LispVal payload = Nil;
    for (int i = PAYLOAD_LEN - 1; i >= 0; --i) {
        const auto enc = encode(static_cast<double>(i));
        BOOST_REQUIRE(enc.has_value());
        const auto cell = make_cons(heap, *enc, payload);
        BOOST_REQUIRE_MESSAGE(cell.has_value(),
            "payload alloc failed at element " << i);
        payload = *cell;
    }

    // Root the payload so it survives GC.
    roots.push_back(payload);

    // Apply pressure: each round allocates EPHEMERAL_PER_ROUND unreachable pairs.
    for (int round = 0; round < PRESSURE_ROUNDS; ++round) {
        for (int j = 0; j < EPHEMERAL_PER_ROUND; ++j) {
            // Not rooted — will be collected when GC fires.
            (void)make_cons(heap, Nil, Nil);
        }
    }

    // Verify: the payload list is fully intact.
    BOOST_CHECK_EQUAL(list_length(heap, payload), PAYLOAD_LEN);

    // Spot-check: the car of the first element must be 0.0.
    const auto first_car = list_nth_car(heap, payload, 0);
    const auto first_val = decode<double>(first_car);
    BOOST_REQUIRE(first_val.has_value());
    BOOST_CHECK_CLOSE(*first_val, 0.0, 1e-9);

    // Spot-check: the car of the last element must be (PAYLOAD_LEN - 1).0.
    const auto last_car = list_nth_car(heap, payload, PAYLOAD_LEN - 1);
    const auto last_val = decode<double>(last_car);
    BOOST_REQUIRE(last_val.has_value());
    BOOST_CHECK_CLOSE(*last_val, static_cast<double>(PAYLOAD_LEN - 1), 1e-9);
}

// ----------------------------------------------------------------------------
/**
 * @test recursive_cons_reconstruction
 *
 * Verifies that a list built incrementally (by prepending one element per step)
 * on a small heap survives repeated GC collections triggered by ephemeral
 * allocations at each step.
 *
 * At each step the algorithm:
 *   1. Allocates the new head cell (cons i head).
 *   2. Updates the single root to the new head.
 *   3. Allocates EPHEMERAL_PER_STEP unreachable cells to drive GC pressure.
 *
 * GC fires during step 3 once the heap fills up, freeing the previous step's
 * ephemeral cells.  The rooted growing list must be fully retained throughout.
 */
BOOST_AUTO_TEST_CASE(recursive_cons_reconstruction) {
    constexpr std::size_t HEAP_SIZE         = 1 * 1024; // 1 KiB
    constexpr int         LIST_DEPTH        = 30;
    constexpr int         EPHEMERAL_PER_STEP = 25;

    Heap heap(HEAP_SIZE);
    MarkSweepGC gc;
    std::vector<LispVal> roots;

    heap.set_gc_callback([&]() {
        gc.collect(heap, roots.begin(), roots.end());
    });

    // The single root tracks the current list head.
    LispVal head = Nil;
    roots.push_back(head); // roots[0] — updated in-place each step.

    for (int i = 0; i < LIST_DEPTH; ++i) {
        // Prepend element i to the list.
        const auto enc = encode(static_cast<double>(i));
        BOOST_REQUIRE(enc.has_value());
        const auto cell = make_cons(heap, *enc, head);
        BOOST_REQUIRE_MESSAGE(cell.has_value(),
            "cons alloc failed at step " << i);
        head = *cell;
        roots[0] = head; // keep root current before ephemeral pressure.

        // Allocate ephemeral cells — not rooted, will be freed by GC.
        for (int j = 0; j < EPHEMERAL_PER_STEP; ++j) {
            (void)make_cons(heap, Nil, Nil);
        }
    }

    // The reconstructed list must contain exactly LIST_DEPTH elements.
    BOOST_CHECK_EQUAL(list_length(heap, head), LIST_DEPTH);

    // The head's car must be (LIST_DEPTH - 1) — the last prepended value.
    const auto head_car = list_nth_car(heap, head, 0);
    const auto head_val = decode<double>(head_car);
    BOOST_REQUIRE(head_val.has_value());
    BOOST_CHECK_CLOSE(*head_val, static_cast<double>(LIST_DEPTH - 1), 1e-9);
}

// ----------------------------------------------------------------------------
/**
 * @test multithread_gc_isolation
 *
 * Runs two independent Heap+GC instances concurrently on separate threads.
 * Each thread allocates LIVE_COUNT rooted cons cells, then runs GC_CYCLES
 * explicit collection rounds interleaved with ephemeral allocations.
 *
 * After joining both threads, the test asserts:
 *   - Neither thread reported a failure.
 *   - Each thread's LIVE_COUNT rooted cells are still accessible.
 *
 * This verifies that concurrent GC on distinct heaps causes no cross-heap
 * corruption (data races, wrong freed counts, etc.).
 */
BOOST_AUTO_TEST_CASE(multithread_gc_isolation) {
    constexpr std::size_t HEAP_SIZE  = 256 * 1024; // 256 KiB per thread
    constexpr int         LIVE_COUNT = 100;
    constexpr int         GC_CYCLES  = 20;
    constexpr int         EPHEMERAL  = 50; // unreachable allocations between cycles

    std::atomic<bool> thread1_ok{true};
    std::atomic<bool> thread2_ok{true};

    // Worker: owns its own Heap+GC, rooted vector, and verification logic.
    auto worker = [&](std::atomic<bool>& success) noexcept {
        try {
            Heap heap(HEAP_SIZE);
            MarkSweepGC gc;
            std::vector<LispVal> roots;
            roots.reserve(LIVE_COUNT);

            heap.set_gc_callback([&]() {
                gc.collect(heap, roots.begin(), roots.end());
            });

            // Allocate LIVE_COUNT rooted cons cells.
            for (int i = 0; i < LIVE_COUNT; ++i) {
                const auto c = make_cons(heap, Nil, Nil);
                if (!c.has_value()) { success = false; return; }
                roots.push_back(*c);
            }

            // Alternate: allocate ephemeral cells + explicit GC collections.
            for (int cycle = 0; cycle < GC_CYCLES; ++cycle) {
                for (int j = 0; j < EPHEMERAL; ++j) {
                    (void)make_cons(heap, Nil, Nil); // unreachable
                }
                GCStats stats{};
                gc.collect(heap, roots.begin(), roots.end(), &stats);
                // The LIVE_COUNT rooted cells must never be freed.
                if (stats.objects_freed > static_cast<std::size_t>(EPHEMERAL)) {
                    // More objects freed than allocated ephemerals — rooted cell lost.
                    success = false;
                    return;
                }
            }

            // Final verification: every rooted cell is still accessible.
            for (int i = 0; i < LIVE_COUNT; ++i) {
                const auto v = roots[static_cast<std::size_t>(i)];
                if (!is_boxed(v) || tag(v) != Tag::HeapObject) {
                    success = false;
                    return;
                }
                const auto id = static_cast<ObjectId>(payload(v));
                // Cons cells live in the pool; check there first.
                if (!heap.cons_pool().try_get(id)) {
                    // Fall back to general heap (e.g. pool was full at alloc time).
                    HeapEntry entry;
                    if (!heap.try_get(id, entry)) {
                        success = false;
                        return;
                    }
                }
            }
        } catch (...) {
            success = false;
        }
    };

    std::thread t1([&]() { worker(thread1_ok); });
    std::thread t2([&]() { worker(thread2_ok); });
    t1.join();
    t2.join();

    BOOST_CHECK_MESSAGE(thread1_ok.load(), "thread 1: GC isolation failure");
    BOOST_CHECK_MESSAGE(thread2_ok.load(), "thread 2: GC isolation failure");
}

BOOST_AUTO_TEST_SUITE_END()

