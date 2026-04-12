#pragma once

#include <deque>
#include <ostream>
#include <vector>
#include <cstdint>

#include <nng/nng.h>

namespace eta::nng {

    /// Supported nng socket protocols.
    enum class NngProtocol : uint8_t {
        Pair, Req, Rep, Pub, Sub, Push, Pull, Surveyor, Respondent, Bus
    };

    /// RAII wrapper around nng_socket.
    /// Stored as ObjectKind::NngSocket in the Eta heap.
    /// The destructor calls nng_close() automatically when the GC
    /// deallocates the entry.
    ///
    /// pending_msgs_ holds messages received during nng-poll that have
    /// not yet been consumed by recv!.  recv! drains this queue before
    /// calling nng_recv.
    struct NngSocketPtr {
        nng_socket socket = NNG_SOCKET_INITIALIZER;
        NngProtocol protocol{NngProtocol::Pair};
        bool listening{false};
        bool dialed{false};
        bool closed{false};

        /// Buffered messages from nng-poll peek (byte payloads, already freed from nng).
        std::deque<std::vector<uint8_t>> pending_msgs;

        ~NngSocketPtr() {
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
              pending_msgs(std::move(o.pending_msgs))
        {
            o.socket = NNG_SOCKET_INITIALIZER;
            o.closed = true;
        }

        NngSocketPtr& operator=(NngSocketPtr&& o) noexcept {
            if (this != &o) {
                if (!closed) nng_close(socket);
                socket = o.socket;
                protocol = o.protocol;
                listening = o.listening;
                dialed = o.dialed;
                closed = o.closed;
                pending_msgs = std::move(o.pending_msgs);
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

