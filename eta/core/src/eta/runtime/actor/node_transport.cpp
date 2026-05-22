#include "eta/runtime/actor/node_transport.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstring>
#include <expected>
#include <memory>
#include <mutex>
#include <span>
#include <thread>
#include <unordered_map>
#include <utility>

#include <nng/nng.h>
#include <nng/protocol/pair0/pair.h>

namespace eta::runtime::actor {

namespace {

constexpr std::uint8_t kEnvelopeVersion = 1;
constexpr std::uint32_t kFeatureRemoteSend = 1u << 0;
constexpr std::uint32_t kFeatureRemoteMonitor = 1u << 1;
constexpr std::uint32_t kHandshakeFeatureFlags = kFeatureRemoteSend | kFeatureRemoteMonitor;
constexpr int kSocketReceiveTimeoutMs = 100;
constexpr auto kHandshakeTimeout = std::chrono::seconds(3);

enum class FrameType : std::uint8_t {
    Hello = 1,
    HelloAck = 2,
    ActorMessage = 3,
    MonitorRequest = 4,
    DemonitorRequest = 5,
    DownSignal = 6,
};

enum class HandshakeStatus : std::uint8_t {
    Ok = 0,
    BadCookie = 1,
    Incompatible = 2,
};

struct HelloPayload {
    std::uint32_t feature_flags{0};
    std::uint64_t node_id{0};
    std::string node_name{};
    std::string cookie{};
};

struct HelloAckPayload {
    std::uint32_t feature_flags{0};
    HandshakeStatus status{HandshakeStatus::Incompatible};
    std::uint64_t node_id{0};
    std::string node_name{};
};

struct ActorEnvelope {
    std::uint32_t feature_flags{0};
    types::Pid from{};
    types::Pid to{};
    std::vector<std::uint8_t> payload{};
};

struct MonitorControlEnvelope {
    std::uint32_t feature_flags{0};
    std::uint64_t monitor_ref{0};
    types::Pid watcher{};
    types::Pid target{};
};

struct DownEnvelope {
    std::uint32_t feature_flags{0};
    std::uint64_t monitor_ref{0};
    types::Pid watcher{};
    types::Pid target{};
    NodeTransport::RemoteDownReasonKind reason_kind{
        NodeTransport::RemoteDownReasonKind::Error};
    std::vector<std::uint8_t> reason_payload{};
};

void write_u8(std::vector<std::uint8_t>& out, std::uint8_t value) {
    out.push_back(value);
}

void write_u32(std::vector<std::uint8_t>& out, std::uint32_t value) {
    out.push_back(static_cast<std::uint8_t>(value & 0xffu));
    out.push_back(static_cast<std::uint8_t>((value >> 8u) & 0xffu));
    out.push_back(static_cast<std::uint8_t>((value >> 16u) & 0xffu));
    out.push_back(static_cast<std::uint8_t>((value >> 24u) & 0xffu));
}

void write_u64(std::vector<std::uint8_t>& out, std::uint64_t value) {
    for (int i = 0; i < 8; ++i) {
        out.push_back(static_cast<std::uint8_t>(value & 0xffu));
        value >>= 8u;
    }
}

bool read_u8(std::span<const std::uint8_t> data, std::size_t& cursor, std::uint8_t& out) {
    if (cursor >= data.size()) return false;
    out = data[cursor++];
    return true;
}

bool read_u32(std::span<const std::uint8_t> data, std::size_t& cursor, std::uint32_t& out) {
    if (cursor + 4 > data.size()) return false;
    out = static_cast<std::uint32_t>(data[cursor])
        | (static_cast<std::uint32_t>(data[cursor + 1]) << 8u)
        | (static_cast<std::uint32_t>(data[cursor + 2]) << 16u)
        | (static_cast<std::uint32_t>(data[cursor + 3]) << 24u);
    cursor += 4;
    return true;
}

bool read_u64(std::span<const std::uint8_t> data, std::size_t& cursor, std::uint64_t& out) {
    if (cursor + 8 > data.size()) return false;
    out = 0;
    for (int i = 0; i < 8; ++i) {
        out |= (static_cast<std::uint64_t>(data[cursor + static_cast<std::size_t>(i)]) << (8u * i));
    }
    cursor += 8;
    return true;
}

void write_string(std::vector<std::uint8_t>& out, std::string_view value) {
    write_u32(out, static_cast<std::uint32_t>(value.size()));
    out.insert(out.end(), value.begin(), value.end());
}

bool read_string(std::span<const std::uint8_t> data, std::size_t& cursor, std::string& out) {
    std::uint32_t size = 0;
    if (!read_u32(data, cursor, size)) return false;
    if (cursor + size > data.size()) return false;
    out.assign(reinterpret_cast<const char*>(data.data() + cursor), size);
    cursor += size;
    return true;
}

void write_pid(std::vector<std::uint8_t>& out, const types::Pid& pid) {
    write_u64(out, pid.node_id);
    write_u64(out, pid.actor_id);
    write_u32(out, pid.incarnation);
}

bool read_pid(std::span<const std::uint8_t> data, std::size_t& cursor, types::Pid& out) {
    std::uint64_t node_id = 0;
    std::uint64_t actor_id = 0;
    std::uint32_t incarnation = 0;
    if (!read_u64(data, cursor, node_id)) return false;
    if (!read_u64(data, cursor, actor_id)) return false;
    if (!read_u32(data, cursor, incarnation)) return false;
    out.node_id = node_id;
    out.actor_id = actor_id;
    out.incarnation = incarnation;
    return true;
}

std::vector<std::uint8_t> encode_hello_frame(const HelloPayload& payload) {
    std::vector<std::uint8_t> out;
    out.reserve(64 + payload.node_name.size() + payload.cookie.size());
    write_u8(out, kEnvelopeVersion);
    write_u8(out, static_cast<std::uint8_t>(FrameType::Hello));
    write_u32(out, payload.feature_flags);
    write_u64(out, payload.node_id);
    write_string(out, payload.node_name);
    write_string(out, payload.cookie);
    return out;
}

std::optional<HelloPayload> decode_hello_frame(std::span<const std::uint8_t> frame) {
    std::size_t cursor = 0;
    std::uint8_t version = 0;
    std::uint8_t type = 0;
    std::uint32_t feature_flags = 0;
    if (!read_u8(frame, cursor, version)) return std::nullopt;
    if (!read_u8(frame, cursor, type)) return std::nullopt;
    if (!read_u32(frame, cursor, feature_flags)) return std::nullopt;
    if (version != kEnvelopeVersion) return std::nullopt;
    if (type != static_cast<std::uint8_t>(FrameType::Hello)) return std::nullopt;

    HelloPayload payload;
    payload.feature_flags = feature_flags;
    if (!read_u64(frame, cursor, payload.node_id)) return std::nullopt;
    if (!read_string(frame, cursor, payload.node_name)) return std::nullopt;
    if (!read_string(frame, cursor, payload.cookie)) return std::nullopt;
    if (cursor != frame.size()) return std::nullopt;
    return payload;
}

std::vector<std::uint8_t> encode_hello_ack_frame(const HelloAckPayload& payload) {
    std::vector<std::uint8_t> out;
    out.reserve(64 + payload.node_name.size());
    write_u8(out, kEnvelopeVersion);
    write_u8(out, static_cast<std::uint8_t>(FrameType::HelloAck));
    write_u32(out, payload.feature_flags);
    write_u8(out, static_cast<std::uint8_t>(payload.status));
    write_u64(out, payload.node_id);
    write_string(out, payload.node_name);
    return out;
}

std::optional<HelloAckPayload> decode_hello_ack_frame(std::span<const std::uint8_t> frame) {
    std::size_t cursor = 0;
    std::uint8_t version = 0;
    std::uint8_t type = 0;
    std::uint32_t feature_flags = 0;
    std::uint8_t status = 0;
    if (!read_u8(frame, cursor, version)) return std::nullopt;
    if (!read_u8(frame, cursor, type)) return std::nullopt;
    if (!read_u32(frame, cursor, feature_flags)) return std::nullopt;
    if (version != kEnvelopeVersion) return std::nullopt;
    if (type != static_cast<std::uint8_t>(FrameType::HelloAck)) return std::nullopt;
    if (!read_u8(frame, cursor, status)) return std::nullopt;

    HelloAckPayload payload;
    payload.feature_flags = feature_flags;
    payload.status = static_cast<HandshakeStatus>(status);
    if (!read_u64(frame, cursor, payload.node_id)) return std::nullopt;
    if (!read_string(frame, cursor, payload.node_name)) return std::nullopt;
    if (cursor != frame.size()) return std::nullopt;
    return payload;
}

std::vector<std::uint8_t> encode_actor_frame(const ActorEnvelope& envelope) {
    std::vector<std::uint8_t> out;
    out.reserve(64 + envelope.payload.size());
    write_u8(out, kEnvelopeVersion);
    write_u8(out, static_cast<std::uint8_t>(FrameType::ActorMessage));
    write_u32(out, envelope.feature_flags);
    write_pid(out, envelope.from);
    write_pid(out, envelope.to);
    write_u32(out, static_cast<std::uint32_t>(envelope.payload.size()));
    out.insert(out.end(), envelope.payload.begin(), envelope.payload.end());
    return out;
}

std::optional<ActorEnvelope> decode_actor_frame(std::span<const std::uint8_t> frame) {
    std::size_t cursor = 0;
    std::uint8_t version = 0;
    std::uint8_t type = 0;
    std::uint32_t feature_flags = 0;
    if (!read_u8(frame, cursor, version)) return std::nullopt;
    if (!read_u8(frame, cursor, type)) return std::nullopt;
    if (!read_u32(frame, cursor, feature_flags)) return std::nullopt;
    if (version != kEnvelopeVersion) return std::nullopt;
    if (type != static_cast<std::uint8_t>(FrameType::ActorMessage)) return std::nullopt;

    ActorEnvelope out;
    out.feature_flags = feature_flags;
    if (!read_pid(frame, cursor, out.from)) return std::nullopt;
    if (!read_pid(frame, cursor, out.to)) return std::nullopt;

    std::uint32_t payload_size = 0;
    if (!read_u32(frame, cursor, payload_size)) return std::nullopt;
    if (cursor + payload_size != frame.size()) return std::nullopt;
    out.payload.assign(frame.begin() + static_cast<std::ptrdiff_t>(cursor), frame.end());
    return out;
}

std::vector<std::uint8_t> encode_monitor_control_frame(
    FrameType type,
    const MonitorControlEnvelope& envelope) {
    std::vector<std::uint8_t> out;
    out.reserve(80);
    write_u8(out, kEnvelopeVersion);
    write_u8(out, static_cast<std::uint8_t>(type));
    write_u32(out, envelope.feature_flags);
    write_u64(out, envelope.monitor_ref);
    write_pid(out, envelope.watcher);
    write_pid(out, envelope.target);
    return out;
}

std::optional<MonitorControlEnvelope> decode_monitor_control_frame(
    std::span<const std::uint8_t> frame,
    FrameType expected_type) {
    std::size_t cursor = 0;
    std::uint8_t version = 0;
    std::uint8_t type = 0;
    std::uint32_t feature_flags = 0;
    if (!read_u8(frame, cursor, version)) return std::nullopt;
    if (!read_u8(frame, cursor, type)) return std::nullopt;
    if (!read_u32(frame, cursor, feature_flags)) return std::nullopt;
    if (version != kEnvelopeVersion) return std::nullopt;
    if (type != static_cast<std::uint8_t>(expected_type)) return std::nullopt;

    MonitorControlEnvelope out;
    out.feature_flags = feature_flags;
    if (!read_u64(frame, cursor, out.monitor_ref)) return std::nullopt;
    if (!read_pid(frame, cursor, out.watcher)) return std::nullopt;
    if (!read_pid(frame, cursor, out.target)) return std::nullopt;
    if (cursor != frame.size()) return std::nullopt;
    return out;
}

std::vector<std::uint8_t> encode_down_frame(const DownEnvelope& envelope) {
    std::vector<std::uint8_t> out;
    out.reserve(96 + envelope.reason_payload.size());
    write_u8(out, kEnvelopeVersion);
    write_u8(out, static_cast<std::uint8_t>(FrameType::DownSignal));
    write_u32(out, envelope.feature_flags);
    write_u64(out, envelope.monitor_ref);
    write_pid(out, envelope.watcher);
    write_pid(out, envelope.target);
    write_u8(out, static_cast<std::uint8_t>(envelope.reason_kind));
    write_u32(out, static_cast<std::uint32_t>(envelope.reason_payload.size()));
    out.insert(out.end(), envelope.reason_payload.begin(), envelope.reason_payload.end());
    return out;
}

std::optional<DownEnvelope> decode_down_frame(std::span<const std::uint8_t> frame) {
    std::size_t cursor = 0;
    std::uint8_t version = 0;
    std::uint8_t type = 0;
    std::uint32_t feature_flags = 0;
    if (!read_u8(frame, cursor, version)) return std::nullopt;
    if (!read_u8(frame, cursor, type)) return std::nullopt;
    if (!read_u32(frame, cursor, feature_flags)) return std::nullopt;
    if (version != kEnvelopeVersion) return std::nullopt;
    if (type != static_cast<std::uint8_t>(FrameType::DownSignal)) return std::nullopt;

    DownEnvelope out;
    out.feature_flags = feature_flags;
    if (!read_u64(frame, cursor, out.monitor_ref)) return std::nullopt;
    if (!read_pid(frame, cursor, out.watcher)) return std::nullopt;
    if (!read_pid(frame, cursor, out.target)) return std::nullopt;

    std::uint8_t reason_kind = 0;
    if (!read_u8(frame, cursor, reason_kind)) return std::nullopt;
    out.reason_kind = static_cast<NodeTransport::RemoteDownReasonKind>(reason_kind);

    std::uint32_t payload_size = 0;
    if (!read_u32(frame, cursor, payload_size)) return std::nullopt;
    if (cursor + payload_size != frame.size()) return std::nullopt;
    out.reason_payload.assign(
        frame.begin() + static_cast<std::ptrdiff_t>(cursor),
        frame.end());
    return out;
}

[[nodiscard]] std::string decode_ack_error(HandshakeStatus status) {
    switch (status) {
        case HandshakeStatus::Ok:
            return {};
        case HandshakeStatus::BadCookie:
            return "remote node rejected cookie";
        case HandshakeStatus::Incompatible:
            return "remote node rejected protocol version or feature flags";
    }
    return "remote node rejected handshake";
}

} // namespace

class NodeTransport::Impl {
public:
    struct ConnectionState {
        nng_socket socket{};
        bool socket_open{false};
        bool listener_side{false};
        bool handshake_complete{false};
        std::uint32_t remote_feature_flags{0};
        std::atomic<bool> stop{false};
        std::thread reader{};
        std::mutex send_mutex{};
        std::string endpoint{};
        std::string remote_node_name{};
        std::uint64_t remote_node_id{0};
    };

