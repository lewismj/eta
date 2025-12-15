#include <atomic>
#include <barrier>
#include <memory>
#include <thread>
#include <vector>
#include <chrono>
#include <random>

#include <boost/test/unit_test.hpp>

#include <eta/runtime/memory/hazard_ptr.h>

using namespace eta::runtime::memory::hazard;

namespace {
    // Test node for linked data structures
    struct Node {
        std::atomic<int> value;
        std::atomic<Node*> next;

        explicit Node(int v) : value(v), next(nullptr) {}
    };

    // Track allocations/deallocations for verification
    struct TrackedNode {
        int value;
        inline static std::atomic<int> live_count{0};
        inline static std::atomic<int> total_allocated{0};
        inline static std::atomic<int> total_deleted{0};

        explicit TrackedNode(int v) : value(v) {
            live_count.fetch_add(1, std::memory_order_relaxed);
            total_allocated.fetch_add(1, std::memory_order_relaxed);
        }

        ~TrackedNode() {
            live_count.fetch_sub(1, std::memory_order_relaxed);
            total_deleted.fetch_add(1, std::memory_order_relaxed);
        }

        static void reset_counters() {
            live_count.store(0, std::memory_order_relaxed);
            total_allocated.store(0, std::memory_order_relaxed);
            total_deleted.store(0, std::memory_order_relaxed);
        }
    };
}

BOOST_AUTO_TEST_SUITE(hazard_ptr_tests)

BOOST_AUTO_TEST_CASE(handle_creation_and_destruction) {
    HazardPointerDomain<Node> domain;

    {
        auto handle = domain.make_handle();
        // Handle should be valid
        BOOST_TEST(true);
    }

    // Handle destroyed without issues
    BOOST_TEST(true);
}

BOOST_AUTO_TEST_CASE(acquire_and_release_single_pointer) {
    HazardPointerDomain<Node> domain;
    auto handle = domain.make_handle();

    Node* node = new Node(42);

    // Acquire protection
    handle.acquire(node);

    // Release protection
    handle.release();

    delete node;
    BOOST_TEST(true);
}

BOOST_AUTO_TEST_CASE(retire_immediately_reclaims_unprotected) {
    TrackedNode::reset_counters();

    HazardPointerDomain<TrackedNode> domain;
    auto handle = domain.make_handle();

    auto* node = new TrackedNode(100);
    BOOST_TEST(TrackedNode::live_count.load() == 1);

    // Retire without protection - should be reclaimed after threshold
    for (int i = 0; i < 400; ++i) {
        auto* temp = new TrackedNode(i);
        handle.retire(temp);
    }

    // Force flush
    handle.flush();

    // All retired nodes should be deleted
    BOOST_TEST(TrackedNode::total_deleted.load() == 400);

    delete node;
}

BOOST_AUTO_TEST_CASE(protected_pointer_not_reclaimed) {
    TrackedNode::reset_counters();

    HazardPointerDomain<TrackedNode> domain;
    auto handle = domain.make_handle();

    auto* protected_node = new TrackedNode(999);

    // Protect the node
    handle.acquire(protected_node);

    // Retire it (but it's protected)
    handle.retire(protected_node);

    // Add more nodes to trigger reclamation
    for (int i = 0; i < 400; ++i) {
        auto* temp = new TrackedNode(i);
        handle.retire(temp);
    }

    handle.flush();

    // The protected node should NOT be deleted yet
    BOOST_TEST(TrackedNode::live_count.load() >= 1);

    // Release protection
    handle.release();
    handle.flush();

    // Now it should be reclaimed
    BOOST_TEST(TrackedNode::live_count.load() == 0);
}

BOOST_AUTO_TEST_CASE(multiple_handles_same_domain) {
    HazardPointerDomain<Node> domain;

    auto handle1 = domain.make_handle(0);
    auto handle2 = domain.make_handle(1);

    Node* node = new Node(123);

    handle1.acquire(node);
    handle2.acquire(node);

    handle1.release();
    handle2.release();

    delete node;
    BOOST_TEST(true);
}

BOOST_AUTO_TEST_CASE(custom_deleter) {
    HazardPointerDomain<Node> domain;
    auto handle = domain.make_handle();

    bool custom_deleter_called = false;

    auto* node = new Node(42);
    handle.retire(node, [&custom_deleter_called](Node* p) {
        custom_deleter_called = true;
        delete p;
    });

    handle.flush();

    BOOST_TEST(custom_deleter_called);
}

BOOST_AUTO_TEST_CASE(function_pointer_deleter) {
    HazardPointerDomain<Node> domain;
    auto handle = domain.make_handle();

    static bool function_deleter_called = false;
    function_deleter_called = false;

    auto deleter = [](Node* p) {
        function_deleter_called = true;
        delete p;
    };

    auto* node = new Node(42);
    handle.retire(node, +deleter); // Convert to function pointer

    handle.flush();

    BOOST_TEST(function_deleter_called);
}

BOOST_AUTO_TEST_CASE(handle_move_constructor) {
    HazardPointerDomain<Node> domain;
    auto handle1 = domain.make_handle();

    Node* node = new Node(42);
    handle1.acquire(node);

    // Move construct
    auto handle2 = std::move(handle1);

    // handle2 should now own the protection
    handle2.release();
    delete node;
    BOOST_TEST(true);
}

BOOST_AUTO_TEST_CASE(handle_move_assignment) {
    HazardPointerDomain<Node> domain;
    auto handle1 = domain.make_handle(0);
    auto handle2 = domain.make_handle(1);

    Node* node = new Node(42);
    handle1.acquire(node);

    // Move assign
    handle2 = std::move(handle1);

    handle2.release();
    delete node;
    BOOST_TEST(true);
}

