#include "eta/runtime/actor/actor_system.h"

#include <algorithm>
#include <deque>
#include <utility>

#include "eta/runtime/actor/node_transport.h"

namespace eta::runtime::actor {

namespace {

struct ThreadActorBinding {
    const ActorSystem* system{nullptr};
    types::Pid pid{};
    bool active{false};
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

} // namespace

ActorSystem::ActorSystem()
    : local_node_id_(allocate_local_node_id()) {
    node_name_ = "eta@node-" + std::to_string(local_node_id_);
    node_transport_ = std::make_unique<NodeTransport>(
        local_node_id_,
        [this](const types::Pid& pid, BinaryMessage payload) {
            return deliver_remote_payload(pid, std::move(payload));
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
    {
        std::lock_guard lock(mutex_);
        if (shutting_down_.load(std::memory_order_acquire)) {
            return std::unexpected("actor system is shutting down");
        }
        process->pid = allocate_pid_unsafe();
        processes_.emplace(process->pid, process);
    }

    try {
        process->worker = std::thread(
            [this, process, entry = std::move(entry)]() mutable {
                const auto pid = process->pid;
                ExitReason reason;
                reason.kind = ExitReason::Kind::Normal;

                if (!bind_current_thread_pid(pid)) {
                    reason.kind = ExitReason::Kind::Error;
                    complete_actor(pid, reason);
                    return;
                }

                try {
                    entry(pid);
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
    return true;
}

void ActorSystem::unbind_current_thread_pid() {
    if (g_thread_actor_binding.system != this) return;
    g_thread_actor_binding.system = nullptr;
    g_thread_actor_binding.pid = {};
    g_thread_actor_binding.active = false;
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
                outbox.emplace_back(
                    process->mailbox,
                    Message::make_payload(std::move(message)));
                status = SendStatus::Delivered;
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

        watcher_process->monitors[ref] = target;
        monitor_refs_[ref] = MonitorSubscription{.ref = ref, .watcher = watcher, .target = target};

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
    }

    apply_outbox_messages(outbox);
    return ref;
}

bool ActorSystem::demonitor(
    const types::Pid& watcher,
    MonitorRef ref,
    bool flush_down_message) {
    std::shared_ptr<Mailbox> watcher_mailbox;
    bool removed = false;

    {
        std::lock_guard lock(mutex_);
        auto watcher_process = lookup_process_unsafe(watcher);
        if (!watcher_process) return false;
        if (!watcher_process->alive.load(std::memory_order_acquire)) return false;

        watcher_mailbox = watcher_process->mailbox;

        auto owned = watcher_process->monitors.find(ref);
        if (owned != watcher_process->monitors.end()) {
            erase_monitor_unsafe(ref);
            removed = true;
        }
    }

    if (flush_down_message && watcher_mailbox) {
        (void)watcher_mailbox->erase_if([ref](const Message& message) {
            return message.kind == Message::Kind::DownSignal && message.monitor_ref == ref;
        });
    }

    return removed;
}

bool ActorSystem::signal_exit(
    const std::optional<types::Pid>& from,
    const types::Pid& target,
    ExitReason reason,
    bool untrappable) {
    std::vector<std::pair<std::shared_ptr<Mailbox>, Message>> outbox;
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
            terminate_actor_chain_unsafe(target, std::move(reason), outbox);
            handled = true;
        }
    }

    apply_outbox_messages(outbox);
    return handled;
}

void ActorSystem::complete_actor(const types::Pid& pid, ExitReason reason) {
    std::vector<std::pair<std::shared_ptr<Mailbox>, Message>> outbox;
    {
        std::lock_guard lock(mutex_);
        terminate_actor_chain_unsafe(pid, std::move(reason), outbox);
    }
    apply_outbox_messages(outbox);
}

std::optional<ActorSystem::Message> ActorSystem::receive(
    const types::Pid& pid,
    std::optional<std::chrono::milliseconds> timeout) {
    std::shared_ptr<Mailbox> mailbox;
    {
        std::lock_guard lock(mutex_);
        auto process = lookup_process_unsafe(pid);
        if (!process) return std::nullopt;
        mailbox = process->mailbox;
    }
    return mailbox->pop(timeout);
}

std::expected<std::optional<ActorSystem::Message>, error::RuntimeError>
ActorSystem::receive_matching(
    const types::Pid& pid,
    std::optional<std::chrono::milliseconds> timeout,
    const MessageMatcher& matcher) {
    std::shared_ptr<Mailbox> mailbox;
    {
        std::lock_guard lock(mutex_);
        auto process = lookup_process_unsafe(pid);
        if (!process) return std::optional<Message>{};
        mailbox = process->mailbox;
    }
    return mailbox->pop_matching(timeout, matcher);
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

void ActorSystem::shutdown() {
    if (shutting_down_.exchange(true, std::memory_order_acq_rel)) return;
    if (node_transport_) {
        node_transport_->shutdown();
    }

    std::vector<std::shared_ptr<ActorProcess>> processes;
    {
        std::lock_guard lock(mutex_);
        monitor_refs_.clear();
        registry_by_name_.clear();
        registry_by_pid_.clear();
        processes.reserve(processes_.size());
        for (const auto& [_, process] : processes_) {
            process->alive.store(false, std::memory_order_release);
            process->links.clear();
            process->monitors.clear();
            process->registered_name.clear();
            process->mailbox->close();
            processes.push_back(process);
        }
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
    std::vector<std::pair<std::shared_ptr<Mailbox>, Message>>& outbox) const {
    auto process = lookup_process_unsafe(pid);
    if (!process) return false;
    if (!process->alive.load(std::memory_order_acquire)) return false;
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

void ActorSystem::terminate_actor_chain_unsafe(
    const types::Pid& initial_pid,
    ExitReason reason,
    std::vector<std::pair<std::shared_ptr<Mailbox>, Message>>& outbox) {
    std::deque<std::pair<types::Pid, ExitReason>> pending;
    pending.emplace_back(initial_pid, std::move(reason));

    while (!pending.empty()) {
        auto [pid, current_reason] = std::move(pending.front());
        pending.pop_front();

        auto process = lookup_process_unsafe(pid);
        if (!process) continue;
        if (!process->alive.load(std::memory_order_acquire)) continue;

        process->alive.store(false, std::memory_order_release);
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
        for (const auto& [ref, _] : process->monitors) {
            owned_refs.push_back(ref);
        }
        process->monitors.clear();
        for (const auto ref : owned_refs) {
            monitor_refs_.erase(ref);
        }

        std::vector<MonitorSubscription> down_watchers;
        std::vector<MonitorRef> down_refs;
        for (const auto& [ref, subscription] : monitor_refs_) {
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

std::uint64_t ActorSystem::allocate_local_node_id() {
    auto node_id = g_next_node_id.fetch_add(1, std::memory_order_relaxed);
    if (node_id == 0) {
        node_id = g_next_node_id.fetch_add(1, std::memory_order_relaxed);
    }
    return node_id;
}

} // namespace eta::runtime::actor
