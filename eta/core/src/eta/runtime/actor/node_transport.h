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
    using DeliverCallback = std::function<bool(const types::Pid&, BinaryMessage)>;

    struct ConnectedNode {
        std::uint64_t node_id{0};
        std::string node_name{};
        std::string endpoint{};
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
        DeliverCallback deliver_message);
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

    void shutdown();

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace eta::runtime::actor
