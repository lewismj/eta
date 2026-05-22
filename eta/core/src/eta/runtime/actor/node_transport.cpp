#include "eta/runtime/actor/node_transport.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstring>
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
constexpr int kSocketReceiveTimeoutMs = 100;
constexpr auto kHandshakeTimeout = std::chrono::seconds(3);

enum class FrameType : std::uint8_t {
    Hello = 1,
    HelloAck = 2,
    ActorMessage = 3,
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
        std::atomic<bool> stop{false};
        std::thread reader{};
        std::mutex send_mutex{};
        std::string endpoint{};
        std::string remote_node_name{};
        std::uint64_t remote_node_id{0};
    };

    Impl(std::uint64_t local_node_id, DeliverCallback deliver_message)
        : local_node_id_(local_node_id),
          deliver_message_(std::move(deliver_message)) {}

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
        hello.feature_flags = kFeatureRemoteSend;
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
        connection->endpoint = std::move(endpoint);
        connection->remote_node_id = ack.node_id;
        connection->remote_node_name = std::move(ack.node_name);

        {
            std::lock_guard lock(mutex_);
            connection_ = connection;
            register_connected_locked(*connection);
        }

        connection->reader = std::thread([this, connection] {
            reader_loop(connection);
        });
        return true;
    }

    [[nodiscard]] bool disconnect_node(std::string_view node_name) {
        std::shared_ptr<ConnectionState> connection;
        {
            std::lock_guard lock(mutex_);
            if (!connection_) return false;
            if (connection_->remote_node_name != node_name) return false;
            connection = connection_;
            unregister_connected_locked(*connection);
            connection_.reset();
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
        std::shared_ptr<ConnectionState> connection;
        {
            std::lock_guard lock(mutex_);
            if (!connection_) {
                return SendResult{
                    .code = SendResultCode::NoRoute,
                    .detail = "no active node transport connection"};
            }
            if (!connection_->handshake_complete) {
                return SendResult{
                    .code = SendResultCode::NoRoute,
                    .detail = "node handshake is not complete"};
            }
            if (connection_->remote_node_id != to.node_id) {
                return SendResult{
                    .code = SendResultCode::NoRoute,
                    .detail = "no route for destination node id"};
            }
            connection = connection_;
        }

        ActorEnvelope envelope;
        envelope.feature_flags = kFeatureRemoteSend;
        envelope.from = from;
        envelope.to = to;
        envelope.payload = std::move(payload);
        auto frame = encode_actor_frame(envelope);

        int rv = 0;
        {
            std::lock_guard send_lock(connection->send_mutex);
            rv = nng_send(connection->socket, frame.data(), frame.size(), 0);
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

    void shutdown() {
        std::shared_ptr<ConnectionState> connection;
        {
            std::lock_guard lock(mutex_);
            connection = connection_;
            if (connection_) {
                unregister_connected_locked(*connection_);
                connection_.reset();
            }
            nodes_by_id_.clear();
            node_ids_by_name_.clear();
        }
        close_connection(connection);
    }

private:
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
                    break;
                }

                std::string cookie;
                {
                    std::lock_guard lock(mutex_);
                    cookie = cookie_;
                }
                if (hello->cookie != cookie) {
                    send_handshake_ack(*connection, HandshakeStatus::BadCookie);
                    break;
                }

                connection->remote_node_id = hello->node_id;
                connection->remote_node_name = std::move(hello->node_name);
                connection->handshake_complete = true;

                {
                    std::lock_guard lock(mutex_);
                    register_connected_locked(*connection);
                }

                send_handshake_ack(*connection, HandshakeStatus::Ok);
                continue;
            }

            auto envelope = decode_actor_frame(frame);
            if (!envelope.has_value()) continue;
            if ((envelope->feature_flags & kFeatureRemoteSend) == 0u) continue;
            if (!deliver_message_) continue;
            (void)deliver_message_(envelope->to, std::move(envelope->payload));
        }

        {
            std::lock_guard lock(mutex_);
            unregister_connected_locked(*connection);
            if (connection_ == connection) {
                connection_.reset();
            }
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
        ack.feature_flags = kFeatureRemoteSend;
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

    void register_connected_locked(const ConnectionState& connection) {
        if (connection.remote_node_id == 0 || connection.remote_node_name.empty()) return;
        ConnectedNode node;
        node.node_id = connection.remote_node_id;
        node.node_name = connection.remote_node_name;
        node.endpoint = connection.endpoint;
        nodes_by_id_[node.node_id] = node;
        node_ids_by_name_[node.node_name] = node.node_id;
    }

    void unregister_connected_locked(const ConnectionState& connection) {
        if (connection.remote_node_id != 0) {
            nodes_by_id_.erase(connection.remote_node_id);
        }
        if (!connection.remote_node_name.empty()) {
            node_ids_by_name_.erase(connection.remote_node_name);
        }
    }

    const std::uint64_t local_node_id_{0};
    DeliverCallback deliver_message_{};

    mutable std::mutex mutex_{};
    std::string node_name_{"nonode@local"};
    std::string cookie_{};
    std::shared_ptr<ConnectionState> connection_{};
    std::unordered_map<std::uint64_t, ConnectedNode> nodes_by_id_{};
    std::unordered_map<std::string, std::uint64_t> node_ids_by_name_{};
};

NodeTransport::NodeTransport(
    std::uint64_t local_node_id,
    DeliverCallback deliver_message)
    : impl_(std::make_unique<Impl>(local_node_id, std::move(deliver_message))) {}

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

void NodeTransport::shutdown() {
    impl_->shutdown();
}

} // namespace eta::runtime::actor
