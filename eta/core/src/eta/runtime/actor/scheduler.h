#pragma once

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

#include "eta/runtime/types/pid.h"

namespace eta::runtime::actor {

/**
 * @brief Fixed-size actor scheduler with per-worker queues and work stealing.
 */
class Scheduler {
public:
    using Runnable = types::Pid;
    using DispatchFn = std::function<void(const Runnable&, std::size_t worker_index)>;
    using DirtyTask = std::function<void()>;

    struct StatsSnapshot {
        std::uint64_t runnable_queue_depth{0};
        std::uint64_t scheduler_wakeups{0};
        std::uint64_t dirty_queue_depth{0};
        std::uint64_t enqueued{0};
        std::uint64_t dequeued{0};
        std::uint64_t steals{0};
        std::uint64_t global_queue_depth{0};
    };

    explicit Scheduler(
        std::size_t worker_count,
        DispatchFn dispatch = {},
        std::size_t dirty_worker_count = 0,
        std::size_t dirty_queue_limit = 0);
    ~Scheduler();

    Scheduler(const Scheduler&) = delete;
    Scheduler& operator=(const Scheduler&) = delete;
    Scheduler(Scheduler&&) = delete;
    Scheduler& operator=(Scheduler&&) = delete;

    /**
     * @brief Start worker threads.
     */
    void start();

    /**
     * @brief Stop worker threads and release run-queue workers.
     */
    void shutdown();

    /**
     * @brief Enqueue runnable work on the global fallback queue.
     */
    [[nodiscard]] bool enqueue(Runnable pid);

    /**
     * @brief Enqueue runnable work on one worker-local queue.
     */
    [[nodiscard]] bool enqueue_for_worker(std::size_t worker_index, Runnable pid);

    /**
     * @brief Enqueue one potentially blocking task on the dirty queue.
     */
    [[nodiscard]] bool enqueue_dirty(DirtyTask task);

    /**
     * @brief Deterministic dequeue helper for tests.
     */
    [[nodiscard]] std::optional<Runnable> try_dequeue_for_worker(std::size_t worker_index);

    /**
     * @brief Deterministic global-queue dequeue helper for tests.
     */
    [[nodiscard]] std::optional<Runnable> try_dequeue_global();

    [[nodiscard]] std::size_t worker_count() const noexcept;
    [[nodiscard]] std::size_t dirty_worker_count() const noexcept;
    [[nodiscard]] StatsSnapshot stats_snapshot() const;

private:
    struct RunQueue {
        mutable std::mutex mutex{};
        std::deque<Runnable> entries{};
    };

    [[nodiscard]] std::optional<Runnable> try_dequeue_internal(
        std::size_t worker_index,
        bool allow_steal);
    void worker_loop(std::size_t worker_index);
    void dirty_worker_loop();

    std::vector<RunQueue> run_queues_{};
    RunQueue global_queue_{};
    DispatchFn dispatch_{};
    std::vector<std::thread> workers_{};
    std::vector<std::thread> dirty_workers_{};
    std::mutex wait_mutex_{};
    std::condition_variable wait_cv_{};
    std::mutex dirty_mutex_{};
    std::condition_variable dirty_cv_{};
    std::deque<DirtyTask> dirty_queue_{};
    std::size_t dirty_worker_count_{0};
    std::size_t dirty_queue_limit_{0};
    std::atomic<bool> running_{false};
    std::atomic<bool> stopping_{false};

    std::atomic<std::uint64_t> runnable_queue_depth_{0};
    std::atomic<std::uint64_t> scheduler_wakeups_{0};
    std::atomic<std::uint64_t> dirty_queue_depth_{0};
    std::atomic<std::uint64_t> enqueued_{0};
    std::atomic<std::uint64_t> dequeued_{0};
    std::atomic<std::uint64_t> steals_{0};
};

} // namespace eta::runtime::actor