    Impl(std::uint64_t local_node_id, NodeTransport::Callbacks callbacks)
        : local_node_id_(local_node_id),
          callbacks_(std::move(callbacks)) {}

    ~Impl() {
        shutdown();
    }

    [[nodiscard]] bool configure(
        std::string node_name,
        std::string cookie,
        std::string* error_message) {
        if (node_name.empty()) {
            if (error_message) *error_message = "node name must be non-empty";
            return false;
        }

        std::lock_guard lock(mutex_);
        if (connection_) {
            if (error_message) *error_message = "cannot reconfigure while connected";
            return false;
        }

        node_name_ = std::move(node_name);
        cookie_ = std::move(cookie);
        return true;
    }

    [[nodiscard]] std::string node_name() const {
        std::lock_guard lock(mutex_);
        return node_name_;
    }

    [[nodiscard]] std::uint64_t local_node_id() const noexcept {
        return local_node_id_;
    }

    [[nodiscard]] bool listen(std::string endpoint, std::string* error_message) {
        if (endpoint.empty()) {
            if (error_message) *error_message = "node-listen: endpoint must be non-empty";
            return false;
        }

        {
            std::lock_guard lock(mutex_);
            if (node_name_.empty()) {
                if (error_message) *error_message = "node-listen: node is not configured";
                return false;
            }
            if (connection_) {
                if (error_message) *error_message = "node-listen: transport is already active";
                return false;
            }
        }

        nng_socket socket{};
        int rv = nng_pair0_open(&socket);
        if (rv != 0) {
            if (error_message) *error_message = "node-listen: " + std::string(nng_strerror(rv));
            return false;
        }

        rv = nng_socket_set_ms(socket, NNG_OPT_RECVTIMEO, kSocketReceiveTimeoutMs);
        if (rv != 0) {
            (void)nng_close(socket);
            if (error_message) *error_message = "node-listen: " + std::string(nng_strerror(rv));
            return false;
        }

        rv = nng_listen(socket, endpoint.c_str(), nullptr, 0);
        if (rv != 0) {
            (void)nng_close(socket);
            if (error_message) *error_message = "node-listen: " + std::string(nng_strerror(rv));
            return false;
        }

        auto connection = std::make_shared<ConnectionState>();
        connection->socket = socket;
        connection->socket_open = true;
        connection->listener_side = true;
        connection->endpoint = std::move(endpoint);

        {
            std::lock_guard lock(mutex_);
            connection_ = connection;
        }

        connection->reader = std::thread([this, connection] {
            reader_loop(connection);
        });
        return true;
    }

