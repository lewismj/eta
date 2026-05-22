#include "eta/runtime/actor/scheduler.h"

#include <chrono>
#include <utility>

namespace eta::runtime::actor {

Scheduler::Scheduler(std::size_t worker_count, DispatchFn dispatch)
    : run_queues_(worker_count == 0 ? 1u : worker_count),
      dispatch_(std::move(dispatch)) {}

Scheduler::~Scheduler() {
    shutdown();
}

void Scheduler::start() {
    if (running_.exchange(true, std::memory_order_acq_rel)) return;

    stopping_.store(false, std::memory_order_release);
    workers_.reserve(run_queues_.size());
    for (std::size_t worker_index = 0; worker_index < run_queues_.size(); ++worker_index) {
        workers_.emplace_back([this, worker_index] {
            worker_loop(worker_index);
        });
    }
}

void Scheduler::shutdown() {
    stopping_.store(true, std::memory_order_release);
    wait_cv_.notify_all();

    for (auto& worker : workers_) {
        if (!worker.joinable()) continue;
        if (worker.get_id() == std::this_thread::get_id()) continue;
        worker.join();
    }
    workers_.clear();
    running_.store(false, std::memory_order_release);
}

bool Scheduler::enqueue(Runnable pid) {
    {
        std::lock_guard lock(global_queue_.mutex);
        global_queue_.entries.push_back(std::move(pid));
    }
    runnable_queue_depth_.fetch_add(1, std::memory_order_relaxed);
    enqueued_.fetch_add(1, std::memory_order_relaxed);
    wait_cv_.notify_one();
    return true;
}

bool Scheduler::enqueue_for_worker(std::size_t worker_index, Runnable pid) {
    if (worker_index >= run_queues_.size()) {
        return enqueue(std::move(pid));
    }

    {
        std::lock_guard lock(run_queues_[worker_index].mutex);
        run_queues_[worker_index].entries.push_back(std::move(pid));
    }
    runnable_queue_depth_.fetch_add(1, std::memory_order_relaxed);
    enqueued_.fetch_add(1, std::memory_order_relaxed);
    wait_cv_.notify_one();
    return true;
}

std::optional<Scheduler::Runnable> Scheduler::try_dequeue_for_worker(std::size_t worker_index) {
    if (worker_index >= run_queues_.size()) return std::nullopt;
    return try_dequeue_internal(worker_index, true);
}

std::optional<Scheduler::Runnable> Scheduler::try_dequeue_global() {
    Runnable next;
    {
        std::lock_guard lock(global_queue_.mutex);
        if (global_queue_.entries.empty()) return std::nullopt;
        next = std::move(global_queue_.entries.front());
        global_queue_.entries.pop_front();
    }
    runnable_queue_depth_.fetch_sub(1, std::memory_order_relaxed);
    dequeued_.fetch_add(1, std::memory_order_relaxed);
    return next;
}

std::size_t Scheduler::worker_count() const noexcept {
    return run_queues_.size();
}

Scheduler::StatsSnapshot Scheduler::stats_snapshot() const {
    StatsSnapshot snapshot;
    snapshot.runnable_queue_depth = runnable_queue_depth_.load(std::memory_order_relaxed);
    snapshot.scheduler_wakeups = scheduler_wakeups_.load(std::memory_order_relaxed);
    snapshot.dirty_queue_depth = 0;
    snapshot.enqueued = enqueued_.load(std::memory_order_relaxed);
    snapshot.dequeued = dequeued_.load(std::memory_order_relaxed);
    snapshot.steals = steals_.load(std::memory_order_relaxed);
    {
        std::lock_guard lock(global_queue_.mutex);
        snapshot.global_queue_depth =
            static_cast<std::uint64_t>(global_queue_.entries.size());
    }
    return snapshot;
}

std::optional<Scheduler::Runnable> Scheduler::try_dequeue_internal(
    std::size_t worker_index,
    bool allow_steal) {
    if (worker_index >= run_queues_.size()) return std::nullopt;

    {
        std::lock_guard lock(run_queues_[worker_index].mutex);
        if (!run_queues_[worker_index].entries.empty()) {
            auto next = std::move(run_queues_[worker_index].entries.front());
            run_queues_[worker_index].entries.pop_front();
            runnable_queue_depth_.fetch_sub(1, std::memory_order_relaxed);
            dequeued_.fetch_add(1, std::memory_order_relaxed);
            return next;
        }
    }

    {
        std::lock_guard lock(global_queue_.mutex);
        if (!global_queue_.entries.empty()) {
            auto next = std::move(global_queue_.entries.front());
            global_queue_.entries.pop_front();
            runnable_queue_depth_.fetch_sub(1, std::memory_order_relaxed);
            dequeued_.fetch_add(1, std::memory_order_relaxed);
            return next;
        }
    }

    if (!allow_steal) return std::nullopt;

    for (std::size_t offset = 1; offset < run_queues_.size(); ++offset) {
        const auto victim_index = (worker_index + offset) % run_queues_.size();
        std::lock_guard lock(run_queues_[victim_index].mutex);
        if (run_queues_[victim_index].entries.empty()) continue;

        auto stolen = std::move(run_queues_[victim_index].entries.back());
        run_queues_[victim_index].entries.pop_back();
        runnable_queue_depth_.fetch_sub(1, std::memory_order_relaxed);
        dequeued_.fetch_add(1, std::memory_order_relaxed);
        steals_.fetch_add(1, std::memory_order_relaxed);
        return stolen;
    }

    return std::nullopt;
}

void Scheduler::worker_loop(std::size_t worker_index) {
    for (;;) {
        auto runnable = try_dequeue_internal(worker_index, true);
        if (runnable.has_value()) {
            if (dispatch_) {
                dispatch_(*runnable, worker_index);
            }
            continue;
        }

        if (stopping_.load(std::memory_order_acquire)) break;

        std::unique_lock wait_lock(wait_mutex_);
        wait_cv_.wait_for(wait_lock, std::chrono::milliseconds(10), [this] {
            return stopping_.load(std::memory_order_acquire)
                || runnable_queue_depth_.load(std::memory_order_relaxed) > 0;
        });
        scheduler_wakeups_.fetch_add(1, std::memory_order_relaxed);
    }
}

} // namespace eta::runtime::actor
