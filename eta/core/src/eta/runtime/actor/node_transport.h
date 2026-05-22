#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "eta/runtime/types/pid.h"

namespace eta::runtime::actor {

/**
 * @brief Node-to-node transport bridge for distributed actor messages.
 */
class NodeTransport {
public:
    using BinaryMessage = std::vector<std::uint8_t>;
    using MonitorRef = std::uint64_t;

    struct ConnectedNode {
        std::uint64_t node_id{0};
        std::string node_name{};
        std::string endpoint{};
    };

    enum class NodeDownReason : std::uint8_t {
        Disconnected,
        BadCookie,
        Incompatible,
    };

    enum class RemoteDownReasonKind : std::uint8_t {
        Normal,
        Shutdown,
        Killed,
        Error,
        NoConnection,
        BadCookie,
        Custom,
    };

    struct RemoteDownSignal {
        types::Pid watcher{};
        types::Pid target{};
        MonitorRef monitor_ref{0};
        RemoteDownReasonKind reason_kind{RemoteDownReasonKind::Error};
        BinaryMessage reason_payload{};
    };

    using DeliverCallback = std::function<bool(const types::Pid&, BinaryMessage)>;
    using RemoteMonitorCallback =
        std::function<void(const types::Pid&, const types::Pid&, MonitorRef)>;
    using RemoteDemonitorCallback =
        std::function<void(const types::Pid&, const types::Pid&, MonitorRef)>;
    using RemoteDownCallback = std::function<void(const RemoteDownSignal&)>;
    using NodeUpCallback = std::function<void(const ConnectedNode&)>;
    using NodeDownCallback = std::function<void(const ConnectedNode&, NodeDownReason)>;

    struct Callbacks {
        DeliverCallback deliver_message{};
        RemoteMonitorCallback remote_monitor{};
        RemoteDemonitorCallback remote_demonitor{};
        RemoteDownCallback remote_down{};
        NodeUpCallback node_up{};
        NodeDownCallback node_down{};
    };

    enum class SendResultCode : std::uint8_t {
        Delivered,
        NoRoute,
        TransportError,
    };

    struct SendResult {
        SendResultCode code{SendResultCode::TransportError};
        std::string detail{};
    };

    NodeTransport(
        std::uint64_t local_node_id,
        Callbacks callbacks);
    ~NodeTransport();

    NodeTransport(const NodeTransport&) = delete;
    NodeTransport& operator=(const NodeTransport&) = delete;
    NodeTransport(NodeTransport&&) = delete;
    NodeTransport& operator=(NodeTransport&&) = delete;

    [[nodiscard]] bool configure(
        std::string node_name,
        std::string cookie,
        std::string* error_message = nullptr);

    [[nodiscard]] std::string node_name() const;
    [[nodiscard]] std::uint64_t local_node_id() const noexcept;

    [[nodiscard]] bool listen(
        std::string endpoint,
        std::string* error_message = nullptr);

    [[nodiscard]] bool connect(
        std::string endpoint,
        std::string* error_message = nullptr);

    [[nodiscard]] bool disconnect_node(std::string_view node_name);
    [[nodiscard]] std::vector<ConnectedNode> connected_nodes() const;

    [[nodiscard]] SendResult send_remote(
        const types::Pid& from,
        const types::Pid& to,
        BinaryMessage payload);
    [[nodiscard]] SendResult send_remote_monitor(
        const types::Pid& watcher,
        const types::Pid& target,
        MonitorRef monitor_ref);
    [[nodiscard]] SendResult send_remote_demonitor(
        const types::Pid& watcher,
        const types::Pid& target,
        MonitorRef monitor_ref);
    [[nodiscard]] SendResult send_remote_down(RemoteDownSignal signal);

    void shutdown();

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace eta::runtime::actor