    [[nodiscard]] bool connect(std::string endpoint, std::string* error_message) {
        if (endpoint.empty()) {
            if (error_message) *error_message = "node-connect: endpoint must be non-empty";
            return false;
        }

        std::string node_name;
        std::string cookie;
        {
            std::lock_guard lock(mutex_);
            if (node_name_.empty()) {
                if (error_message) *error_message = "node-connect: node is not configured";
                return false;
            }
            if (connection_) {
                if (error_message) *error_message = "node-connect: transport is already active";
                return false;
            }
            node_name = node_name_;
            cookie = cookie_;
        }

        nng_socket socket{};
        int rv = nng_pair0_open(&socket);
        if (rv != 0) {
            if (error_message) *error_message = "node-connect: " + std::string(nng_strerror(rv));
            return false;
        }

        rv = nng_socket_set_ms(socket, NNG_OPT_RECVTIMEO, kSocketReceiveTimeoutMs);
        if (rv != 0) {
            (void)nng_close(socket);
            if (error_message) *error_message = "node-connect: " + std::string(nng_strerror(rv));
            return false;
        }

        rv = nng_dial(socket, endpoint.c_str(), nullptr, 0);
        if (rv != 0) {
            (void)nng_close(socket);
            if (error_message) *error_message = "node-connect: " + std::string(nng_strerror(rv));
            return false;
        }

        HelloPayload hello;
        hello.feature_flags = kHandshakeFeatureFlags;
        hello.node_id = local_node_id_;
        hello.node_name = node_name;
        hello.cookie = cookie;
        auto hello_frame = encode_hello_frame(hello);
        rv = nng_send(socket, hello_frame.data(), hello_frame.size(), 0);
        if (rv != 0) {
            (void)nng_close(socket);
            if (error_message) *error_message = "node-connect: " + std::string(nng_strerror(rv));
            return false;
        }

        HelloAckPayload ack;
        bool ack_received = false;
        const auto deadline = std::chrono::steady_clock::now() + kHandshakeTimeout;
        for (;;) {
            std::uint8_t* raw = nullptr;
            std::size_t size = 0;
            rv = nng_recv(socket, &raw, &size, NNG_FLAG_ALLOC);
            if (rv == NNG_ETIMEDOUT) {
                if (std::chrono::steady_clock::now() >= deadline) break;
                continue;
            }
            if (rv != 0) break;

            std::vector<std::uint8_t> frame(raw, raw + size);
            nng_free(raw, size);
            auto maybe_ack = decode_hello_ack_frame(frame);
            if (!maybe_ack.has_value()) {
                continue;
            }
            ack = std::move(*maybe_ack);
            ack_received = true;
            break;
        }

        if (!ack_received) {
            (void)nng_close(socket);
            if (error_message) {
                *error_message = (rv == NNG_ETIMEDOUT)
                    ? "node-connect: handshake timeout"
                    : "node-connect: " + std::string(nng_strerror(rv));
            }
            return false;
        }

        if (ack.status != HandshakeStatus::Ok) {
            (void)nng_close(socket);
            if (error_message) {
                *error_message = "node-connect: " + decode_ack_error(ack.status);
            }
            return false;
        }

        if ((ack.feature_flags & kFeatureRemoteSend) == 0u) {
            (void)nng_close(socket);
            if (error_message) {
                *error_message = "node-connect: remote node does not support remote actor send";
            }
            return false;
        }

        auto connection = std::make_shared<ConnectionState>();
        connection->socket = socket;
        connection->socket_open = true;
        connection->listener_side = false;
        connection->handshake_complete = true;
        connection->remote_feature_flags = ack.feature_flags;
        connection->endpoint = std::move(endpoint);
        connection->remote_node_id = ack.node_id;
        connection->remote_node_name = std::move(ack.node_name);

        std::optional<ConnectedNode> node_up_event;
        {
            std::lock_guard lock(mutex_);
            connection_ = connection;
            node_up_event = register_connected_locked(*connection);
        }
        if (node_up_event.has_value() && callbacks_.node_up) {
            callbacks_.node_up(*node_up_event);
        }

        connection->reader = std::thread([this, connection] {
            reader_loop(connection);
        });
        return true;
    }

