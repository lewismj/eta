#include <boost/test/unit_test.hpp>

#include <atomic>
#include <chrono>
#include <future>
#include <optional>
#include <thread>

#include "eta/runtime/actor/actor_system.h"
#include "eta/runtime/actor/scheduler.h"

namespace {

template <typename Predicate>
[[nodiscard]] bool wait_until(
    Predicate&& predicate,
    std::chrono::milliseconds timeout = std::chrono::milliseconds(1500)) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return predicate();
}

} // namespace

BOOST_AUTO_TEST_SUITE(actor_scheduler_tests)

BOOST_AUTO_TEST_CASE(scheduler_queue_order_and_work_steal_are_deterministic) {
    using eta::runtime::actor::Scheduler;
    using eta::runtime::types::Pid;

    Scheduler scheduler(2);

    const Pid first{.node_id = 1, .actor_id = 1, .incarnation = 1};
    const Pid second{.node_id = 1, .actor_id = 2, .incarnation = 1};
    const Pid global{.node_id = 1, .actor_id = 3, .incarnation = 1};
    const Pid stolen{.node_id = 1, .actor_id = 4, .incarnation = 1};

    BOOST_TEST(scheduler.enqueue_for_worker(0, first));
    BOOST_TEST(scheduler.enqueue_for_worker(0, second));
    BOOST_TEST(scheduler.enqueue(global));
    BOOST_TEST(scheduler.enqueue_for_worker(0, stolen));

    auto first_dequeued = scheduler.try_dequeue_for_worker(0);
    BOOST_REQUIRE(first_dequeued.has_value());
    BOOST_TEST(first_dequeued->node_id == first.node_id);
    BOOST_TEST(first_dequeued->actor_id == first.actor_id);
    BOOST_TEST(first_dequeued->incarnation == first.incarnation);

    auto second_dequeued = scheduler.try_dequeue_for_worker(0);
    BOOST_REQUIRE(second_dequeued.has_value());
    BOOST_TEST(second_dequeued->node_id == second.node_id);
    BOOST_TEST(second_dequeued->actor_id == second.actor_id);
    BOOST_TEST(second_dequeued->incarnation == second.incarnation);

    auto global_dequeued = scheduler.try_dequeue_for_worker(1);
    BOOST_REQUIRE(global_dequeued.has_value());
    BOOST_TEST(global_dequeued->node_id == global.node_id);
    BOOST_TEST(global_dequeued->actor_id == global.actor_id);
    BOOST_TEST(global_dequeued->incarnation == global.incarnation);

    auto stolen_dequeued = scheduler.try_dequeue_for_worker(1);
    BOOST_REQUIRE(stolen_dequeued.has_value());
    BOOST_TEST(stolen_dequeued->node_id == stolen.node_id);
    BOOST_TEST(stolen_dequeued->actor_id == stolen.actor_id);
    BOOST_TEST(stolen_dequeued->incarnation == stolen.incarnation);

    auto stats = scheduler.stats_snapshot();
    BOOST_TEST(stats.runnable_queue_depth == 0u);
    BOOST_TEST(stats.global_queue_depth == 0u);
    BOOST_TEST(stats.enqueued == 4u);
    BOOST_TEST(stats.dequeued == 4u);
    BOOST_TEST(stats.steals >= 1u);
}

BOOST_AUTO_TEST_CASE(actor_run_state_transitions_and_shadow_scheduler_metrics_are_visible) {
    using eta::runtime::actor::ActorSystem;

    ActorSystem system;
    auto main_pid = system.register_current_thread_actor();
    BOOST_REQUIRE(main_pid.has_value());

    system.set_scheduler_mode(ActorSystem::SchedulerMode::PoolShadow);

    auto worker_pid = system.spawn([&system, parent = *main_pid](const eta::runtime::types::Pid& pid) {
        ActorSystem::BinaryMessage started{0x01u};
        (void)system.send(parent, std::move(started));

        auto wake = system.receive(pid, std::nullopt);
        if (!wake.has_value()) return;

        const auto running_until =
            std::chrono::steady_clock::now() + std::chrono::milliseconds(75);
        while (std::chrono::steady_clock::now() < running_until) {
            std::this_thread::yield();
        }
    });
    BOOST_REQUIRE(worker_pid.has_value());

    auto started = system.receive(*main_pid, std::chrono::milliseconds(1000));
    BOOST_REQUIRE(started.has_value());

    const bool observed_waiting = wait_until([&] {
        auto info = system.process_info(*worker_pid);
        return info.has_value()
            && info->run_state == ActorSystem::RunState::Waiting;
    });
    BOOST_REQUIRE(observed_waiting);

    const auto stats_before = system.scheduler_stats();
    BOOST_TEST(
        static_cast<int>(stats_before.mode)
        == static_cast<int>(ActorSystem::SchedulerMode::PoolShadow));

    ActorSystem::BinaryMessage wake{0x7fu};
    BOOST_TEST(system.send(*worker_pid, std::move(wake)));

    const bool observed_running = wait_until([&] {
        auto info = system.process_info(*worker_pid);
        return info.has_value()
            && info->run_state == ActorSystem::RunState::Running;
    });
    BOOST_REQUIRE(observed_running);

    const bool observed_exited = wait_until([&] {
        auto info = system.process_info(*worker_pid);
        return info.has_value()
            && info->run_state == ActorSystem::RunState::Exited;
    }, std::chrono::milliseconds(2500));
    BOOST_REQUIRE(observed_exited);

    const auto stats_after = system.scheduler_stats();
    BOOST_TEST(stats_after.enqueued >= stats_before.enqueued + 1u);
    BOOST_TEST(stats_after.dequeued >= stats_before.dequeued + 1u);
    BOOST_TEST(stats_after.dirty_queue_depth == 0u);
}

