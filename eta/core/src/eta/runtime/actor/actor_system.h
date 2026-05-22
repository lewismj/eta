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
#include "eta/runtime/actor/node_transport.h"
#include "eta/runtime/types/pid.h"

namespace eta::runtime::actor {

class Scheduler;

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

    enum class YieldReason : std::uint8_t {
        None,
        BudgetExhausted,
        BlockedOnReceive,
        Finished,
        Error,
    };

    enum class SchedulerMode : std::uint8_t {
        ThreadPerActor,
        Pool,
        PoolShadow,
    };

    enum class RunState : std::uint8_t {
        Runnable,
        Running,
        Waiting,
        Exited,
    };

    struct ConnectedNode {
        std::uint64_t node_id{0};
        std::string node_name{};
        std::string endpoint{};
    };

    struct ProcessInfo {
        types::Pid pid{};
        bool alive{false};
        std::size_t mailbox_length{0};
        std::string registered_name{};
        std::vector<types::Pid> links{};
        std::vector<MonitorRef> monitors{};
        std::uint64_t reductions{0};
        RunState run_state{RunState::Runnable};
        YieldReason last_yield_reason{YieldReason::None};
    };

    struct SchedulerStats {
        SchedulerMode mode{SchedulerMode::ThreadPerActor};
        std::uint64_t runnable_queue_depth{0};
        std::uint64_t scheduler_wakeups{0};
        std::uint64_t dirty_queue_depth{0};
        std::uint64_t enqueued{0};
        std::uint64_t dequeued{0};
        std::uint64_t steals{0};
        std::uint64_t global_queue_depth{0};
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
     * @brief Register one node lifecycle monitor for @p node_name.
     */
    [[nodiscard]] std::optional<MonitorRef> monitor_node(
        const types::Pid& watcher,
        std::string node_name);

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
    [[nodiscard]] std::optional<ProcessInfo> process_info(const types::Pid& pid) const;
    void set_scheduler_mode(SchedulerMode mode);
    [[nodiscard]] SchedulerMode scheduler_mode() const;
    [[nodiscard]] SchedulerStats scheduler_stats() const;
    /**
     * @brief Enqueue one blocking task on the dirty scheduler queue.
     */
    [[nodiscard]] bool enqueue_dirty_task(std::function<void()> task);
    /**
     * @brief Mark one actor runnable due to an external completion event.
     */
    void notify_external_runnable(const types::Pid& pid);
    [[nodiscard]] bool pid_exists(const types::Pid& pid) const;
    [[nodiscard]] bool register_name(std::string name, const types::Pid& pid);
    [[nodiscard]] bool unregister_name(std::string_view name);
    [[nodiscard]] std::optional<types::Pid> whereis(std::string_view name) const;
    [[nodiscard]] std::vector<std::string> registered_names() const;

    /**
     * @brief Add reduction units to the actor currently bound to this thread.
     */
    void add_current_thread_reductions(std::uint64_t units) noexcept;

    /**
     * @brief Update last yield reason for the actor bound to this thread.
     */
    void set_current_thread_last_yield_reason(YieldReason reason) noexcept;

    /**
     * @brief Stop all mailboxes and join spawned worker threads.
     */
    void shutdown();

private:
    struct PidHasher {
        [[nodiscard]] std::size_t operator()(const types::Pid& pid) const noexcept;
    };

    struct MonitorSubscription {
        enum class Kind : std::uint8_t {
            LocalProcess,
            RemoteProcess,
            Node,
        };

        MonitorRef ref{0};
        types::Pid watcher{};
        types::Pid target{};
        Kind kind{Kind::LocalProcess};
        std::string node_name{};
    };

    struct RemoteMonitorSubscription {
        MonitorRef ref{0};
        types::Pid watcher{};
        types::Pid target{};
    };

    struct ActorProcess {
        types::Pid pid{};
        std::shared_ptr<Mailbox> mailbox{std::make_shared<Mailbox>()};
        std::thread worker{};
        ActorEntry entry{};
        bool managed_by_scheduler{false};
        std::atomic<bool> alive{true};
        std::atomic<bool> run_queue_enqueued{false};
        std::atomic<bool> in_dispatch{false};
        std::string registered_name{};
        bool is_main_thread_actor{false};
        bool trap_exit{false};
        std::unordered_set<types::Pid, PidHasher> links{};
        std::unordered_set<MonitorRef> monitors{};
        std::atomic<std::uint64_t> reductions{0};
        std::atomic<RunState> run_state{RunState::Runnable};
        std::atomic<YieldReason> last_yield_reason{YieldReason::None};
    };