    [[nodiscard]] bool disconnect_node(std::string_view node_name) {
        std::shared_ptr<ConnectionState> connection;
        std::optional<ConnectedNode> node_down_event;
        {
            std::lock_guard lock(mutex_);
            if (!connection_) return false;
            if (connection_->remote_node_name != node_name) return false;
            connection = connection_;
            node_down_event = unregister_connected_locked(*connection);
            connection_.reset();
        }
        if (node_down_event.has_value() && callbacks_.node_down) {
            callbacks_.node_down(*node_down_event, NodeDownReason::Disconnected);
        }

        close_connection(connection);
        return true;
    }

    [[nodiscard]] std::vector<ConnectedNode> connected_nodes() const {
        std::lock_guard lock(mutex_);
        std::vector<ConnectedNode> out;
        out.reserve(nodes_by_id_.size());
        for (const auto& [_, node] : nodes_by_id_) {
            out.push_back(node);
        }
        std::sort(out.begin(), out.end(), [](const ConnectedNode& lhs, const ConnectedNode& rhs) {
            return lhs.node_name < rhs.node_name;
        });
        return out;
    }

    [[nodiscard]] SendResult send_remote(
        const types::Pid& from,
        const types::Pid& to,
        BinaryMessage payload) {
        auto lookup = route_to_node(to.node_id);
        if (!lookup.has_value()) return lookup.error();
        auto& connection = lookup->connection;

        if ((connection->remote_feature_flags & kFeatureRemoteSend) == 0u) {
            return SendResult{
                .code = SendResultCode::NoRoute,
                .detail = "remote node does not support remote actor send"};
        }

        ActorEnvelope envelope;
        envelope.feature_flags = kFeatureRemoteSend;
        envelope.from = from;
        envelope.to = to;
        envelope.payload = std::move(payload);
        auto frame = encode_actor_frame(envelope);

        return send_frame(*connection, std::move(frame));
    }

