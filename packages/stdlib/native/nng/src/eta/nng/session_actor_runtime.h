/**
 * @file session_actor_runtime.h
 * @brief Actor runtime implementation backed by NNG process-manager primitives.
 */

#pragma once

#include <functional>
#include <memory>
#include <string>
#include <string_view>

#include "eta/native/actor_runtime.h"

namespace eta::nng {

/**
 * @brief NNG-backed actor runtime used by session::Driver.
 */
class SessionActorRuntime final : public native::ActorRuntime {
public:
    SessionActorRuntime();
    ~SessionActorRuntime() override;

    SessionActorRuntime(const SessionActorRuntime&) = delete;
    SessionActorRuntime& operator=(const SessionActorRuntime&) = delete;
    SessionActorRuntime(SessionActorRuntime&&) = delete;
    SessionActorRuntime& operator=(SessionActorRuntime&&) = delete;

    /**
     * @brief Install the `current-mailbox` socket for a spawned child runtime.
     */
    bool install_mailbox(runtime::memory::heap::Heap& heap,
                         const std::string& endpoint) override;

    /**
     * @brief Register actor worker factories used by NNG spawn primitives.
     */
    void install_worker_factories(const std::string& module_search_path,
                                  StreamSink stdout_sink = {},
                                  StreamSink stderr_sink = {}) override;

    /**
     * @brief Subscribe to actor lifecycle events from the process manager.
     */
    void on_actor_lifecycle(std::function<void(const ActorEvent&)> on_event) override;

    [[nodiscard]] native::ActorProcessManager* process_manager() noexcept override;
    [[nodiscard]] const native::ActorProcessManager* process_manager() const noexcept override;

    [[nodiscard]] runtime::nanbox::LispVal mailbox() const noexcept override;
    [[nodiscard]] runtime::nanbox::LispVal* mailbox_slot() noexcept override;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

/**
 * @brief Construct a generic actor runtime service backed by NNG.
 */
[[nodiscard]] std::unique_ptr<native::ActorRuntime> make_session_actor_runtime();

} // namespace eta::nng