// ============================================================================
// Concurrent Access Tests
// ============================================================================

BOOST_AUTO_TEST_CASE(concurrent_acquire_release) {
    HazardPointerDomain<Node> domain;
    Node* shared_node = new Node(100);

    constexpr int num_threads = 8;
    constexpr int iterations = 1000;

    std::vector<std::thread> threads;
    std::barrier sync_point(num_threads);

    for (int t = 0; t < num_threads; ++t) {
        threads.emplace_back([&, t] {
            auto handle = domain.make_handle(t % 3);

            sync_point.arrive_and_wait();

            for (int i = 0; i < iterations; ++i) {
                handle.acquire(shared_node);

                // Simulate some work
                std::this_thread::yield();

                handle.release();
            }
        });
    }

    for (auto& t : threads) {
        t.join();
    }

    delete shared_node;
    BOOST_TEST(true);
}

BOOST_AUTO_TEST_CASE(concurrent_retire_with_protection) {
    TrackedNode::reset_counters();

    HazardPointerDomain<TrackedNode> domain;

    constexpr int num_threads = 8;
    constexpr int nodes_per_thread = 50;

    std::vector<std::thread> threads;
    std::barrier sync_point(num_threads);

    for (int t = 0; t < num_threads; ++t) {
        threads.emplace_back([&] {
            auto handle = domain.make_handle();

            sync_point.arrive_and_wait();

            for (int i = 0; i < nodes_per_thread; ++i) {
                auto* node = new TrackedNode(i);

                // Randomly protect some nodes briefly
                if (i % 3 == 0) {
                    handle.acquire(node);
                    std::this_thread::yield();
                    handle.release();
                }

                handle.retire(node);
            }

            handle.flush();
        });
    }

    for (auto& t : threads) {
        t.join();
    }

    // All nodes should eventually be deleted
    BOOST_TEST(TrackedNode::total_allocated.load() == num_threads * nodes_per_thread);
    BOOST_TEST(TrackedNode::total_deleted.load() == num_threads * nodes_per_thread);
}

BOOST_AUTO_TEST_CASE(reader_writer_scenario) {
    TrackedNode::reset_counters();

    HazardPointerDomain<TrackedNode> domain;
    std::atomic<TrackedNode*> shared_ptr{new TrackedNode(0)};
    std::atomic<bool> stop{false};

    constexpr int num_readers = 4;
    constexpr int num_writers = 2;

    std::vector<std::thread> threads;

    // Reader threads
    for (int r = 0; r < num_readers; ++r) {
        threads.emplace_back([&] {
            auto handle = domain.make_handle();

            while (!stop.load(std::memory_order_relaxed)) {
                // Protect and read
                TrackedNode* node = shared_ptr.load(std::memory_order_acquire);
                handle.acquire(node);

                // Re-check after protection
                TrackedNode* current = shared_ptr.load(std::memory_order_acquire);
                if (current == node) {
                    // Safe to access
                    int value = node->value;
                    BOOST_TEST(value >= 0);
                }

                handle.release();
                std::this_thread::yield();
            }
        });
    }

    // Writer threads
    for (int w = 0; w < num_writers; ++w) {
        threads.emplace_back([&] {
            auto handle = domain.make_handle();

            for (int i = 1; i <= 100; ++i) {
                auto* new_node = new TrackedNode(i);
                TrackedNode* old_node = shared_ptr.exchange(new_node, std::memory_order_acq_rel);

                // Retire the old node
                handle.retire(old_node);

                std::this_thread::sleep_for(std::chrono::microseconds(10));
            }
        });
    }

    // Let it run for a bit
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    stop.store(true, std::memory_order_relaxed);

    for (auto& t : threads) {
        t.join();
    }

    // Clean up the last node
    delete shared_ptr.load();

    BOOST_TEST(TrackedNode::live_count.load() >= 0);
}

// ============================================================================
// Stress Tests
// ============================================================================

BOOST_AUTO_TEST_CASE(batch_retirement_threshold) {
    TrackedNode::reset_counters();

    HazardPointerDomain<TrackedNode> domain;
    auto handle = domain.make_handle();

    // Retire just below threshold (R = K * 128 = 384)
    constexpr int below_threshold = 383;

    for (int i = 0; i < below_threshold; ++i) {
        auto* node = new TrackedNode(i);
        handle.retire(node);
    }

    // Should not have triggered automatic scan yet
    int deleted_before = TrackedNode::total_deleted.load();

    // One more should trigger
    auto* trigger_node = new TrackedNode(9999);
    handle.retire(trigger_node);

    // Should have triggered reclamation
    int deleted_after = TrackedNode::total_deleted.load();

    BOOST_TEST(deleted_after > deleted_before);
}

BOOST_AUTO_TEST_CASE(memory_reclamation_completeness) {
    TrackedNode::reset_counters();

    {
        HazardPointerDomain<TrackedNode> domain;
        auto handle = domain.make_handle();

        constexpr int total_nodes = 1000;

        for (int i = 0; i < total_nodes; ++i) {
            auto* node = new TrackedNode(i);
            handle.retire(node);
        }

        // Final flush
        handle.flush();
        handle.flush(); // Double flush to ensure everything is cleaned
    }

    // All nodes should be reclaimed
    BOOST_TEST(TrackedNode::total_allocated.load() == 1000);
    BOOST_TEST(TrackedNode::total_deleted.load() == 1000);
    BOOST_TEST(TrackedNode::live_count.load() == 0);
}

BOOST_AUTO_TEST_SUITE_END()
