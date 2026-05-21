#include "eta/runtime/actor/mailbox.h"

#include <cstddef>

namespace eta::runtime::actor {

Mailbox::Message Mailbox::Message::make_payload(BinaryMessage payload_message) {
    Message message;
    message.kind = Kind::Payload;
    message.payload = std::move(payload_message);
    return message;
}

Mailbox::Message Mailbox::Message::make_exit(types::Pid from_pid, ExitReason exit_reason) {
    Message message;
    message.kind = Kind::ExitSignal;
    message.from = from_pid;
    message.reason = std::move(exit_reason);
    return message;
}

Mailbox::Message Mailbox::Message::make_down(
    std::uint64_t reference,
    types::Pid down_pid,
    ExitReason exit_reason) {
    Message message;
    message.kind = Kind::DownSignal;
    message.monitor_ref = reference;
    message.pid = down_pid;
    message.reason = std::move(exit_reason);
    return message;
}

bool Mailbox::push(Message message) {
    {
        std::lock_guard lock(mutex_);
        if (closed_) return false;
        queue_.push_back(std::move(message));
    }
    ready_.notify_one();
    return true;
}

std::optional<Mailbox::Message> Mailbox::pop(
    std::optional<std::chrono::milliseconds> timeout) {
    std::unique_lock lock(mutex_);

    const auto has_message_or_closed = [this] {
        return !queue_.empty() || closed_;
    };

    if (timeout.has_value()) {
        if (timeout->count() > 0) {
            const bool woke = ready_.wait_for(lock, *timeout, has_message_or_closed);
            if (!woke) return std::nullopt;
        }
    } else {
        ready_.wait(lock, has_message_or_closed);
    }

    if (queue_.empty()) return std::nullopt;

    auto next = std::move(queue_.front());
    queue_.pop_front();
    return next;
}

std::expected<std::optional<Mailbox::Message>, error::RuntimeError> Mailbox::pop_matching(
    std::optional<std::chrono::milliseconds> timeout,
    const Matcher& matcher) {
    if (!matcher) {
        return std::unexpected(error::RuntimeError{error::VMError{
            error::RuntimeErrorCode::InternalError,
            "mailbox matcher is not callable"}});
    }

    const auto deadline = (timeout.has_value() && timeout->count() > 0)
        ? std::optional(std::chrono::steady_clock::now() + *timeout)
        : std::optional<std::chrono::steady_clock::time_point>{};

    std::unique_lock lock(mutex_);
    for (;;) {
        if (queue_.empty()) {
            if (closed_) return std::optional<Message>{};

            if (!timeout.has_value()) {
                ready_.wait(lock, [this] {
                    return closed_ || !queue_.empty();
                });
                continue;
            }

            if (timeout->count() <= 0) {
                return std::optional<Message>{};
            }

            const bool woke = ready_.wait_until(lock, *deadline, [this] {
                return closed_ || !queue_.empty();
            });
            if (!woke) return std::optional<Message>{};
            continue;
        }

        const auto scan_size = queue_.size();
        for (std::size_t index = 0; index < scan_size; ++index) {
            auto candidate = queue_[index];
            lock.unlock();
            auto decision = matcher(candidate);
            lock.lock();

            if (!decision.has_value()) return std::unexpected(decision.error());
            if (!*decision) continue;

            auto it = queue_.begin() + static_cast<std::ptrdiff_t>(index);
            auto matched = std::move(*it);
            queue_.erase(it);
            return matched;
        }

        if (closed_) return std::optional<Message>{};

        if (!timeout.has_value()) {
            const auto size_before_wait = queue_.size();
            ready_.wait(lock, [this, size_before_wait] {
                return closed_ || queue_.size() != size_before_wait;
            });
            continue;
        }

        if (timeout->count() <= 0) {
            return std::optional<Message>{};
        }

        const auto size_before_wait = queue_.size();
        const bool woke = ready_.wait_until(lock, *deadline, [this, size_before_wait] {
            return closed_ || queue_.size() != size_before_wait;
        });
        if (!woke) return std::optional<Message>{};
    }
}

std::size_t Mailbox::erase_if(const std::function<bool(const Message&)>& predicate) {
    if (!predicate) return 0;

    std::lock_guard lock(mutex_);
    std::size_t removed = 0;
    for (auto it = queue_.begin(); it != queue_.end();) {
        if (predicate(*it)) {
            it = queue_.erase(it);
            ++removed;
        } else {
            ++it;
        }
    }
    return removed;
}

std::size_t Mailbox::size() const {
    std::lock_guard lock(mutex_);
    return queue_.size();
}

void Mailbox::close() {
    {
        std::lock_guard lock(mutex_);
        closed_ = true;
    }
    ready_.notify_all();
}

} // namespace eta::runtime::actor