BOOST_AUTO_TEST_CASE(scheduler_worker_stays_responsive_while_dirty_task_is_blocked) {
    using eta::runtime::actor::Scheduler;
    using eta::runtime::types::Pid;

    std::atomic<bool> dirty_enqueue_succeeded{true};
    std::atomic<bool> dirty_started_notified{false};
    std::atomic<bool> first_dispatched_notified{false};
    std::atomic<bool> second_dispatched_notified{false};
    std::atomic<bool> dirty_finished{false};
    std::atomic<bool> second_dispatched_before_dirty_finished{false};

    std::promise<void> first_dispatched_promise;
    auto first_dispatched = first_dispatched_promise.get_future();
    std::promise<void> second_dispatched_promise;
    auto second_dispatched = second_dispatched_promise.get_future();
    std::promise<void> dirty_started_promise;
    auto dirty_started = dirty_started_promise.get_future();
    std::promise<void> release_dirty_promise;
    auto release_dirty = release_dirty_promise.get_future().share();

    Scheduler* scheduler_ptr = nullptr;
    Scheduler::DispatchFn dispatch =
        [&dirty_enqueue_succeeded,
         &dirty_started_notified,
         &first_dispatched_notified,
         &second_dispatched_notified,
         &dirty_finished,
         &second_dispatched_before_dirty_finished,
         &first_dispatched_promise,
         &second_dispatched_promise,
         &dirty_started_promise,
         &release_dirty,
         &scheduler_ptr](const Pid& pid, std::size_t) mutable {
            if (pid.actor_id == 1) {
                const bool queued = scheduler_ptr != nullptr
                    && scheduler_ptr->enqueue_dirty(
                        [&dirty_started_notified,
                         &dirty_started_promise,
                         &release_dirty,
                         &dirty_finished]() mutable {
                            if (!dirty_started_notified.exchange(
                                    true,
                                    std::memory_order_acq_rel)) {
                                dirty_started_promise.set_value();
                            }
                            release_dirty.wait();
                            dirty_finished.store(true, std::memory_order_release);
                        });
                if (!queued) {
                    dirty_enqueue_succeeded.store(false, std::memory_order_release);
                }
                if (!first_dispatched_notified.exchange(true, std::memory_order_acq_rel)) {
                    first_dispatched_promise.set_value();
                }
                return;
            }

            if (pid.actor_id == 2) {
                second_dispatched_before_dirty_finished.store(
                    !dirty_finished.load(std::memory_order_acquire),
                    std::memory_order_release);
                if (!second_dispatched_notified.exchange(true, std::memory_order_acq_rel)) {
                    second_dispatched_promise.set_value();
                }
            }
        };

    Scheduler scheduler(
        /*worker_count=*/1,
        std::move(dispatch),
        /*dirty_worker_count=*/1,
        /*dirty_queue_limit=*/0);
    scheduler_ptr = &scheduler;
    scheduler.start();

    const Pid first{.node_id = 1, .actor_id = 1, .incarnation = 1};
    const Pid second{.node_id = 1, .actor_id = 2, .incarnation = 1};

    BOOST_REQUIRE(scheduler.enqueue(first));
    BOOST_REQUIRE(
        first_dispatched.wait_for(std::chrono::milliseconds(1000))
        == std::future_status::ready);
    BOOST_REQUIRE(
        dirty_started.wait_for(std::chrono::milliseconds(1000))
        == std::future_status::ready);

    BOOST_REQUIRE(scheduler.enqueue(second));
    BOOST_REQUIRE(
        second_dispatched.wait_for(std::chrono::milliseconds(1000))
        == std::future_status::ready);

    BOOST_TEST(dirty_enqueue_succeeded.load(std::memory_order_acquire));
    BOOST_TEST(second_dispatched_before_dirty_finished.load(std::memory_order_acquire));

    release_dirty_promise.set_value();
    BOOST_REQUIRE(wait_until([&dirty_finished]() {
        return dirty_finished.load(std::memory_order_acquire);
    }));

    scheduler.shutdown();
}

BOOST_AUTO_TEST_CASE(scheduler_dirty_queue_backpressure_and_shutdown_behavior) {
    using eta::runtime::actor::Scheduler;

    Scheduler scheduler(
        /*worker_count=*/1,
        Scheduler::DispatchFn{},
        /*dirty_worker_count=*/1,
        /*dirty_queue_limit=*/1);
    scheduler.start();

    std::atomic<int> executed{0};
    std::promise<void> first_started_promise;
    auto first_started = first_started_promise.get_future();
    std::promise<void> release_first_promise;
    auto release_first = release_first_promise.get_future().share();

    BOOST_REQUIRE(scheduler.enqueue_dirty([&executed, &first_started_promise, release_first]() mutable {
        first_started_promise.set_value();
        release_first.wait();
        executed.fetch_add(1, std::memory_order_relaxed);
    }));

    BOOST_REQUIRE(
        first_started.wait_for(std::chrono::milliseconds(1000))
        == std::future_status::ready);

    BOOST_REQUIRE(scheduler.enqueue_dirty([&executed]() {
        executed.fetch_add(1, std::memory_order_relaxed);
    }));
    BOOST_TEST(!scheduler.enqueue_dirty([&executed]() {
        executed.fetch_add(1, std::memory_order_relaxed);
    }));

    release_first_promise.set_value();
    BOOST_REQUIRE(wait_until([&executed]() {
        return executed.load(std::memory_order_relaxed) == 2;
    }));

    scheduler.shutdown();

    auto stats = scheduler.stats_snapshot();
    BOOST_TEST(stats.dirty_queue_depth == 0u);
    BOOST_TEST(!scheduler.enqueue_dirty([] {}));
}

BOOST_AUTO_TEST_SUITE_END()
