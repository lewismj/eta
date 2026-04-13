#pragma once

#include <atomic>
#include <deque>
#include <memory>
#include <mutex>
#include <ostream>
#include <string>
#include <thread>
#include <vector>
#include <cstdint>

#include <nng/nng.h>

namespace eta::nng {

    /// Supported nng socket protocols.
    enum class NngProtocol : uint8_t {
        Pair, Req, Rep, Pub, Sub, Push, Pull, Surveyor, Respondent, Bus
    };

    // Heartbeat state

    /// State for the background heartbeat thread.
    /// Owned jointly by NngSocketPtr (via MonitorState) and the heartbeat thread lambda
    /// (via shared_ptr), so it stays alive until the thread exits.
    struct HeartbeatState {
        std::atomic<bool>    stop{false};
        int64_t              interval_ms{0};
        /// Epoch milliseconds (steady_clock) of the last pong received.
        /// Initialised to 0 (never received).
        std::atomic<int64_t> last_pong_epoch_ms{0};
        /// Epoch milliseconds of the last ping sent.
        std::atomic<int64_t> last_ping_epoch_ms{0};
        std::thread          thread;

        HeartbeatState() = default;
        ~HeartbeatState() {
            stop.store(true, std::memory_order_release);
            if (thread.joinable()) thread.detach();
        }
        HeartbeatState(const HeartbeatState&) = delete;
        HeartbeatState& operator=(const HeartbeatState&) = delete;
    };

    // Monitor / heartbeat state

    /// Thread-safe notification state created lazily when monitor() or
    /// enable-heartbeat() is called on a socket.
    ///
    /// notif_msgs is written by the monitor pipe callback and the heartbeat
    /// thread; it is read by recv! (VM thread).  Both directions hold the mutex.
    struct MonitorState {
        std::mutex                       mu;              ///< guards notif_msgs
        std::deque<std::vector<uint8_t>> notif_msgs;      ///< (down …) notifications
        std::atomic<bool>                closing_normally{false}; ///< set before intentional close
        bool                             monitored{false};
        std::vector<uint8_t>             monitor_down_msg; ///< pre-serialized (down endpoint "disconnected")
        std::shared_ptr<HeartbeatState>  heartbeat;        ///< null unless enable-heartbeat() called

        void push_notification(const std::vector<uint8_t>& msg) {
            std::lock_guard<std::mutex> lk(mu);
            notif_msgs.push_back(msg);
        }

        /// Pop and return the next notification, or nullopt if none.
        std::optional<std::vector<uint8_t>> pop_notification() {
            std::lock_guard<std::mutex> lk(mu);
            if (notif_msgs.empty()) return std::nullopt;
            auto m = std::move(notif_msgs.front());
            notif_msgs.pop_front();
            return m;
        }
    };

    /// RAII wrapper around nng_socket.
    /// Stored as ObjectKind::NngSocket in the Eta heap.
    /// The destructor calls nng_close() automatically when the GC
    /// deallocates the entry.
    ///
    /// pending_msgs holds messages received during nng-poll that have
    /// not yet been consumed by recv!.  recv! drains this queue before
    /// calling nng_recv.
    struct NngSocketPtr {
        nng_socket socket = NNG_SOCKET_INITIALIZER;
        NngProtocol protocol{NngProtocol::Pair};
        bool listening{false};
        bool dialed{false};
        bool closed{false};

        /// Buffered messages from nng-poll peek (byte payloads, already freed from nng).
        /// Accessed from the VM thread only; no mutex needed.
        std::deque<std::vector<uint8_t>> pending_msgs;

        /// Endpoint string for identification in down messages.
        std::string endpoint_hint;

        /// Monitoring and heartbeat state.  null unless monitor() or
        /// enable-heartbeat() has been called.
        std::shared_ptr<MonitorState> monitor_state;

        ~NngSocketPtr() {
            if (monitor_state) {
                monitor_state->closing_normally.store(true, std::memory_order_release);
                if (monitor_state->heartbeat)
                    monitor_state->heartbeat->stop.store(true, std::memory_order_release);
            }
            if (!closed) {
                nng_close(socket);
            }
        }

        // Non-copyable (socket is an OS resource).
        NngSocketPtr(const NngSocketPtr&) = delete;
        NngSocketPtr& operator=(const NngSocketPtr&) = delete;

        NngSocketPtr(NngSocketPtr&& o) noexcept
            : socket(o.socket), protocol(o.protocol),
              listening(o.listening), dialed(o.dialed), closed(o.closed),
              pending_msgs(std::move(o.pending_msgs)),
              endpoint_hint(std::move(o.endpoint_hint)),
              monitor_state(std::move(o.monitor_state))
        {
            o.socket = NNG_SOCKET_INITIALIZER;
            o.closed = true;
        }

        NngSocketPtr& operator=(NngSocketPtr&& o) noexcept {
            if (this != &o) {
                if (monitor_state) {
                    monitor_state->closing_normally.store(true, std::memory_order_release);
                    if (monitor_state->heartbeat) monitor_state->heartbeat->stop.store(true);
                }
                if (!closed) nng_close(socket);
                socket = o.socket;
                protocol = o.protocol;
                listening = o.listening;
                dialed = o.dialed;
                closed = o.closed;
                pending_msgs = std::move(o.pending_msgs);
                endpoint_hint = std::move(o.endpoint_hint);
                monitor_state = std::move(o.monitor_state);
                o.socket = NNG_SOCKET_INITIALIZER;
                o.closed = true;
            }
            return *this;
        }

        NngSocketPtr() = default;
    };

    /// Stream operator for NngProtocol — required by Boost.Test to print values.
    inline std::ostream& operator<<(std::ostream& os, NngProtocol p) {
        switch (p) {
            case NngProtocol::Pair:       return os << "NngProtocol::Pair";
            case NngProtocol::Req:        return os << "NngProtocol::Req";
            case NngProtocol::Rep:        return os << "NngProtocol::Rep";
            case NngProtocol::Pub:        return os << "NngProtocol::Pub";
            case NngProtocol::Sub:        return os << "NngProtocol::Sub";
            case NngProtocol::Push:       return os << "NngProtocol::Push";
            case NngProtocol::Pull:       return os << "NngProtocol::Pull";
            case NngProtocol::Surveyor:   return os << "NngProtocol::Surveyor";
            case NngProtocol::Respondent: return os << "NngProtocol::Respondent";
            case NngProtocol::Bus:        return os << "NngProtocol::Bus";
            default:                      return os << "NngProtocol::Unknown";
        }
    }

} // namespace eta::nng

