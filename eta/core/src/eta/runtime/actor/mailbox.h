#pragma once

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <expected>
#include <functional>
#include <mutex>
#include <optional>
#include <vector>

#include "eta/runtime/error.h"
#include "eta/runtime/types/pid.h"

namespace eta::runtime::actor {

/**
 * @brief Per-actor FIFO mailbox.
 */
class Mailbox {
public:
    using BinaryMessage = std::vector<std::uint8_t>;

    struct ExitReason {
        enum class Kind : std::uint8_t {
            Normal,
            Shutdown,
            Killed,
            Error,
            Custom,
        };

        Kind kind{Kind::Normal};
        BinaryMessage payload{};
    };

    struct Message {
        enum class Kind : std::uint8_t {
            Payload,
            ExitSignal,
            DownSignal,
        };

        Kind kind{Kind::Payload};
        BinaryMessage payload{};
        types::Pid from{};
        types::Pid pid{};
        std::uint64_t monitor_ref{0};
        ExitReason reason{};

        [[nodiscard]] static Message make_payload(BinaryMessage payload_message);
        [[nodiscard]] static Message make_exit(types::Pid from_pid, ExitReason exit_reason);
        [[nodiscard]] static Message make_down(
            std::uint64_t reference,
            types::Pid down_pid,
            ExitReason exit_reason);
    };

    using MatchDecision = std::expected<bool, error::RuntimeError>;
    using Matcher = std::function<MatchDecision(const Message&)>;

    /**
     * @brief Append one message.
     *
     * @return `false` when the mailbox is closed.
     */
    [[nodiscard]] bool push(Message message);

    /**
     * @brief Pop one message from the head of the queue.
     *
     * When `timeout` is `std::nullopt`, waits indefinitely.
     * When `timeout` is `0ms`, checks without blocking.
     */
    [[nodiscard]] std::optional<Message> pop(
        std::optional<std::chrono::milliseconds> timeout);

    /**
     * @brief Pop the first message matched by @p matcher.
     *
     * The mailbox is scanned from oldest to newest. Unmatched messages keep
     * their original order.
     *
     * @return Matched payload, timeout/closed as `std::nullopt`, or matcher
     *         evaluation error.
     */
    [[nodiscard]] std::expected<std::optional<Message>, error::RuntimeError> pop_matching(
        std::optional<std::chrono::milliseconds> timeout,
        const Matcher& matcher);

    /**
     * @brief Remove queued messages where @p predicate returns true.
     */
    [[nodiscard]] std::size_t erase_if(const std::function<bool(const Message&)>& predicate);

    [[nodiscard]] std::size_t size() const;
    void close();

private:
    mutable std::mutex mutex_{};
    std::condition_variable ready_{};
    std::deque<Message> queue_{};
    bool closed_{false};
};

} // namespace eta::runtime::actor