    [[nodiscard]] SendResult send_remote_monitor(
        const types::Pid& watcher,
        const types::Pid& target,
        MonitorRef monitor_ref) {
        auto lookup = route_to_node(target.node_id);
        if (!lookup.has_value()) return lookup.error();
        auto& connection = lookup->connection;

        if ((connection->remote_feature_flags & kFeatureRemoteMonitor) == 0u) {
            return SendResult{
                .code = SendResultCode::NoRoute,
                .detail = "remote node does not support remote monitors"};
        }

        MonitorControlEnvelope envelope;
        envelope.feature_flags = kFeatureRemoteMonitor;
        envelope.monitor_ref = monitor_ref;
        envelope.watcher = watcher;
        envelope.target = target;
        auto frame = encode_monitor_control_frame(FrameType::MonitorRequest, envelope);
        return send_frame(*connection, std::move(frame));
    }

    [[nodiscard]] SendResult send_remote_demonitor(
        const types::Pid& watcher,
        const types::Pid& target,
        MonitorRef monitor_ref) {
        auto lookup = route_to_node(target.node_id);
        if (!lookup.has_value()) return lookup.error();
        auto& connection = lookup->connection;

        if ((connection->remote_feature_flags & kFeatureRemoteMonitor) == 0u) {
            return SendResult{
                .code = SendResultCode::NoRoute,
                .detail = "remote node does not support remote monitors"};
        }

        MonitorControlEnvelope envelope;
        envelope.feature_flags = kFeatureRemoteMonitor;
        envelope.monitor_ref = monitor_ref;
        envelope.watcher = watcher;
        envelope.target = target;
        auto frame = encode_monitor_control_frame(FrameType::DemonitorRequest, envelope);
        return send_frame(*connection, std::move(frame));
    }

