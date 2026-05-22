#include "eta/runtime/actor/actor_system.h"

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <deque>
#include <limits>
#include <utility>

#include "eta/runtime/actor/node_transport.h"
#include "eta/runtime/actor/scheduler.h"

namespace eta::runtime::actor {

namespace {

struct ThreadActorBinding {
    const ActorSystem* system{nullptr};
    types::Pid pid{};
    bool active{false};
    std::atomic<std::uint64_t>* reductions_counter{nullptr};
    std::atomic<ActorSystem::YieldReason>* yield_reason{nullptr};
};

thread_local ThreadActorBinding g_thread_actor_binding{};
std::atomic<std::uint64_t> g_next_node_id{1};

[[nodiscard]] std::size_t hash_combine(std::size_t seed, std::size_t value) noexcept {
    seed ^= value + 0x9e3779b97f4a7c15ULL + (seed << 6) + (seed >> 2);
    return seed;
}

void apply_outbox_messages(
    std::vector<std::pair<std::shared_ptr<Mailbox>, ActorSystem::Message>>& outbox) {
    for (auto& [mailbox, message] : outbox) {
        if (!mailbox) continue;
        (void)mailbox->push(std::move(message));
    }
    outbox.clear();
}

[[nodiscard]] std::size_t parse_size_t_env(
    const char* name,
    std::size_t default_value) noexcept {
    const char* value = std::getenv(name);
    if (!value || value[0] == '\0') return default_value;

    char* end = nullptr;
    errno = 0;
    const auto parsed = std::strtoull(value, &end, 10);
    if (end == value || errno == ERANGE) return default_value;
    if (end && *end != '\0') return default_value;
    if (parsed == 0ULL) return default_value;

    constexpr auto kMaxSizeT = static_cast<unsigned long long>(
        (std::numeric_limits<std::size_t>::max)());
    if (parsed > kMaxSizeT) return default_value;
    return static_cast<std::size_t>(parsed);
}

[[nodiscard]] bool pid_less(const types::Pid& lhs, const types::Pid& rhs) noexcept {
    if (lhs.node_id != rhs.node_id) return lhs.node_id < rhs.node_id;
    if (lhs.actor_id != rhs.actor_id) return lhs.actor_id < rhs.actor_id;
    return lhs.incarnation < rhs.incarnation;
}

} // namespace

ActorSystem::ActorSystem()
    : local_node_id_(allocate_local_node_id()) {
    node_name_ = "eta@node-" + std::to_string(local_node_id_);
    node_transport_ = std::make_unique<NodeTransport>(
        local_node_id_,
        NodeTransport::Callbacks{
            .deliver_message = [this](const types::Pid& pid, BinaryMessage payload) {
                return deliver_remote_payload(pid, std::move(payload));
            },
            .remote_monitor = [this](
                                  const types::Pid& watcher,
                                  const types::Pid& target,
                                  MonitorRef ref) {
                handle_remote_monitor_request(watcher, target, ref);
            },
            .remote_demonitor = [this](
                                    const types::Pid& watcher,
                                    const types::Pid& target,
                                    MonitorRef ref) {
                handle_remote_demonitor_request(watcher, target, ref);
            },
            .remote_down = [this](const NodeTransport::RemoteDownSignal& signal) {
                handle_remote_down_signal(signal);
            },
            .node_up = [this](const NodeTransport::ConnectedNode& node) {
                handle_node_up(node);
            },
            .node_down = [this](
                             const NodeTransport::ConnectedNode& node,
                             NodeTransport::NodeDownReason reason) {
                handle_node_down(node, reason);
            },
        });
    if (node_transport_) {
        (void)node_transport_->configure(node_name_, node_cookie_, nullptr);
    }
}

ActorSystem::~ActorSystem() {
    shutdown();
}

std::expected<types::Pid, std::string> ActorSystem::register_current_thread_actor() {
    if (auto pid = current_pid(); pid.has_value()) {
        return *pid;
    }

    auto process = std::make_shared<ActorProcess>();
    {
        std::lock_guard lock(mutex_);
        if (shutting_down_.load(std::memory_order_acquire)) {
            return std::unexpected("actor system is shutting down");
        }
        process->pid = allocate_pid_unsafe();
        process->is_main_thread_actor = true;
        processes_.emplace(process->pid, process);
    }

    if (!bind_current_thread_pid(process->pid)) {
        std::lock_guard lock(mutex_);
        processes_.erase(process->pid);
        return std::unexpected("failed to bind main actor thread");
    }
    return process->pid;
}

std::expected<types::Pid, std::string> ActorSystem::spawn(ActorEntry entry) {
    if (!entry) {
        return std::unexpected("spawn requires a non-empty actor entrypoint");
    }

    auto process = std::make_shared<ActorProcess>();
    bool pool_managed = false;
    {
        std::lock_guard lock(mutex_);
        if (shutting_down_.load(std::memory_order_acquire)) {
            return std::unexpected("actor system is shutting down");
        }
        process->pid = allocate_pid_unsafe();
        process->entry = std::move(entry);
        pool_managed = scheduler_mode_ == SchedulerMode::Pool;
        process->managed_by_scheduler = pool_managed;
        processes_.emplace(process->pid, process);
        if (pool_managed) {
            if (!scheduler_) {
                processes_.erase(process->pid);
                return std::unexpected("scheduler pool is not available");
            }
            enqueue_runnable_unsafe(process);
        } else {
            enqueue_runnable_unsafe(process);
        }
    }

    if (pool_managed) {
        return process->pid;
    }

    try {
        process->worker = std::thread([this, process]() mutable {
                const auto pid = process->pid;
                ExitReason reason;
                reason.kind = ExitReason::Kind::Normal;

                if (!bind_current_thread_pid(pid)) {
                    reason.kind = ExitReason::Kind::Error;
                    complete_actor(pid, reason);
                    return;
                }

                try {
                    process->entry(pid);
                } catch (...) {
                    reason.kind = ExitReason::Kind::Error;
                }

                complete_actor(pid, reason);
                unbind_current_thread_pid();
            });
    } catch (...) {
        std::lock_guard lock(mutex_);
        processes_.erase(process->pid);
        return std::unexpected("failed to create actor thread");
    }

    return process->pid;
}

bool ActorSystem::bind_current_thread_pid(const types::Pid& pid) {
    std::lock_guard lock(mutex_);
    auto process = lookup_process_unsafe(pid);
    if (!process) return false;
    if (!process->alive.load(std::memory_order_acquire)) return false;

    g_thread_actor_binding.system = this;
    g_thread_actor_binding.pid = pid;
    g_thread_actor_binding.active = true;
    g_thread_actor_binding.reductions_counter = &process->reductions;
    g_thread_actor_binding.yield_reason = &process->last_yield_reason;
    process->run_state.store(RunState::Running, std::memory_order_relaxed);
    process->last_yield_reason.store(
        YieldReason::None,
        std::memory_order_relaxed);
    return true;
}

void ActorSystem::unbind_current_thread_pid() {
    if (g_thread_actor_binding.system != this) return;
    g_thread_actor_binding.system = nullptr;
    g_thread_actor_binding.pid = {};
    g_thread_actor_binding.active = false;
    g_thread_actor_binding.reductions_counter = nullptr;
    g_thread_actor_binding.yield_reason = nullptr;
}

std::optional<types::Pid> ActorSystem::current_pid() const {
    if (g_thread_actor_binding.system != this || !g_thread_actor_binding.active) {
        return std::nullopt;
    }
    return g_thread_actor_binding.pid;
}

bool ActorSystem::send(const types::Pid& pid, BinaryMessage message) {
    return send_checked(pid, std::move(message)) == SendStatus::Delivered;
}

ActorSystem::SendStatus ActorSystem::send_checked(const types::Pid& pid, BinaryMessage message) {
    if (pid.node_id == local_node_id_) {
        std::vector<std::pair<std::shared_ptr<Mailbox>, Message>> outbox;
        SendStatus status = SendStatus::NoSuchPid;
        {
            std::lock_guard lock(mutex_);
            auto process = lookup_process_unsafe(pid);
            if (!process) {
                status = SendStatus::NoSuchPid;
            } else if (!process->alive.load(std::memory_order_acquire)) {
                status = SendStatus::DeadPid;
            } else {
                status = enqueue_message_unsafe(
                    pid,
                    Message::make_payload(std::move(message)),
                    outbox)
                    ? SendStatus::Delivered
                    : SendStatus::DeadPid;
            }
        }
        apply_outbox_messages(outbox);
        return status;
    }

    if (!node_transport_) {
        return SendStatus::NoRoute;
    }

    auto sender = current_pid();
    const types::Pid from = sender.value_or(types::Pid{
        .node_id = local_node_id_,
        .actor_id = 0,
        .incarnation = 0});
    auto result = node_transport_->send_remote(from, pid, std::move(message));
    switch (result.code) {
        case NodeTransport::SendResultCode::Delivered:
            return SendStatus::Delivered;
        case NodeTransport::SendResultCode::NoRoute:
            return SendStatus::NoRoute;
        case NodeTransport::SendResultCode::TransportError:
            return SendStatus::TransportError;
    }
    return SendStatus::TransportError;
}

std::string ActorSystem::node_name() const {
    std::lock_guard lock(mutex_);
    return node_name_;
}

std::uint64_t ActorSystem::local_node_id() const noexcept {
    return local_node_id_;
}

bool ActorSystem::configure_node(
    std::string node_name,
    std::string cookie,
    std::string* error_message) {
    if (node_name.empty()) {
        if (error_message) *error_message = "node name must be non-empty";
        return false;
    }
    if (!node_transport_) {
        if (error_message) *error_message = "node transport is unavailable";
        return false;
    }

    if (!node_transport_->configure(node_name, cookie, error_message)) {
        return false;
    }

    {
        std::lock_guard lock(mutex_);
        node_name_ = std::move(node_name);
        node_cookie_ = std::move(cookie);
    }
    return true;
}

bool ActorSystem::node_listen(
    std::string endpoint,
    std::string* error_message) {
    if (!node_transport_) {
        if (error_message) *error_message = "node transport is unavailable";
        return false;
    }
    return node_transport_->listen(std::move(endpoint), error_message);
}

bool ActorSystem::node_connect(
    std::string endpoint,
    std::string* error_message) {
    if (!node_transport_) {
        if (error_message) *error_message = "node transport is unavailable";
        return false;
    }
    return node_transport_->connect(std::move(endpoint), error_message);
}

bool ActorSystem::disconnect_node(std::string_view node_name) {
    if (!node_transport_) return false;
    return node_transport_->disconnect_node(node_name);
}

std::vector<ActorSystem::ConnectedNode> ActorSystem::connected_nodes() const {
    if (!node_transport_) return {};
    const auto nodes = node_transport_->connected_nodes();
    std::vector<ConnectedNode> out;
    out.reserve(nodes.size());
    for (const auto& node : nodes) {
        out.push_back(ConnectedNode{
            .node_id = node.node_id,
            .node_name = node.node_name,
            .endpoint = node.endpoint});
    }
    return out;
}

bool ActorSystem::deliver_remote_payload(const types::Pid& pid, BinaryMessage payload) {
    if (pid.node_id != local_node_id_) return false;

    std::vector<std::pair<std::shared_ptr<Mailbox>, Message>> outbox;
    bool queued = false;
    {
        std::lock_guard lock(mutex_);
        queued = enqueue_message_unsafe(
            pid,
            Message::make_payload(std::move(payload)),
            outbox);
    }
    apply_outbox_messages(outbox);
    return queued;
}

bool ActorSystem::set_trap_exit(const types::Pid& pid, bool enabled) {
    std::lock_guard lock(mutex_);
    auto process = lookup_process_unsafe(pid);
    if (!process) return false;
    if (!process->alive.load(std::memory_order_acquire)) return false;
    process->trap_exit = enabled;
    return true;
}

bool ActorSystem::link(const types::Pid& lhs, const types::Pid& rhs) {
    std::lock_guard lock(mutex_);
    auto left = lookup_process_unsafe(lhs);
    if (!left || !left->alive.load(std::memory_order_acquire)) return false;

    if (lhs == rhs) return true;

    auto right = lookup_process_unsafe(rhs);
    if (!right || !right->alive.load(std::memory_order_acquire)) return false;

    left->links.insert(rhs);
    right->links.insert(lhs);
    return true;
}

bool ActorSystem::unlink(const types::Pid& lhs, const types::Pid& rhs) {
    std::lock_guard lock(mutex_);

    auto left = lookup_process_unsafe(lhs);
    auto right = lookup_process_unsafe(rhs);

    bool changed = false;
    if (left) changed = left->links.erase(rhs) > 0 || changed;
    if (right) changed = right->links.erase(lhs) > 0 || changed;
    return changed;
}

std::optional<ActorSystem::MonitorRef> ActorSystem::monitor(
    const types::Pid& watcher,
    const types::Pid& target) {
    std::vector<std::pair<std::shared_ptr<Mailbox>, Message>> outbox;
    bool enqueue_remote_failure = false;

    MonitorRef ref = 0;
    {
        std::lock_guard lock(mutex_);

        auto watcher_process = lookup_process_unsafe(watcher);
        if (!watcher_process) return std::nullopt;
        if (!watcher_process->alive.load(std::memory_order_acquire)) return std::nullopt;

        ref = next_monitor_ref_.fetch_add(1, std::memory_order_relaxed);
        if (ref == 0) {
            ref = next_monitor_ref_.fetch_add(1, std::memory_order_relaxed);
            if (ref == 0) return std::nullopt;
        }

        watcher_process->monitors.insert(ref);
        MonitorSubscription subscription;
        subscription.ref = ref;
        subscription.watcher = watcher;
        subscription.target = target;
        subscription.kind = (target.node_id == local_node_id_)
            ? MonitorSubscription::Kind::LocalProcess
            : MonitorSubscription::Kind::RemoteProcess;
        monitor_refs_[ref] = subscription;

        if (subscription.kind == MonitorSubscription::Kind::LocalProcess) {
            auto target_process = lookup_process_unsafe(target);
            if (!target_process || !target_process->alive.load(std::memory_order_acquire)) {
                erase_monitor_unsafe(ref);
                ExitReason down_reason;
                down_reason.kind = ExitReason::Kind::Error;
                (void)enqueue_message_unsafe(
                    watcher,
                    Message::make_down(ref, target, down_reason),
                    outbox);
            }
        } else if (!node_transport_) {
            erase_monitor_unsafe(ref);
            enqueue_remote_failure = true;
        }
    }

    if (!enqueue_remote_failure && target.node_id != local_node_id_ && node_transport_) {
        auto sent = node_transport_->send_remote_monitor(watcher, target, ref);
        if (sent.code != NodeTransport::SendResultCode::Delivered) {
            enqueue_remote_failure = true;
        }
    }

    if (enqueue_remote_failure) {
        std::lock_guard lock(mutex_);
        auto monitor_it = monitor_refs_.find(ref);
        if (monitor_it != monitor_refs_.end() && monitor_it->second.watcher == watcher) {
            erase_monitor_unsafe(ref);
            ExitReason down_reason;
            down_reason.kind = ExitReason::Kind::NoConnection;
            (void)enqueue_message_unsafe(
                watcher,
                Message::make_down(ref, target, down_reason),
                outbox);
        }
    }

    apply_outbox_messages(outbox);
    return ref;
}

std::optional<ActorSystem::MonitorRef> ActorSystem::monitor_node(
    const types::Pid& watcher,
    std::string node_name) {
    if (node_name.empty()) return std::nullopt;

    std::vector<std::pair<std::shared_ptr<Mailbox>, Message>> outbox;
    MonitorRef ref = 0;

    {
        std::lock_guard lock(mutex_);
        auto watcher_process = lookup_process_unsafe(watcher);
        if (!watcher_process) return std::nullopt;
        if (!watcher_process->alive.load(std::memory_order_acquire)) return std::nullopt;

        ref = next_monitor_ref_.fetch_add(1, std::memory_order_relaxed);
        if (ref == 0) {
            ref = next_monitor_ref_.fetch_add(1, std::memory_order_relaxed);
            if (ref == 0) return std::nullopt;
        }

        watcher_process->monitors.insert(ref);
        MonitorSubscription subscription;
        subscription.ref = ref;
        subscription.watcher = watcher;
        subscription.kind = MonitorSubscription::Kind::Node;
        subscription.node_name = node_name;
        monitor_refs_[ref] = std::move(subscription);
    }

    if (node_transport_) {
        const auto nodes = node_transport_->connected_nodes();
        auto found = std::find_if(nodes.begin(), nodes.end(), [&node_name](const auto& node) {
            return node.node_name == node_name;
        });
        if (found != nodes.end()) {
            std::lock_guard lock(mutex_);
            (void)enqueue_message_unsafe(
                watcher,
                Message::make_node_up(ref, found->node_name, found->node_id),
                outbox);
        }
    }

    apply_outbox_messages(outbox);
    return ref;
}

bool ActorSystem::demonitor(
    const types::Pid& watcher,
    MonitorRef ref,
    bool flush_down_message) {
    std::shared_ptr<Mailbox> watcher_mailbox;
    std::optional<MonitorSubscription> removed_subscription;
    bool removed = false;

    {
        std::lock_guard lock(mutex_);
        auto watcher_process = lookup_process_unsafe(watcher);
        if (!watcher_process) return false;
        if (!watcher_process->alive.load(std::memory_order_acquire)) return false;

        watcher_mailbox = watcher_process->mailbox;

        if (watcher_process->monitors.contains(ref)) {
            auto monitor_it = monitor_refs_.find(ref);
            if (monitor_it != monitor_refs_.end()) {
                removed_subscription = monitor_it->second;
            }
            erase_monitor_unsafe(ref);
            removed = true;
        }
    }

    if (removed
        && removed_subscription.has_value()
        && removed_subscription->kind == MonitorSubscription::Kind::RemoteProcess
        && node_transport_) {
        (void)node_transport_->send_remote_demonitor(
            watcher,
            removed_subscription->target,
            ref);
    }

    if (flush_down_message && watcher_mailbox) {
        flush_monitor_messages_unsafe(watcher_mailbox, ref, flush_down_message);
    }

    return removed;
}

bool ActorSystem::signal_exit(
    const std::optional<types::Pid>& from,
    const types::Pid& target,
    ExitReason reason,
    bool untrappable) {
    std::vector<std::pair<std::shared_ptr<Mailbox>, Message>> outbox;
    std::vector<NodeTransport::RemoteDownSignal> remote_down_signals;
    std::vector<RemoteMonitorSubscription> remote_demonitors;
    bool handled = false;

    {
        std::lock_guard lock(mutex_);

        auto target_process = lookup_process_unsafe(target);
        if (!target_process) return false;
        if (!target_process->alive.load(std::memory_order_acquire)) return false;

        const bool self_signal = from.has_value() && *from == target;
        if (!self_signal && !untrappable && is_normal_exit(reason)) {
            handled = true;
        } else if (!self_signal && !untrappable && target_process->trap_exit && from.has_value()) {
            handled = enqueue_message_unsafe(
                target,
                Message::make_exit(*from, std::move(reason)),
                outbox);
        } else {
            terminate_actor_chain_unsafe(
                target,
                std::move(reason),
                outbox,
                remote_down_signals,
                remote_demonitors);
            handled = true;
        }
    }

    apply_outbox_messages(outbox);
    apply_remote_demonitors(remote_demonitors);
    apply_remote_down_signals(remote_down_signals);
    return handled;
}

void ActorSystem::complete_actor(const types::Pid& pid, ExitReason reason) {
    std::vector<std::pair<std::shared_ptr<Mailbox>, Message>> outbox;
    std::vector<NodeTransport::RemoteDownSignal> remote_down_signals;
    std::vector<RemoteMonitorSubscription> remote_demonitors;
    {
        std::lock_guard lock(mutex_);
        terminate_actor_chain_unsafe(
            pid,
            std::move(reason),
            outbox,
            remote_down_signals,
            remote_demonitors);
    }
    apply_outbox_messages(outbox);
    apply_remote_demonitors(remote_demonitors);
    apply_remote_down_signals(remote_down_signals);
}

std::optional<ActorSystem::Message> ActorSystem::receive(
    const types::Pid& pid,
    std::optional<std::chrono::milliseconds> timeout) {
    std::shared_ptr<Mailbox> mailbox;
    std::shared_ptr<ActorProcess> process;
    {
        std::lock_guard lock(mutex_);
        process = lookup_process_unsafe(pid);
        if (!process) return std::nullopt;
        mailbox = process->mailbox;
        mark_process_waiting_if_blocking_receive_unsafe(process, timeout);
    }

    auto message = mailbox->pop(timeout);

    {
        std::lock_guard lock(mutex_);
        auto refreshed = lookup_process_unsafe(pid);
        if (!refreshed) return message;
        if (refreshed->alive.load(std::memory_order_acquire)) {
            mark_process_running_unsafe(refreshed);
        } else {
            mark_process_exited_unsafe(refreshed);
        }
    }

    return message;
}

std::expected<std::optional<ActorSystem::Message>, error::RuntimeError>
ActorSystem::receive_matching(
    const types::Pid& pid,
    std::optional<std::chrono::milliseconds> timeout,
    const MessageMatcher& matcher) {
    std::shared_ptr<Mailbox> mailbox;
    std::shared_ptr<ActorProcess> process;
    {
        std::lock_guard lock(mutex_);
        process = lookup_process_unsafe(pid);
        if (!process) return std::optional<Message>{};
        mailbox = process->mailbox;
        mark_process_waiting_if_blocking_receive_unsafe(process, timeout);
    }

    auto matched = mailbox->pop_matching(timeout, matcher);

    {
        std::lock_guard lock(mutex_);
        auto refreshed = lookup_process_unsafe(pid);
        if (refreshed) {
            if (refreshed->alive.load(std::memory_order_acquire)) {
                mark_process_running_unsafe(refreshed);
            } else {
                mark_process_exited_unsafe(refreshed);
            }
        }
    }

    return matched;
}

std::optional<std::size_t> ActorSystem::mailbox_size(const types::Pid& pid) const {
    std::shared_ptr<Mailbox> mailbox;
    {
        std::lock_guard lock(mutex_);
        auto process = lookup_process_unsafe(pid);
        if (!process) return std::nullopt;
        mailbox = process->mailbox;
    }
    return mailbox->size();
}

std::optional<ActorSystem::ProcessInfo> ActorSystem::process_info(
    const types::Pid& pid) const {
    std::shared_ptr<ActorProcess> process;
    ProcessInfo info;
    {
        std::lock_guard lock(mutex_);
        process = lookup_process_unsafe(pid);
        if (!process) return std::nullopt;

        info.pid = process->pid;
        info.alive = process->alive.load(std::memory_order_acquire);
        info.registered_name = process->registered_name;
        info.reductions = process->reductions.load(std::memory_order_relaxed);
        info.run_state = process->run_state.load(std::memory_order_relaxed);
        info.last_yield_reason = process->last_yield_reason.load(std::memory_order_relaxed);

        info.links.reserve(process->links.size());
        for (const auto& linked_pid : process->links) {
            info.links.push_back(linked_pid);
        }

        info.monitors.reserve(process->monitors.size());
        for (const auto ref : process->monitors) {
            info.monitors.push_back(ref);
        }
    }

    info.mailbox_length = process->mailbox->size();

    std::sort(
        info.links.begin(),
        info.links.end(),
        pid_less);
    std::sort(info.monitors.begin(), info.monitors.end());
    return info;
}

std::vector<ActorSystem::ProcessInfo> ActorSystem::list_processes() const {
    std::vector<ProcessInfo> infos;
    {
        std::lock_guard lock(mutex_);
        infos.reserve(processes_.size());
        for (const auto& [_, process] : processes_) {
            if (!process) continue;

            ProcessInfo info;
            info.pid = process->pid;
            info.alive = process->alive.load(std::memory_order_acquire);
            info.mailbox_length = process->mailbox->size();
            info.registered_name = process->registered_name;
            info.reductions = process->reductions.load(std::memory_order_relaxed);
            info.run_state = process->run_state.load(std::memory_order_relaxed);
            info.last_yield_reason = process->last_yield_reason.load(std::memory_order_relaxed);

            info.links.reserve(process->links.size());
            for (const auto& linked_pid : process->links) {
                info.links.push_back(linked_pid);
            }
            std::sort(info.links.begin(), info.links.end(), pid_less);

            info.monitors.reserve(process->monitors.size());
            for (const auto ref : process->monitors) {
                info.monitors.push_back(ref);
            }
            std::sort(info.monitors.begin(), info.monitors.end());

            infos.push_back(std::move(info));
        }
    }

    std::sort(
        infos.begin(),
        infos.end(),
        [](const ProcessInfo& lhs, const ProcessInfo& rhs) {
            return pid_less(lhs.pid, rhs.pid);
        });
    return infos;
}

void ActorSystem::set_scheduler_mode(SchedulerMode mode) {
    std::lock_guard lock(mutex_);

    if (scheduler_) {
        scheduler_->shutdown();
        scheduler_.reset();
    }

    scheduler_mode_ = mode;

    if (mode == SchedulerMode::ThreadPerActor) return;

    const auto dirty_worker_count = parse_size_t_env("ETA_ACTOR_DIRTY_SCHEDULERS", 0u);
    const auto dirty_queue_limit = parse_size_t_env("ETA_ACTOR_DIRTY_QUEUE_LIMIT", 0u);

    Scheduler::DispatchFn dispatch{};
    if (mode == SchedulerMode::Pool) {
        dispatch = [this](const types::Pid& pid, std::size_t worker_index) {
            dispatch_pool_runnable(pid, worker_index);
        };
    } else {
        dispatch = [this](const types::Pid& pid, std::size_t) {
            dispatch_shadow_runnable(pid);
        };
    }

    scheduler_ = std::make_unique<Scheduler>(
        default_scheduler_worker_count(),
        std::move(dispatch),
        dirty_worker_count,
        dirty_queue_limit);
    scheduler_->start();
}

ActorSystem::SchedulerMode ActorSystem::scheduler_mode() const {
    std::lock_guard lock(mutex_);
    return scheduler_mode_;
}

ActorSystem::SchedulerStats ActorSystem::scheduler_stats() const {
    std::lock_guard lock(mutex_);
    SchedulerStats stats;
    stats.mode = scheduler_mode_;
    if (!scheduler_) return stats;

    const auto snapshot = scheduler_->stats_snapshot();
    stats.runnable_queue_depth = snapshot.runnable_queue_depth;
    stats.scheduler_wakeups = snapshot.scheduler_wakeups;
    stats.dirty_queue_depth = snapshot.dirty_queue_depth;
    stats.enqueued = snapshot.enqueued;
    stats.dequeued = snapshot.dequeued;
    stats.steals = snapshot.steals;
    stats.global_queue_depth = snapshot.global_queue_depth;
    return stats;
}

bool ActorSystem::enqueue_dirty_task(std::function<void()> task) {
    std::lock_guard lock(mutex_);
    if (scheduler_mode_ != SchedulerMode::Pool) return false;
    if (!scheduler_) return false;
    return scheduler_->enqueue_dirty(std::move(task));
}

void ActorSystem::notify_external_runnable(const types::Pid& pid) {
    std::lock_guard lock(mutex_);
    auto process = lookup_process_unsafe(pid);
    if (!process) return;
    if (!process->alive.load(std::memory_order_acquire)) return;
    if (scheduler_mode_ == SchedulerMode::Pool && !process->managed_by_scheduler) return;

    process->run_state.store(RunState::Runnable, std::memory_order_relaxed);
    if (scheduler_mode_ == SchedulerMode::Pool) {
        if (process->in_dispatch.load(std::memory_order_acquire)) return;
        enqueue_runnable_unsafe(process);
        return;
    }
    if (scheduler_mode_ == SchedulerMode::PoolShadow) {
        enqueue_runnable_unsafe(process);
    }
}

bool ActorSystem::pid_exists(const types::Pid& pid) const {
    std::lock_guard lock(mutex_);
    auto process = lookup_process_unsafe(pid);
    if (!process) return false;
    return process->alive.load(std::memory_order_acquire);
}

bool ActorSystem::register_name(std::string name, const types::Pid& pid) {
    if (name.empty()) return false;

    std::lock_guard lock(mutex_);

    auto process = lookup_process_unsafe(pid);
    if (!process) return false;
    if (!process->alive.load(std::memory_order_acquire)) return false;

    auto by_name = registry_by_name_.find(name);
    if (by_name != registry_by_name_.end() && by_name->second != pid) {
        return false;
    }

    auto by_pid = registry_by_pid_.find(pid);
    if (by_pid != registry_by_pid_.end() && by_pid->second != name) {
        return false;
    }

    if (process->registered_name.empty()) {
        process->registered_name = name;
    } else if (process->registered_name != name) {
        return false;
    }

    registry_by_name_[name] = pid;
    registry_by_pid_[pid] = std::move(name);
    return true;
}

bool ActorSystem::unregister_name(std::string_view name) {
    if (name.empty()) return false;

    std::lock_guard lock(mutex_);

    auto by_name = registry_by_name_.find(std::string(name));
    if (by_name == registry_by_name_.end()) return false;

    const auto pid = by_name->second;
    registry_by_name_.erase(by_name);

    auto by_pid = registry_by_pid_.find(pid);
    if (by_pid != registry_by_pid_.end()) {
        registry_by_pid_.erase(by_pid);
    }

    auto process = lookup_process_unsafe(pid);
    if (process) {
        process->registered_name.clear();
    }
    return true;
}

std::optional<types::Pid> ActorSystem::whereis(std::string_view name) const {
    if (name.empty()) return std::nullopt;

    std::lock_guard lock(mutex_);
    auto it = registry_by_name_.find(std::string(name));
    if (it == registry_by_name_.end()) return std::nullopt;

    auto process = lookup_process_unsafe(it->second);
    if (!process) return std::nullopt;
    if (!process->alive.load(std::memory_order_acquire)) return std::nullopt;
    return it->second;
}

std::vector<std::string> ActorSystem::registered_names() const {
    std::lock_guard lock(mutex_);

    std::vector<std::string> names;
    names.reserve(registry_by_name_.size());
    for (const auto& [name, _] : registry_by_name_) {
        names.push_back(name);
    }

    std::sort(names.begin(), names.end());
    return names;
}

void ActorSystem::dispatch_pool_runnable(const types::Pid& pid, std::size_t worker_index) {
    std::shared_ptr<ActorProcess> process;
    {
        std::lock_guard lock(mutex_);
        process = lookup_process_unsafe(pid);
        if (!process) return;
        process->run_queue_enqueued.store(false, std::memory_order_relaxed);
        if (!process->managed_by_scheduler) return;
        if (!process->alive.load(std::memory_order_acquire)) {
            process->in_dispatch.store(false, std::memory_order_relaxed);
            return;
        }
        if (process->run_state.load(std::memory_order_relaxed) != RunState::Runnable) {
            return;
        }
        process->in_dispatch.store(true, std::memory_order_release);
    }

    ExitReason reason;
    reason.kind = ExitReason::Kind::Normal;

    if (!bind_current_thread_pid(pid)) {
        reason.kind = ExitReason::Kind::Error;
    } else {
        try {
            process->entry(pid);
        } catch (...) {
            reason.kind = ExitReason::Kind::Error;
        }
    }

    YieldReason yield_reason = YieldReason::None;
    bool alive = false;
    {
        std::lock_guard lock(mutex_);
        auto refreshed = lookup_process_unsafe(pid);
        if (refreshed) {
            refreshed->in_dispatch.store(false, std::memory_order_release);
            alive = refreshed->alive.load(std::memory_order_acquire);
            yield_reason = refreshed->last_yield_reason.load(std::memory_order_relaxed);
        }
    }

    unbind_current_thread_pid();

    if (reason.kind == ExitReason::Kind::Error) {
        complete_actor(pid, reason);
        return;
    }
    if (!alive) return;

    if (yield_reason == YieldReason::BudgetExhausted) {
        std::lock_guard lock(mutex_);
        auto refreshed = lookup_process_unsafe(pid);
        if (!refreshed) return;
        if (!refreshed->alive.load(std::memory_order_acquire)) return;
        refreshed->run_state.store(RunState::Runnable, std::memory_order_relaxed);
        enqueue_runnable_unsafe(refreshed);
        return;
    }

    if (yield_reason == YieldReason::BlockedOnReceive) {
        std::lock_guard lock(mutex_);
        auto refreshed = lookup_process_unsafe(pid);
        if (!refreshed) return;
        if (!refreshed->alive.load(std::memory_order_acquire)) return;
        if (refreshed->run_state.load(std::memory_order_relaxed) == RunState::Runnable) {
            enqueue_runnable_unsafe(refreshed);
            return;
        }
        if (refreshed->mailbox->size() > 0) {
            refreshed->run_state.store(RunState::Runnable, std::memory_order_relaxed);
            enqueue_runnable_unsafe(refreshed);
        } else {
            refreshed->run_state.store(RunState::Waiting, std::memory_order_relaxed);
        }
        return;
    }

    if (yield_reason == YieldReason::Error) {
        reason.kind = ExitReason::Kind::Error;
        complete_actor(pid, reason);
        return;
    }

    complete_actor(pid, reason);
}

void ActorSystem::dispatch_shadow_runnable(const types::Pid& pid) {
    std::lock_guard lock(mutex_);
    auto process = lookup_process_unsafe(pid);
    if (!process) return;
    process->run_queue_enqueued.store(false, std::memory_order_relaxed);
}

void ActorSystem::mark_process_waiting_if_blocking_receive_unsafe(
    const std::shared_ptr<ActorProcess>& process,
    std::optional<std::chrono::milliseconds> timeout) {
    if (!process) return;
    if (!process->alive.load(std::memory_order_acquire)) return;

    const bool potentially_blocking_receive =
        !timeout.has_value() || timeout->count() > 0;
    if (!potentially_blocking_receive) return;
    if (process->mailbox->size() > 0) return;
    process->run_state.store(RunState::Waiting, std::memory_order_relaxed);
}

void ActorSystem::mark_process_running_unsafe(const std::shared_ptr<ActorProcess>& process) {
    if (!process) return;
    if (!process->alive.load(std::memory_order_acquire)) return;
    process->run_state.store(RunState::Running, std::memory_order_relaxed);
}

void ActorSystem::mark_process_exited_unsafe(const std::shared_ptr<ActorProcess>& process) {
    if (!process) return;
    process->run_state.store(RunState::Exited, std::memory_order_relaxed);
}

void ActorSystem::mark_process_runnable_from_message_unsafe(
    const std::shared_ptr<ActorProcess>& process) {
    if (!process) return;
    if (!process->alive.load(std::memory_order_acquire)) return;

    const auto previous = process->run_state.load(std::memory_order_relaxed);
    if (previous != RunState::Waiting) return;

    process->run_state.store(RunState::Runnable, std::memory_order_relaxed);
    if (scheduler_mode_ == SchedulerMode::Pool) {
        if (!process->managed_by_scheduler) return;
        if (process->in_dispatch.load(std::memory_order_acquire)) return;
    }
    enqueue_runnable_unsafe(process);
}

void ActorSystem::enqueue_runnable_unsafe(
    const std::shared_ptr<ActorProcess>& process,
    std::optional<std::size_t> preferred_worker_index) {
    if (!process) return;
    if (scheduler_mode_ != SchedulerMode::Pool && scheduler_mode_ != SchedulerMode::PoolShadow) {
        return;
    }
    if (scheduler_mode_ == SchedulerMode::Pool && !process->managed_by_scheduler) return;
    if (!scheduler_) return;

    if (process->run_queue_enqueued.exchange(true, std::memory_order_acq_rel)) return;

    bool queued = false;
    if (scheduler_mode_ == SchedulerMode::Pool && preferred_worker_index.has_value()) {
        queued = scheduler_->enqueue_for_worker(*preferred_worker_index, process->pid);
    } else {
        queued = scheduler_->enqueue(process->pid);
    }
    if (!queued) {
        process->run_queue_enqueued.store(false, std::memory_order_release);
    }
}

std::size_t ActorSystem::default_scheduler_worker_count() noexcept {
    if (const auto configured = parse_size_t_env("ETA_ACTOR_SCHEDULERS", 0u);
        configured > 0) {
        return configured;
    }
    const auto concurrency = std::thread::hardware_concurrency();
    return concurrency == 0 ? 1u : static_cast<std::size_t>(concurrency);
}

void ActorSystem::add_current_thread_reductions(std::uint64_t units) noexcept {
    if (units == 0) return;
    if (g_thread_actor_binding.system != this || !g_thread_actor_binding.active) {
        return;
    }
    auto* reductions_counter = g_thread_actor_binding.reductions_counter;
    if (!reductions_counter) return;
    reductions_counter->fetch_add(units, std::memory_order_relaxed);
}

void ActorSystem::set_current_thread_last_yield_reason(YieldReason reason) noexcept {
    if (g_thread_actor_binding.system != this || !g_thread_actor_binding.active) {
        return;
    }
    auto* yield_reason = g_thread_actor_binding.yield_reason;
    if (!yield_reason) return;
    yield_reason->store(reason, std::memory_order_relaxed);
}

void ActorSystem::shutdown() {
    if (shutting_down_.exchange(true, std::memory_order_acq_rel)) return;
    if (node_transport_) {
        node_transport_->shutdown();
    }

    std::vector<std::shared_ptr<ActorProcess>> processes;
    {
        std::lock_guard lock(mutex_);
        monitor_refs_.clear();
        remote_watchers_by_target_.clear();
        registry_by_name_.clear();
        registry_by_pid_.clear();
        processes.reserve(processes_.size());
        for (const auto& [_, process] : processes_) {
            process->alive.store(false, std::memory_order_release);
            process->run_state.store(RunState::Exited, std::memory_order_relaxed);
            process->run_queue_enqueued.store(false, std::memory_order_relaxed);
            process->in_dispatch.store(false, std::memory_order_relaxed);
            process->links.clear();
            process->monitors.clear();
            process->registered_name.clear();
            process->mailbox->close();
            processes.push_back(process);
        }
    }

    if (scheduler_) {
        scheduler_->shutdown();
        scheduler_.reset();
    }

    const auto current_thread = std::this_thread::get_id();
    for (const auto& process : processes) {
        if (!process->worker.joinable()) continue;
        if (process->worker.get_id() == current_thread) continue;
        process->worker.join();
    }

    {
        std::lock_guard lock(mutex_);
        processes_.clear();
    }

    unbind_current_thread_pid();
}

std::size_t ActorSystem::PidHasher::operator()(const types::Pid& pid) const noexcept {
    std::size_t seed = std::hash<std::uint64_t>{}(pid.node_id);
    seed = hash_combine(seed, std::hash<std::uint64_t>{}(pid.actor_id));
    seed = hash_combine(seed, std::hash<std::uint32_t>{}(pid.incarnation));
    return seed;
}

types::Pid ActorSystem::allocate_pid_unsafe() {
    types::Pid pid;
    pid.node_id = local_node_id_;
    pid.actor_id = next_actor_id_.fetch_add(1, std::memory_order_relaxed);
    pid.incarnation = 1;
    return pid;
}

std::shared_ptr<ActorSystem::ActorProcess> ActorSystem::lookup_process_unsafe(
    const types::Pid& pid) const {
    auto it = processes_.find(pid);
    if (it == processes_.end()) return nullptr;
    return it->second;
}

bool ActorSystem::enqueue_message_unsafe(
    const types::Pid& pid,
    Message message,
    std::vector<std::pair<std::shared_ptr<Mailbox>, Message>>& outbox) {
    auto process = lookup_process_unsafe(pid);
    if (!process) return false;
    if (!process->alive.load(std::memory_order_acquire)) return false;
    if (scheduler_mode_ == SchedulerMode::Pool) {
        const bool pushed = process->mailbox->push(std::move(message));
        if (!pushed) return false;
        mark_process_runnable_from_message_unsafe(process);
        return true;
    }

    mark_process_runnable_from_message_unsafe(process);
    outbox.emplace_back(process->mailbox, std::move(message));
    return true;
}

void ActorSystem::erase_monitor_unsafe(MonitorRef ref) {
    auto monitor_it = monitor_refs_.find(ref);
    if (monitor_it == monitor_refs_.end()) return;

    const auto watcher = monitor_it->second.watcher;
    auto watcher_process = lookup_process_unsafe(watcher);
    if (watcher_process) {
        watcher_process->monitors.erase(ref);
    }
    monitor_refs_.erase(monitor_it);
}

void ActorSystem::flush_monitor_messages_unsafe(
    const std::shared_ptr<Mailbox>& mailbox,
    MonitorRef ref,
    bool flush_down_message) {
    if (!flush_down_message || !mailbox) return;
    (void)mailbox->erase_if([ref](const Message& message) {
        if (message.monitor_ref != ref) return false;
        return message.kind == Message::Kind::DownSignal
            || message.kind == Message::Kind::NodeUp
            || message.kind == Message::Kind::NodeDown;
    });
}

void ActorSystem::handle_remote_monitor_request(
    const types::Pid& watcher,
    const types::Pid& target,
    MonitorRef ref) {
    bool target_alive = false;
    {
        std::lock_guard lock(mutex_);
        if (target.node_id != local_node_id_) return;
        auto process = lookup_process_unsafe(target);
        target_alive = process && process->alive.load(std::memory_order_acquire);
        if (target_alive) {
            auto& subscribers = remote_watchers_by_target_[target];
            const auto existing = std::find_if(
                subscribers.begin(),
                subscribers.end(),
                [watcher, ref](const RemoteMonitorSubscription& subscription) {
                    return subscription.watcher == watcher && subscription.ref == ref;
                });
            if (existing == subscribers.end()) {
                subscribers.push_back(RemoteMonitorSubscription{
                    .ref = ref,
                    .watcher = watcher,
                    .target = target});
            }
        }
    }

    if (target_alive || !node_transport_) return;

    NodeTransport::RemoteDownSignal signal;
    signal.watcher = watcher;
    signal.target = target;
    signal.monitor_ref = ref;
    signal.reason_kind = NodeTransport::RemoteDownReasonKind::Error;
    (void)node_transport_->send_remote_down(std::move(signal));
}

void ActorSystem::handle_remote_demonitor_request(
    const types::Pid& watcher,
    const types::Pid& target,
    MonitorRef ref) {
    std::lock_guard lock(mutex_);
    auto by_target = remote_watchers_by_target_.find(target);
    if (by_target == remote_watchers_by_target_.end()) return;

    auto& subscribers = by_target->second;
    subscribers.erase(
        std::remove_if(
            subscribers.begin(),
            subscribers.end(),
            [watcher, ref](const RemoteMonitorSubscription& subscription) {
                return subscription.watcher == watcher && subscription.ref == ref;
            }),
        subscribers.end());

    if (subscribers.empty()) {
        remote_watchers_by_target_.erase(by_target);
    }
}

void ActorSystem::handle_remote_down_signal(const NodeTransport::RemoteDownSignal& signal) {
    std::vector<std::pair<std::shared_ptr<Mailbox>, Message>> outbox;
    {
        std::lock_guard lock(mutex_);
        auto monitor_it = monitor_refs_.find(signal.monitor_ref);
        if (monitor_it == monitor_refs_.end()) return;

        const auto subscription = monitor_it->second;
        if (subscription.kind != MonitorSubscription::Kind::RemoteProcess) return;
        if (subscription.watcher != signal.watcher) return;
        if (subscription.target != signal.target) return;

        erase_monitor_unsafe(signal.monitor_ref);
        const auto reason = decode_remote_down_reason(signal);
        (void)enqueue_message_unsafe(
            subscription.watcher,
            Message::make_down(signal.monitor_ref, signal.target, reason),
            outbox);
    }
    apply_outbox_messages(outbox);
}

void ActorSystem::handle_node_up(const NodeTransport::ConnectedNode& node) {
    std::vector<std::pair<std::shared_ptr<Mailbox>, Message>> outbox;
    {
        std::lock_guard lock(mutex_);
        for (const auto& [ref, subscription] : monitor_refs_) {
            if (subscription.kind != MonitorSubscription::Kind::Node) continue;
            if (subscription.node_name != node.node_name) continue;
            (void)enqueue_message_unsafe(
                subscription.watcher,
                Message::make_node_up(ref, node.node_name, node.node_id),
                outbox);
        }
    }
    apply_outbox_messages(outbox);
}

void ActorSystem::handle_node_down(
    const NodeTransport::ConnectedNode& node,
    NodeTransport::NodeDownReason reason) {
    std::vector<std::pair<std::shared_ptr<Mailbox>, Message>> outbox;
    std::vector<MonitorRef> remote_process_refs;
    {
        std::lock_guard lock(mutex_);

        for (auto it = remote_watchers_by_target_.begin(); it != remote_watchers_by_target_.end();) {
            auto& subscribers = it->second;
            subscribers.erase(
                std::remove_if(
                    subscribers.begin(),
                    subscribers.end(),
                    [&node](const RemoteMonitorSubscription& subscription) {
                        return subscription.watcher.node_id == node.node_id;
                    }),
                subscribers.end());
            if (subscribers.empty()) {
                it = remote_watchers_by_target_.erase(it);
            } else {
                ++it;
            }
        }

        for (const auto& [ref, subscription] : monitor_refs_) {
            if (subscription.kind == MonitorSubscription::Kind::Node
                && subscription.node_name == node.node_name) {
                (void)enqueue_message_unsafe(
                    subscription.watcher,
                    Message::make_node_down(ref, node.node_name, map_node_down_reason(reason)),
                    outbox);
            }

            if (subscription.kind == MonitorSubscription::Kind::RemoteProcess
                && subscription.target.node_id == node.node_id) {
                remote_process_refs.push_back(ref);
            }
        }

        for (const auto ref : remote_process_refs) {
            auto monitor_it = monitor_refs_.find(ref);
            if (monitor_it == monitor_refs_.end()) continue;
            const auto subscription = monitor_it->second;
            erase_monitor_unsafe(ref);
            ExitReason down_reason;
            down_reason.kind = ExitReason::Kind::NoConnection;
            (void)enqueue_message_unsafe(
                subscription.watcher,
                Message::make_down(ref, subscription.target, down_reason),
                outbox);
        }
    }
    apply_outbox_messages(outbox);
}

void ActorSystem::apply_remote_down_signals(
    std::vector<NodeTransport::RemoteDownSignal>& remote_down_signals) {
    if (!node_transport_) return;
    for (auto& signal : remote_down_signals) {
        (void)node_transport_->send_remote_down(std::move(signal));
    }
    remote_down_signals.clear();
}

void ActorSystem::apply_remote_demonitors(
    std::vector<RemoteMonitorSubscription>& remote_demonitors) {
    if (!node_transport_) return;
    for (const auto& subscription : remote_demonitors) {
        (void)node_transport_->send_remote_demonitor(
            subscription.watcher,
            subscription.target,
            subscription.ref);
    }
    remote_demonitors.clear();
}

void ActorSystem::terminate_actor_chain_unsafe(
    const types::Pid& initial_pid,
    ExitReason reason,
    std::vector<std::pair<std::shared_ptr<Mailbox>, Message>>& outbox,
    std::vector<NodeTransport::RemoteDownSignal>& remote_down_signals,
    std::vector<RemoteMonitorSubscription>& remote_demonitors) {
    std::deque<std::pair<types::Pid, ExitReason>> pending;
    pending.emplace_back(initial_pid, std::move(reason));

    while (!pending.empty()) {
        auto [pid, current_reason] = std::move(pending.front());
        pending.pop_front();

        auto process = lookup_process_unsafe(pid);
        if (!process) continue;
        if (!process->alive.load(std::memory_order_acquire)) continue;

        process->alive.store(false, std::memory_order_release);
        process->run_state.store(RunState::Exited, std::memory_order_relaxed);
        process->mailbox->close();

        if (!process->registered_name.empty()) {
            registry_by_name_.erase(process->registered_name);
            process->registered_name.clear();
        }
        registry_by_pid_.erase(pid);

        std::vector<types::Pid> linked_pids;
        linked_pids.reserve(process->links.size());
        for (const auto& linked : process->links) {
            linked_pids.push_back(linked);
        }
        process->links.clear();

        for (const auto& linked_pid : linked_pids) {
            auto linked_process = lookup_process_unsafe(linked_pid);
            if (!linked_process) continue;
            linked_process->links.erase(pid);
        }

        std::vector<MonitorRef> owned_refs;
        owned_refs.reserve(process->monitors.size());
        for (const auto ref : process->monitors) {
            owned_refs.push_back(ref);
        }
        process->monitors.clear();
        for (const auto ref : owned_refs) {
            auto monitor_it = monitor_refs_.find(ref);
            if (monitor_it != monitor_refs_.end()) {
                if (monitor_it->second.kind == MonitorSubscription::Kind::RemoteProcess) {
                    remote_demonitors.push_back(RemoteMonitorSubscription{
                        .ref = ref,
                        .watcher = monitor_it->second.watcher,
                        .target = monitor_it->second.target});
                }
            }
            monitor_refs_.erase(ref);
        }

        std::vector<MonitorSubscription> down_watchers;
        std::vector<MonitorRef> down_refs;
        for (const auto& [ref, subscription] : monitor_refs_) {
            if (subscription.kind != MonitorSubscription::Kind::LocalProcess) continue;
            if (subscription.target != pid) continue;
            down_watchers.push_back(subscription);
            down_refs.push_back(ref);
        }

        for (const auto ref : down_refs) {
            erase_monitor_unsafe(ref);
        }

        const auto down_reason = normalize_down_reason(current_reason);
        for (const auto& watcher : down_watchers) {
            (void)enqueue_message_unsafe(
                watcher.watcher,
                Message::make_down(watcher.ref, pid, down_reason),
                outbox);
        }

        auto remote_it = remote_watchers_by_target_.find(pid);
        if (remote_it != remote_watchers_by_target_.end()) {
            for (const auto& watcher : remote_it->second) {
                NodeTransport::RemoteDownSignal signal;
                signal.watcher = watcher.watcher;
                signal.target = pid;
                signal.monitor_ref = watcher.ref;
                signal.reason_kind = encode_remote_down_reason(down_reason);
                if (down_reason.kind == ExitReason::Kind::Custom) {
                    signal.reason_payload = down_reason.payload;
                }
                remote_down_signals.push_back(std::move(signal));
            }
            remote_watchers_by_target_.erase(remote_it);
        }

        if (is_normal_exit(current_reason)) continue;

        for (const auto& linked_pid : linked_pids) {
            auto linked_process = lookup_process_unsafe(linked_pid);
            if (!linked_process) continue;
            if (!linked_process->alive.load(std::memory_order_acquire)) continue;

            if (linked_process->trap_exit) {
                (void)enqueue_message_unsafe(
                    linked_pid,
                    Message::make_exit(pid, current_reason),
                    outbox);
            } else {
                pending.emplace_back(linked_pid, current_reason);
            }
        }
    }
}

bool ActorSystem::is_normal_exit(const ExitReason& reason) {
    return reason.kind == ExitReason::Kind::Normal;
}

ActorSystem::ExitReason ActorSystem::normalize_down_reason(const ExitReason& reason) {
    if (reason.kind != ExitReason::Kind::Custom) return reason;
    return reason;
}

NodeTransport::RemoteDownReasonKind ActorSystem::encode_remote_down_reason(
    const ExitReason& reason) {
    switch (reason.kind) {
        case ExitReason::Kind::Normal:
            return NodeTransport::RemoteDownReasonKind::Normal;
        case ExitReason::Kind::Shutdown:
            return NodeTransport::RemoteDownReasonKind::Shutdown;
        case ExitReason::Kind::Killed:
            return NodeTransport::RemoteDownReasonKind::Killed;
        case ExitReason::Kind::Error:
            return NodeTransport::RemoteDownReasonKind::Error;
        case ExitReason::Kind::NoConnection:
            return NodeTransport::RemoteDownReasonKind::NoConnection;
        case ExitReason::Kind::BadCookie:
            return NodeTransport::RemoteDownReasonKind::BadCookie;
        case ExitReason::Kind::Custom:
            return NodeTransport::RemoteDownReasonKind::Custom;
    }
    return NodeTransport::RemoteDownReasonKind::Error;
}

ActorSystem::ExitReason ActorSystem::decode_remote_down_reason(
    const NodeTransport::RemoteDownSignal& signal) {
    ExitReason reason;
    switch (signal.reason_kind) {
        case NodeTransport::RemoteDownReasonKind::Normal:
            reason.kind = ExitReason::Kind::Normal;
            break;
        case NodeTransport::RemoteDownReasonKind::Shutdown:
            reason.kind = ExitReason::Kind::Shutdown;
            break;
        case NodeTransport::RemoteDownReasonKind::Killed:
            reason.kind = ExitReason::Kind::Killed;
            break;
        case NodeTransport::RemoteDownReasonKind::Error:
            reason.kind = ExitReason::Kind::Error;
            break;
        case NodeTransport::RemoteDownReasonKind::NoConnection:
            reason.kind = ExitReason::Kind::NoConnection;
            break;
        case NodeTransport::RemoteDownReasonKind::BadCookie:
            reason.kind = ExitReason::Kind::BadCookie;
            break;
        case NodeTransport::RemoteDownReasonKind::Custom:
            reason.kind = ExitReason::Kind::Custom;
            reason.payload = signal.reason_payload;
            break;
    }
    return reason;
}

ActorSystem::ExitReason ActorSystem::map_node_down_reason(
    NodeTransport::NodeDownReason reason_kind) {
    ExitReason reason;
    switch (reason_kind) {
        case NodeTransport::NodeDownReason::Disconnected:
            reason.kind = ExitReason::Kind::NoConnection;
            break;
        case NodeTransport::NodeDownReason::BadCookie:
            reason.kind = ExitReason::Kind::BadCookie;
            break;
        case NodeTransport::NodeDownReason::Incompatible:
            reason.kind = ExitReason::Kind::Error;
            break;
    }
    return reason;
}

std::uint64_t ActorSystem::allocate_local_node_id() {
    auto node_id = g_next_node_id.fetch_add(1, std::memory_order_relaxed);
    if (node_id == 0) {
        node_id = g_next_node_id.fetch_add(1, std::memory_order_relaxed);
    }
    return node_id;
}

} // namespace eta::runtime::actor
