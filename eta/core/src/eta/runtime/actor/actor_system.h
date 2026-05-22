#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string_view>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "eta/runtime/actor/mailbox.h"
#include "eta/runtime/types/pid.h"

namespace eta::runtime::actor {

class NodeTransport;

/**
 * @brief Runtime owner for local actors and their mailboxes.
 */
class ActorSystem {
public:
    using BinaryMessage = Mailbox::BinaryMessage;
    using Message = Mailbox::Message;
    using MessageMatcher = Mailbox::Matcher;
    using ExitReason = Mailbox::ExitReason;
    using MonitorRef = std::uint64_t;
    using ActorEntry = std::function<void(const types::Pid&)>;

    enum class SendStatus : std::uint8_t {
        Delivered,
        NoSuchPid,
        DeadPid,
        NoRoute,
        TransportError,
    };

    struct ConnectedNode {
        std::uint64_t node_id{0};
        std::string node_name{};
        std::string endpoint{};
    };

    ActorSystem();
    ~ActorSystem();

    ActorSystem(const ActorSystem&) = delete;
    ActorSystem& operator=(const ActorSystem&) = delete;
    ActorSystem(ActorSystem&&) = delete;
    ActorSystem& operator=(ActorSystem&&) = delete;

    /**
     * @brief Register the current thread as an actor.
     */
    [[nodiscard]] std::expected<types::Pid, std::string> register_current_thread_actor();

    /**
     * @brief Spawn one actor thread with a fresh PID.
     */
    [[nodiscard]] std::expected<types::Pid, std::string> spawn(ActorEntry entry);

    /**
     * @brief Bind/unbind actor identity for the current thread.
     */
    [[nodiscard]] bool bind_current_thread_pid(const types::Pid& pid);
    void unbind_current_thread_pid();

    /**
     * @brief Current thread PID for this actor system.
     */
    [[nodiscard]] std::optional<types::Pid> current_pid() const;

    /**
     * @brief Send a binary payload to one actor mailbox.
     */
    [[nodiscard]] bool send(const types::Pid& pid, BinaryMessage message);

    /**
     * @brief Send one payload and return delivery status.
     */
    [[nodiscard]] SendStatus send_checked(const types::Pid& pid, BinaryMessage message);

    /**
     * @brief Return local node name used for distributed transport.
     */
    [[nodiscard]] std::string node_name() const;

    /**
     * @brief Return local node id used in PIDs.
     */
    [[nodiscard]] std::uint64_t local_node_id() const noexcept;

    /**
     * @brief Configure local node identity and cookie for transport handshakes.
     */
    [[nodiscard]] bool configure_node(
        std::string node_name,
        std::string cookie,
        std::string* error_message = nullptr);

    /**
     * @brief Start listening for one remote node on @p endpoint.
     */
    [[nodiscard]] bool node_listen(
        std::string endpoint,
        std::string* error_message = nullptr);

    /**
     * @brief Connect to one remote node endpoint.
     */
    [[nodiscard]] bool node_connect(
        std::string endpoint,
        std::string* error_message = nullptr);

    /**
     * @brief Disconnect one node by name.
     */
    [[nodiscard]] bool disconnect_node(std::string_view node_name);

    /**
     * @brief Snapshot connected node metadata.
     */
    [[nodiscard]] std::vector<ConnectedNode> connected_nodes() const;

    /**
     * @brief Set trap-exit mode for one actor.
     */
    [[nodiscard]] bool set_trap_exit(const types::Pid& pid, bool enabled);

    /**
     * @brief Link two actors bidirectionally.
     */
    [[nodiscard]] bool link(const types::Pid& lhs, const types::Pid& rhs);

    /**
     * @brief Remove a bidirectional link.
     */
    [[nodiscard]] bool unlink(const types::Pid& lhs, const types::Pid& rhs);

    /**
     * @brief Register one monitor from @p watcher to @p target.
     */
    [[nodiscard]] std::optional<MonitorRef> monitor(
        const types::Pid& watcher,
        const types::Pid& target);

