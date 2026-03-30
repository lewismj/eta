#include <boost/test/unit_test.hpp>

#include <cstdint>
#include <expected>
#include <vector>

#include <eta/runtime/memory/heap.h>

using namespace eta::runtime::memory::heap;
using constants::PAYLOAD_MASK;

namespace {
    struct SmallPod { int x; }; // trivial type

    struct BigPod { char data[4096]; }; // for soft-limit testing

    // Type with destructor that throws to exercise deallocation error path
    struct ThrowOnDtor {
        int x;
        ~ThrowOnDtor() noexcept(false) { throw std::runtime_error("boom"); }
    };

    // Helper to unwrap expected OK
    template <typename T, typename E>
    T expect_ok(const std::expected<T,E>& r) {
        BOOST_REQUIRE(r.has_value());
        return *r;
    }
}

BOOST_AUTO_TEST_SUITE(heap_tests)

BOOST_AUTO_TEST_CASE(allocate_and_deallocate_basic) {
    Heap heap(/*max_heap_soft_limit*/ 1ull << 20); // 1 MiB

    // Allocate a few small objects
    std::vector<ObjectId> ids;
    for (int i = 0; i < 10; ++i) {
        auto id = expect_ok(heap.allocate<SmallPod, ObjectKind::Fixnum>(SmallPod{i}));
        BOOST_TEST(id > 0);
        BOOST_TEST(id <= PAYLOAD_MASK);
        ids.push_back(id);
    }

    // Deallocate them
    for (auto id : ids) {
        auto r = heap.deallocate(id);
        BOOST_REQUIRE(r.has_value());
    }
}

BOOST_AUTO_TEST_CASE(double_deallocate_returns_not_found) {
    Heap heap(1ull << 20);

    const auto id = expect_ok(heap.allocate<SmallPod, ObjectKind::Fixnum>(SmallPod{123}));

    // First deallocation: success
    BOOST_REQUIRE(heap.deallocate(id).has_value());
    // Second deallocation: should report not found
    auto r2 = heap.deallocate(id);
    BOOST_REQUIRE(!r2.has_value());
    BOOST_TEST(r2.error() == HeapError::ObjectIdNotFound);
}

BOOST_AUTO_TEST_CASE(deallocate_unknown_id_returns_not_found) {
    Heap heap(1ull << 20);

    // Choose an arbitrary ID that was never allocated
    constexpr ObjectId missing = 123456789ull;
    auto r = heap.deallocate(missing);
    BOOST_REQUIRE(!r.has_value());
    BOOST_TEST(r.error() == HeapError::ObjectIdNotFound);
}

BOOST_AUTO_TEST_CASE(soft_limit_enforced_strictly_greater_than) {
    // If current_total + sizeof(T) > max_heap_soft_limit_ -> error
    // Equal to limit is allowed by the code.
    constexpr std::size_t limit = sizeof(BigPod);
    Heap heap(limit);

    // Exactly fits -> allowed
    auto id1 = heap.allocate<BigPod, ObjectKind::Vector>(BigPod{});
    BOOST_REQUIRE(id1.has_value());

    // Next allocation would exceed limit -> error
    auto id2 = heap.allocate<BigPod, ObjectKind::Vector>(BigPod{});
    BOOST_REQUIRE(!id2.has_value());
    BOOST_TEST(id2.error() == HeapError::SoftHeapLimitExceeded);
}

BOOST_AUTO_TEST_CASE(destructor_throw_is_reported_and_erases_entry) {
    Heap heap(1ull << 20);

    // Construct ThrowOnDtor directly in heap storage, avoiding a throwing
    // destructor on a temporary at end of full expression.
    const auto id = expect_ok(heap.allocate<ThrowOnDtor, ObjectKind::Cons>(7));

    // Deallocate should catch and map to FailedToDeallocateMemory, also erase the entry.
    auto r = heap.deallocate(id);
    BOOST_REQUIRE(!r.has_value());
    BOOST_TEST(r.error() == HeapError::FailedToDeallocateMemory);

    // Entry should already be erased; another call must return NotFound
    auto r2 = heap.deallocate(id);
    BOOST_REQUIRE(!r2.has_value());
    BOOST_TEST(r2.error() == HeapError::ObjectIdNotFound);
}

BOOST_AUTO_TEST_CASE(heap_destruction_calls_destructors) {
    // We cannot directly count calls without intrusive hooks, but we can ensure
    // that creating objects and letting the heap go out of scope does not crash.
    {
        Heap heap(1ull << 20);
        (void) heap.allocate<SmallPod, ObjectKind::Fixnum>(SmallPod{1});
        (void) heap.allocate<BigPod,   ObjectKind::Vector>(BigPod{});
        (void) heap.allocate<SmallPod, ObjectKind::Cons>(SmallPod{2});
    }
    BOOST_TEST(true); // reached here without issues
}




BOOST_AUTO_TEST_SUITE_END()