    [[nodiscard]] SendResult send_remote_down(RemoteDownSignal signal) {
        auto lookup = route_to_node(signal.watcher.node_id);
        if (!lookup.has_value()) return lookup.error();
        auto& connection = lookup->connection;

        if ((connection->remote_feature_flags & kFeatureRemoteMonitor) == 0u) {
            return SendResult{
                .code = SendResultCode::NoRoute,
                .detail = "remote node does not support remote monitors"};
        }

        DownEnvelope envelope;
        envelope.feature_flags = kFeatureRemoteMonitor;
        envelope.monitor_ref = signal.monitor_ref;
        envelope.watcher = signal.watcher;
        envelope.target = signal.target;
        envelope.reason_kind = signal.reason_kind;
        envelope.reason_payload = std::move(signal.reason_payload);
        auto frame = encode_down_frame(envelope);
        return send_frame(*connection, std::move(frame));
    }

    void shutdown() {
        std::shared_ptr<ConnectionState> connection;
        std::optional<ConnectedNode> node_down_event;
        {
            std::lock_guard lock(mutex_);
            connection = connection_;
            if (connection_) {
                node_down_event = unregister_connected_locked(*connection_);
                connection_.reset();
            }
            nodes_by_id_.clear();
            node_ids_by_name_.clear();
        }
        if (node_down_event.has_value() && callbacks_.node_down) {
            callbacks_.node_down(*node_down_event, NodeDownReason::Disconnected);
        }
        close_connection(connection);
    }

private:
    struct RouteLookup {
        std::shared_ptr<ConnectionState> connection{};
    };

    [[nodiscard]] std::expected<RouteLookup, SendResult> route_to_node(std::uint64_t node_id) const {
        std::lock_guard lock(mutex_);
        if (!connection_) {
            return std::unexpected(SendResult{
                .code = SendResultCode::NoRoute,
                .detail = "no active node transport connection"});
        }
        if (!connection_->handshake_complete) {
            return std::unexpected(SendResult{
                .code = SendResultCode::NoRoute,
                .detail = "node handshake is not complete"});
        }
        if (connection_->remote_node_id != node_id) {
            return std::unexpected(SendResult{
                .code = SendResultCode::NoRoute,
                .detail = "no route for destination node id"});
        }
        return RouteLookup{.connection = connection_};
    }

    [[nodiscard]] SendResult send_frame(
        ConnectionState& connection,
        std::vector<std::uint8_t> frame) const {
        int rv = 0;
        {
            std::lock_guard send_lock(connection.send_mutex);
            rv = nng_send(connection.socket, frame.data(), frame.size(), 0);
        }
        if (rv != 0) {
            return SendResult{
                .code = SendResultCode::TransportError,
                .detail = std::string("nng-send failed: ") + nng_strerror(rv)};
        }
        return SendResult{
            .code = SendResultCode::Delivered,
            .detail = {}};
    }