    /**
     * @brief Remove a monitor reference owned by @p watcher.
     */
    [[nodiscard]] bool demonitor(
        const types::Pid& watcher,
        MonitorRef ref,
        bool flush_down_message);

    /**
     * @brief Deliver one exit signal to @p target.
     */
    [[nodiscard]] bool signal_exit(
        const std::optional<types::Pid>& from,
        const types::Pid& target,
        ExitReason reason,
        bool untrappable);

    /**
     * @brief Mark one actor as terminated and run link/monitor propagation.
     */
    void complete_actor(const types::Pid& pid, ExitReason reason);

    /**
     * @brief Receive from one mailbox (FIFO head).
     */
    [[nodiscard]] std::optional<Message> receive(
        const types::Pid& pid,
        std::optional<std::chrono::milliseconds> timeout);

    /**
     * @brief Receive the first message that satisfies @p matcher.
     */
    [[nodiscard]] std::expected<std::optional<Message>, error::RuntimeError> receive_matching(
        const types::Pid& pid,
        std::optional<std::chrono::milliseconds> timeout,
        const MessageMatcher& matcher);

    [[nodiscard]] std::optional<std::size_t> mailbox_size(const types::Pid& pid) const;
    [[nodiscard]] bool pid_exists(const types::Pid& pid) const;
    [[nodiscard]] bool register_name(std::string name, const types::Pid& pid);
    [[nodiscard]] bool unregister_name(std::string_view name);
    [[nodiscard]] std::optional<types::Pid> whereis(std::string_view name) const;
    [[nodiscard]] std::vector<std::string> registered_names() const;

    /**
     * @brief Stop all mailboxes and join spawned worker threads.
     */
    void shutdown();

private:
    struct PidHasher {
        [[nodiscard]] std::size_t operator()(const types::Pid& pid) const noexcept;
    };

    struct MonitorSubscription {
        MonitorRef ref{0};
        types::Pid watcher{};
        types::Pid target{};
    };

    struct ActorProcess {
        types::Pid pid{};
        std::shared_ptr<Mailbox> mailbox{std::make_shared<Mailbox>()};
        std::thread worker{};
        std::atomic<bool> alive{true};
        std::string registered_name{};
        bool is_main_thread_actor{false};
        bool trap_exit{false};
        std::unordered_set<types::Pid, PidHasher> links{};
        std::unordered_map<MonitorRef, types::Pid> monitors{};
    };

    [[nodiscard]] types::Pid allocate_pid_unsafe();
    [[nodiscard]] std::shared_ptr<ActorProcess> lookup_process_unsafe(const types::Pid& pid) const;
    [[nodiscard]] bool enqueue_message_unsafe(
        const types::Pid& pid,
        Message message,
        std::vector<std::pair<std::shared_ptr<Mailbox>, Message>>& outbox) const;
    [[nodiscard]] bool deliver_remote_payload(const types::Pid& pid, BinaryMessage payload);
    [[nodiscard]] static std::uint64_t allocate_local_node_id();
    void erase_monitor_unsafe(MonitorRef ref);
    void terminate_actor_chain_unsafe(
        const types::Pid& initial_pid,
        ExitReason reason,
        std::vector<std::pair<std::shared_ptr<Mailbox>, Message>>& outbox);
    static bool is_normal_exit(const ExitReason& reason);
    static ExitReason normalize_down_reason(const ExitReason& reason);

    mutable std::mutex mutex_{};
    std::unordered_map<types::Pid, std::shared_ptr<ActorProcess>, PidHasher> processes_{};
    std::unordered_map<MonitorRef, MonitorSubscription> monitor_refs_{};
    std::unordered_map<std::string, types::Pid> registry_by_name_{};
    std::unordered_map<types::Pid, std::string, PidHasher> registry_by_pid_{};
    std::uint64_t local_node_id_{0};
    std::string node_name_{"nonode@local"};
    std::string node_cookie_{};
    std::unique_ptr<NodeTransport> node_transport_{};
    std::atomic<std::uint64_t> next_actor_id_{1};
    std::atomic<MonitorRef> next_monitor_ref_{1};
    std::atomic<bool> shutting_down_{false};
};

} // namespace eta::runtime::actor
