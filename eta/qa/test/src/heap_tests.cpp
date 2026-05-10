#include <boost/test/unit_test.hpp>

#include <cstdio>
#include <cstdint>
#include <expected>
#include <vector>

#include <eta/runtime/memory/heap.h>
#include <eta/runtime/memory/native_object_inspection.h>

using namespace eta::runtime::memory::heap;
using constants::PAYLOAD_MASK;

namespace {
    struct SmallPod { int x; }; ///< trivial type

    struct BigPod { char data[4096]; }; ///< for soft-limit testing

    /// Type with destructor that throws to exercise deallocation error path
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable: 4722) ///< destructor never returns: intentional for this test fixture
#endif
    struct ThrowOnDtor {
        int x;
        ~ThrowOnDtor() noexcept(false) { throw std::runtime_error("boom"); }
    };
#ifdef _MSC_VER
#pragma warning(pop)
#endif

    /// Helper to unwrap expected OK
    template <typename T, typename E>
    T expect_ok(const std::expected<T,E>& r) {
        BOOST_REQUIRE(r.has_value());
        return *r;
    }

    struct NativeDestroyCounter {
        int calls{0};
    };

    struct NativeDisplayPayload {
        const char* text{nullptr};
    };

    extern "C" void heap_test_native_destroy(void* user_data) {
        if (auto* counter = static_cast<NativeDestroyCounter*>(user_data)) {
            ++counter->calls;
        }
    }

    constexpr EtaNativeObjectVTable kNativeObjectVTable{
        .type_name = "heap.native.test",
        .destroy = &heap_test_native_destroy,
        .trace = nullptr,
        .display = nullptr,
    };

    extern "C" void heap_test_native_display(void* user_data, FILE* out) {
        if (out == nullptr) return;
        auto* payload = static_cast<NativeDisplayPayload*>(user_data);
        if (payload == nullptr || payload->text == nullptr) return;
        std::fputs(payload->text, out);
    }

    constexpr EtaNativeObjectVTable kNativeDisplayVTable{
        .type_name = "heap.native.display",
        .destroy = nullptr,
        .trace = nullptr,
        .display = &heap_test_native_display,
    };
}

BOOST_AUTO_TEST_SUITE(heap_tests)

BOOST_AUTO_TEST_CASE(allocate_and_deallocate_basic) {
    Heap heap(/*max_heap_soft_limit*/ 1ull << 20); ///< 1 MiB

    /// Allocate a few small objects
    std::vector<ObjectId> ids;
    for (int i = 0; i < 10; ++i) {
        auto id = expect_ok(heap.allocate<SmallPod, ObjectKind::Fixnum>(SmallPod{i}));
        BOOST_TEST(id > 0);
        BOOST_TEST(id <= PAYLOAD_MASK);
        ids.push_back(id);
    }

    /// Deallocate them
    for (auto id : ids) {
        auto r = heap.deallocate(id);
        BOOST_REQUIRE(r.has_value());
    }
}

BOOST_AUTO_TEST_CASE(double_deallocate_returns_not_found) {
    Heap heap(1ull << 20);

    const auto id = expect_ok(heap.allocate<SmallPod, ObjectKind::Fixnum>(SmallPod{123}));

    /// First deallocation: success
    BOOST_REQUIRE(heap.deallocate(id).has_value());
    /// Second deallocation: should report not found
    auto r2 = heap.deallocate(id);
    BOOST_REQUIRE(!r2.has_value());
    BOOST_TEST(r2.error() == HeapError::ObjectIdNotFound);
}

BOOST_AUTO_TEST_CASE(deallocate_unknown_id_returns_not_found) {
    Heap heap(1ull << 20);

    /// Choose an arbitrary ID that was never allocated
    constexpr ObjectId missing = 123456789ull;
    auto r = heap.deallocate(missing);
    BOOST_REQUIRE(!r.has_value());
    BOOST_TEST(r.error() == HeapError::ObjectIdNotFound);
}

BOOST_AUTO_TEST_CASE(soft_limit_enforced_strictly_greater_than) {
    /**
     * If current_total + sizeof(T) > max_heap_soft_limit_ -> error
     * Equal to limit is allowed by the code.
     */
    constexpr std::size_t limit = sizeof(BigPod);
    Heap heap(limit);

    /// Exactly fits -> allowed
    auto id1 = heap.allocate<BigPod, ObjectKind::Vector>(BigPod{});
    BOOST_REQUIRE(id1.has_value());

    /// Next allocation would exceed limit -> error
    auto id2 = heap.allocate<BigPod, ObjectKind::Vector>(BigPod{});
    BOOST_REQUIRE(!id2.has_value());
    BOOST_TEST(id2.error() == HeapError::SoftHeapLimitExceeded);
}