    void reader_loop(const std::shared_ptr<ConnectionState>& connection) {
        for (;;) {
            if (connection->stop.load(std::memory_order_acquire)) break;

            std::uint8_t* raw = nullptr;
            std::size_t size = 0;
            const int rv = nng_recv(connection->socket, &raw, &size, NNG_FLAG_ALLOC);
            if (rv == NNG_ETIMEDOUT) continue;
            if (rv != 0) break;

            std::vector<std::uint8_t> frame(raw, raw + size);
            nng_free(raw, size);

            if (!connection->handshake_complete) {
                if (!connection->listener_side) continue;
                auto hello = decode_hello_frame(frame);
                if (!hello.has_value()) {
                    send_handshake_ack(*connection, HandshakeStatus::Incompatible);
                    break;
                }
                if ((hello->feature_flags & kFeatureRemoteSend) == 0u) {
                    send_handshake_ack(*connection, HandshakeStatus::Incompatible);
                    if (callbacks_.node_down) {
                        ConnectedNode node;
                        node.node_id = hello->node_id;
                        node.node_name = hello->node_name;
                        node.endpoint = connection->endpoint;
                        callbacks_.node_down(node, NodeDownReason::Incompatible);
                    }
                    break;
                }

                std::string cookie;
                {
                    std::lock_guard lock(mutex_);
                    cookie = cookie_;
                }
                if (hello->cookie != cookie) {
                    send_handshake_ack(*connection, HandshakeStatus::BadCookie);
                    if (callbacks_.node_down) {
                        ConnectedNode node;
                        node.node_id = hello->node_id;
                        node.node_name = hello->node_name;
                        node.endpoint = connection->endpoint;
                        callbacks_.node_down(node, NodeDownReason::BadCookie);
                    }
                    break;
                }

                connection->remote_node_id = hello->node_id;
                connection->remote_node_name = std::move(hello->node_name);
                connection->handshake_complete = true;
                connection->remote_feature_flags = hello->feature_flags;

                std::optional<ConnectedNode> node_up_event;
                {
                    std::lock_guard lock(mutex_);
                    node_up_event = register_connected_locked(*connection);
                }
                if (node_up_event.has_value() && callbacks_.node_up) {
                    callbacks_.node_up(*node_up_event);
                }

                send_handshake_ack(*connection, HandshakeStatus::Ok);
                continue;
            }

            auto monitor_request = decode_monitor_control_frame(frame, FrameType::MonitorRequest);
            if (monitor_request.has_value()) {
                if ((monitor_request->feature_flags & kFeatureRemoteMonitor) == 0u) continue;
                if (monitor_request->target.node_id != local_node_id_) continue;
                if (monitor_request->watcher.node_id != connection->remote_node_id) continue;
                if (callbacks_.remote_monitor) {
                    callbacks_.remote_monitor(
                        monitor_request->watcher,
                        monitor_request->target,
                        monitor_request->monitor_ref);
                }
                continue;
            }

            auto demonitor_request = decode_monitor_control_frame(frame, FrameType::DemonitorRequest);
            if (demonitor_request.has_value()) {
                if ((demonitor_request->feature_flags & kFeatureRemoteMonitor) == 0u) continue;
                if (demonitor_request->target.node_id != local_node_id_) continue;
                if (demonitor_request->watcher.node_id != connection->remote_node_id) continue;
                if (callbacks_.remote_demonitor) {
                    callbacks_.remote_demonitor(
                        demonitor_request->watcher,
                        demonitor_request->target,
                        demonitor_request->monitor_ref);
                }
                continue;
            }

            auto down_signal = decode_down_frame(frame);
            if (down_signal.has_value()) {
                if ((down_signal->feature_flags & kFeatureRemoteMonitor) == 0u) continue;
                if (down_signal->watcher.node_id != local_node_id_) continue;
                if (down_signal->target.node_id != connection->remote_node_id) continue;
                if (callbacks_.remote_down) {
                    RemoteDownSignal signal;
                    signal.watcher = down_signal->watcher;
                    signal.target = down_signal->target;
                    signal.monitor_ref = down_signal->monitor_ref;
                    signal.reason_kind = down_signal->reason_kind;
                    signal.reason_payload = std::move(down_signal->reason_payload);
                    callbacks_.remote_down(signal);
                }
                continue;
            }

            auto envelope = decode_actor_frame(frame);
            if (!envelope.has_value()) continue;
            if ((envelope->feature_flags & kFeatureRemoteSend) == 0u) continue;
            if (!callbacks_.deliver_message) continue;
            (void)callbacks_.deliver_message(envelope->to, std::move(envelope->payload));
        }

        std::optional<ConnectedNode> node_down_event;
        {
            std::lock_guard lock(mutex_);
            node_down_event = unregister_connected_locked(*connection);
            if (connection_ == connection) {
                connection_.reset();
            }
        }
        if (node_down_event.has_value() && callbacks_.node_down) {
            callbacks_.node_down(*node_down_event, NodeDownReason::Disconnected);
        }
        close_socket_only(*connection);
        if (connection.use_count() == 1
            && connection->reader.joinable()
            && connection->reader.get_id() == std::this_thread::get_id()) {
            connection->reader.detach();
        }
    }