    [[nodiscard]] types::Pid allocate_pid_unsafe();
    [[nodiscard]] std::shared_ptr<ActorProcess> lookup_process_unsafe(const types::Pid& pid) const;
    [[nodiscard]] bool enqueue_message_unsafe(
        const types::Pid& pid,
        Message message,
        std::vector<std::pair<std::shared_ptr<Mailbox>, Message>>& outbox);
    [[nodiscard]] bool deliver_remote_payload(const types::Pid& pid, BinaryMessage payload);
    [[nodiscard]] static std::uint64_t allocate_local_node_id();
    void erase_monitor_unsafe(MonitorRef ref);
    void flush_monitor_messages_unsafe(
        const std::shared_ptr<Mailbox>& mailbox,
        MonitorRef ref,
        bool flush_down_message);
    void handle_remote_monitor_request(
        const types::Pid& watcher,
        const types::Pid& target,
        MonitorRef ref);
    void handle_remote_demonitor_request(
        const types::Pid& watcher,
        const types::Pid& target,
        MonitorRef ref);
    void handle_remote_down_signal(const NodeTransport::RemoteDownSignal& signal);
    void handle_node_up(const NodeTransport::ConnectedNode& node);
    void handle_node_down(
        const NodeTransport::ConnectedNode& node,
        NodeTransport::NodeDownReason reason);
    void apply_remote_down_signals(
        std::vector<NodeTransport::RemoteDownSignal>& remote_down_signals);
    void apply_remote_demonitors(
        std::vector<RemoteMonitorSubscription>& remote_demonitors);
    void terminate_actor_chain_unsafe(
        const types::Pid& initial_pid,
        ExitReason reason,
        std::vector<std::pair<std::shared_ptr<Mailbox>, Message>>& outbox,
        std::vector<NodeTransport::RemoteDownSignal>& remote_down_signals,
        std::vector<RemoteMonitorSubscription>& remote_demonitors);
    static bool is_normal_exit(const ExitReason& reason);
    static ExitReason normalize_down_reason(const ExitReason& reason);
    static NodeTransport::RemoteDownReasonKind encode_remote_down_reason(
        const ExitReason& reason);
    static ExitReason decode_remote_down_reason(
        const NodeTransport::RemoteDownSignal& signal);
    static ExitReason map_node_down_reason(NodeTransport::NodeDownReason reason);
    void dispatch_pool_runnable(const types::Pid& pid, std::size_t worker_index);
    void dispatch_shadow_runnable(const types::Pid& pid);
    void mark_process_waiting_if_blocking_receive_unsafe(
        const std::shared_ptr<ActorProcess>& process,
        std::optional<std::chrono::milliseconds> timeout);
    void mark_process_running_unsafe(const std::shared_ptr<ActorProcess>& process);
    void mark_process_exited_unsafe(const std::shared_ptr<ActorProcess>& process);
    void mark_process_runnable_from_message_unsafe(const std::shared_ptr<ActorProcess>& process);
    void enqueue_runnable_unsafe(
        const std::shared_ptr<ActorProcess>& process,
        std::optional<std::size_t> preferred_worker_index = std::nullopt);
    [[nodiscard]] static std::size_t default_scheduler_worker_count() noexcept;

    mutable std::mutex mutex_{};
    std::unordered_map<types::Pid, std::shared_ptr<ActorProcess>, PidHasher> processes_{};
    std::unordered_map<MonitorRef, MonitorSubscription> monitor_refs_{};
    std::unordered_map<types::Pid, std::vector<RemoteMonitorSubscription>, PidHasher>
        remote_watchers_by_target_{};
    std::unordered_map<std::string, types::Pid> registry_by_name_{};
    std::unordered_map<types::Pid, std::string, PidHasher> registry_by_pid_{};
    std::uint64_t local_node_id_{0};
    std::string node_name_{"nonode@local"};
    std::string node_cookie_{};
    std::unique_ptr<NodeTransport> node_transport_{};
    std::atomic<std::uint64_t> next_actor_id_{1};
    std::atomic<MonitorRef> next_monitor_ref_{1};
    std::atomic<bool> shutting_down_{false};
    SchedulerMode scheduler_mode_{SchedulerMode::ThreadPerActor};
    std::unique_ptr<Scheduler> scheduler_{};
};

} // namespace eta::runtime::actor