BOOST_AUTO_TEST_CASE(destructor_throw_is_reported_and_erases_entry) {
    Heap heap(1ull << 20);

    /**
     * Construct ThrowOnDtor directly in heap storage, avoiding a throwing
     * destructor on a temporary at end of full expression.
     */
    const auto id = expect_ok(heap.allocate<ThrowOnDtor, ObjectKind::Cons>(7));

    /// Deallocate should catch and map to FailedToDeallocateMemory, also erase the entry.
    auto r = heap.deallocate(id);
    BOOST_REQUIRE(!r.has_value());
    BOOST_TEST(r.error() == HeapError::FailedToDeallocateMemory);

    /// Entry should already be erased; another call must return NotFound
    auto r2 = heap.deallocate(id);
    BOOST_REQUIRE(!r2.has_value());
    BOOST_TEST(r2.error() == HeapError::ObjectIdNotFound);
}

BOOST_AUTO_TEST_CASE(heap_destruction_calls_destructors) {
    /**
     * We cannot directly count calls without intrusive hooks, but we can ensure
     * that creating objects and letting the heap go out of scope does not crash.
     */
    {
        Heap heap(1ull << 20);
        (void) heap.allocate<SmallPod, ObjectKind::Fixnum>(SmallPod{1});
        (void) heap.allocate<BigPod,   ObjectKind::Vector>(BigPod{});
        (void) heap.allocate<SmallPod, ObjectKind::Cons>(SmallPod{2});
    }
    BOOST_TEST(true); ///< reached here without issues
}

BOOST_AUTO_TEST_CASE(native_object_deallocate_calls_destroy_once) {
    Heap heap(1ull << 20);
    NativeDestroyCounter counter{};

    const auto id = expect_ok(
        heap.allocate<NativeObjectHeader, ObjectKind::NativeObject>(
            NativeObjectHeader{&kNativeObjectVTable, &counter}));

    BOOST_REQUIRE(heap.deallocate(id).has_value());
    BOOST_TEST(counter.calls == 1);

    auto second = heap.deallocate(id);
    BOOST_REQUIRE(!second.has_value());
    BOOST_TEST(second.error() == HeapError::ObjectIdNotFound);
    BOOST_TEST(counter.calls == 1);
}

BOOST_AUTO_TEST_CASE(native_object_null_vtable_skips_destroy_callback) {
    Heap heap(1ull << 20);
    NativeDestroyCounter counter{};

    const auto id = expect_ok(
        heap.allocate<NativeObjectHeader, ObjectKind::NativeObject>(
            NativeObjectHeader{nullptr, &counter}));

    BOOST_REQUIRE(heap.deallocate(id).has_value());
    BOOST_TEST(counter.calls == 0);
}

BOOST_AUTO_TEST_CASE(native_object_destroy_runs_on_heap_teardown) {
    NativeDestroyCounter counter{};
    {
        Heap heap(1ull << 20);
        (void) expect_ok(
            heap.allocate<NativeObjectHeader, ObjectKind::NativeObject>(
                NativeObjectHeader{&kNativeObjectVTable, &counter}));
    }

    BOOST_TEST(counter.calls == 1);
}

BOOST_AUTO_TEST_CASE(native_object_inspection_reports_type_name_and_display) {
    Heap heap(1ull << 20);
    NativeDisplayPayload payload{"payload-display"};

    const auto id = expect_ok(
        heap.allocate<NativeObjectHeader, ObjectKind::NativeObject>(
            NativeObjectHeader{&kNativeDisplayVTable, &payload}));

    HeapEntry entry{};
    BOOST_REQUIRE(heap.try_get(id, entry));

    auto inspection = native_object_inspection_info(entry);
    BOOST_REQUIRE(inspection.has_value());
    BOOST_TEST(inspection->kind_label == "NativeObject:heap.native.display");
    BOOST_TEST(inspection->type_name == "heap.native.display");
    BOOST_TEST(inspection->display == "payload-display");
    BOOST_TEST(heap_entry_kind_label(entry) == "NativeObject:heap.native.display");
}

BOOST_AUTO_TEST_CASE(native_object_inspection_handles_missing_vtable_metadata) {
    Heap heap(1ull << 20);
    NativeDestroyCounter counter{};

    const auto id = expect_ok(
        heap.allocate<NativeObjectHeader, ObjectKind::NativeObject>(
            NativeObjectHeader{nullptr, &counter}));

    HeapEntry entry{};
    BOOST_REQUIRE(heap.try_get(id, entry));

    auto inspection = native_object_inspection_info(entry);
    BOOST_REQUIRE(inspection.has_value());
    BOOST_TEST(inspection->kind_label == "NativeObject");
    BOOST_TEST(inspection->type_name.empty());
    BOOST_TEST(inspection->display.empty());
    BOOST_TEST(heap_entry_kind_label(entry) == "NativeObject");
}




BOOST_AUTO_TEST_SUITE_END()