    void send_handshake_ack(ConnectionState& connection, HandshakeStatus status) {
        std::string node_name;
        {
            std::lock_guard lock(mutex_);
            node_name = node_name_;
        }

        HelloAckPayload ack;
        ack.feature_flags = kHandshakeFeatureFlags;
        ack.status = status;
        ack.node_id = local_node_id_;
        ack.node_name = std::move(node_name);

        auto frame = encode_hello_ack_frame(ack);
        std::lock_guard send_lock(connection.send_mutex);
        (void)nng_send(connection.socket, frame.data(), frame.size(), 0);
    }

    void close_connection(const std::shared_ptr<ConnectionState>& connection) {
        if (!connection) return;
        connection->stop.store(true, std::memory_order_release);
        close_socket_only(*connection);
        if (connection->reader.joinable()) {
            if (connection->reader.get_id() == std::this_thread::get_id()) {
                connection->reader.detach();
            } else {
                connection->reader.join();
            }
        }
    }

    void close_socket_only(ConnectionState& connection) {
        if (!connection.socket_open) return;
        connection.socket_open = false;
        (void)nng_close(connection.socket);
    }

    [[nodiscard]] std::optional<ConnectedNode> register_connected_locked(
        const ConnectionState& connection) {
        if (connection.remote_node_id == 0 || connection.remote_node_name.empty()) return std::nullopt;
        if (nodes_by_id_.contains(connection.remote_node_id)) return std::nullopt;
        if (node_ids_by_name_.contains(connection.remote_node_name)) return std::nullopt;
        ConnectedNode node;
        node.node_id = connection.remote_node_id;
        node.node_name = connection.remote_node_name;
        node.endpoint = connection.endpoint;
        nodes_by_id_[node.node_id] = node;
        node_ids_by_name_[node.node_name] = node.node_id;
        return node;
    }

    [[nodiscard]] std::optional<ConnectedNode> unregister_connected_locked(
        const ConnectionState& connection) {
        std::optional<ConnectedNode> removed;
        if (connection.remote_node_id != 0) {
            auto it = nodes_by_id_.find(connection.remote_node_id);
            if (it != nodes_by_id_.end()) {
                removed = it->second;
                nodes_by_id_.erase(it);
            }
        }
        if (!connection.remote_node_name.empty()) {
            node_ids_by_name_.erase(connection.remote_node_name);
        }
        return removed;
    }

    const std::uint64_t local_node_id_{0};
    NodeTransport::Callbacks callbacks_{};

    mutable std::mutex mutex_{};
    std::string node_name_{"nonode@local"};
    std::string cookie_{};
    std::shared_ptr<ConnectionState> connection_{};
    std::unordered_map<std::uint64_t, ConnectedNode> nodes_by_id_{};
    std::unordered_map<std::string, std::uint64_t> node_ids_by_name_{};
};

NodeTransport::NodeTransport(
    std::uint64_t local_node_id,
    Callbacks callbacks)
    : impl_(std::make_unique<Impl>(local_node_id, std::move(callbacks))) {}

NodeTransport::~NodeTransport() = default;

bool NodeTransport::configure(
    std::string node_name,
    std::string cookie,
    std::string* error_message) {
    return impl_->configure(std::move(node_name), std::move(cookie), error_message);
}

std::string NodeTransport::node_name() const {
    return impl_->node_name();
}

std::uint64_t NodeTransport::local_node_id() const noexcept {
    return impl_->local_node_id();
}

bool NodeTransport::listen(
    std::string endpoint,
    std::string* error_message) {
    return impl_->listen(std::move(endpoint), error_message);
}

bool NodeTransport::connect(
    std::string endpoint,
    std::string* error_message) {
    return impl_->connect(std::move(endpoint), error_message);
}

bool NodeTransport::disconnect_node(std::string_view node_name) {
    return impl_->disconnect_node(node_name);
}

std::vector<NodeTransport::ConnectedNode> NodeTransport::connected_nodes() const {
    return impl_->connected_nodes();
}

NodeTransport::SendResult NodeTransport::send_remote(
    const types::Pid& from,
    const types::Pid& to,
    BinaryMessage payload) {
    return impl_->send_remote(from, to, std::move(payload));
}

NodeTransport::SendResult NodeTransport::send_remote_monitor(
    const types::Pid& watcher,
    const types::Pid& target,
    MonitorRef monitor_ref) {
    return impl_->send_remote_monitor(watcher, target, monitor_ref);
}

NodeTransport::SendResult NodeTransport::send_remote_demonitor(
    const types::Pid& watcher,
    const types::Pid& target,
    MonitorRef monitor_ref) {
    return impl_->send_remote_demonitor(watcher, target, monitor_ref);
}

NodeTransport::SendResult NodeTransport::send_remote_down(RemoteDownSignal signal) {
    return impl_->send_remote_down(std::move(signal));
}

void NodeTransport::shutdown() {
    impl_->shutdown();
}

} // namespace eta::runtime::actor
