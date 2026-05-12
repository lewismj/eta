/**
 * @file actor_runtime.h
 * @brief Generic actor runtime interfaces used by session and native sidecars.
 */

#pragma once

#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "eta/runtime/memory/heap.h"
#include "eta/runtime/nanbox.h"

namespace eta::native {

/**
 * @brief Snapshot row for one spawned child process.
 */
struct ActorChildInfo {
    int pid{0};
    std::string endpoint;
    std::string module_path;
    bool alive{false};
};

/**
 * @brief Snapshot row for one spawned actor thread.
 */
struct ActorThreadInfo {
    int index{0};
    std::string endpoint;
    std::string module_path;
    std::string func_name;
    bool alive{false};
};

/**
 * @brief Debug lifecycle event for actor worker threads.
 *
 * `vm` and `driver` are opaque pointers owned by the runtime host.
 */
struct ActorThreadDebugEvent {
    enum class Kind {
        Started,
        Exited,
    };

    Kind kind{Kind::Started};
    int index{-1};
    void* vm{nullptr};
    void* driver{nullptr};
    std::string name;
};

/**
 * @brief Abstract process/thread manager surfaced to session tooling.
 */
class ActorProcessManager {
public:
    using ThreadDebugListener = std::function<void(const ActorThreadDebugEvent&)>;

    virtual ~ActorProcessManager() = default;

    virtual void set_debug_listener(ThreadDebugListener listener) = 0;
    [[nodiscard]] virtual std::vector<ActorChildInfo> list_children() const = 0;
    [[nodiscard]] virtual std::vector<ActorThreadInfo> list_threads() const = 0;
    [[nodiscard]] virtual bool terminate_thread_by_index(int index) = 0;
    [[nodiscard]] virtual bool is_thread_alive(runtime::nanbox::LispVal socket) const = 0;
    [[nodiscard]] virtual int join_thread(runtime::nanbox::LispVal socket) = 0;

    virtual void notify_thread_started(void* vm, void* driver, std::string name) = 0;
    virtual void notify_thread_exited(void* vm = nullptr) = 0;

    [[nodiscard]] virtual void* native_handle() noexcept = 0;
    [[nodiscard]] virtual const void* native_handle() const noexcept = 0;
};

/**
 * @brief Abstract actor runtime service owned by session::Driver.
 */
class ActorRuntime {
public:
    using StreamSink = std::function<void(std::string_view)>;

    /**
     * @brief Actor lifecycle event payload exposed to front-ends.
     */
    struct ActorEvent {
        enum class Kind {
            Started,
            Exited,
        };
        Kind kind{Kind::Started};
        int index{-1};
        std::string name;
    };

    virtual ~ActorRuntime() = default;

    virtual bool install_mailbox(runtime::memory::heap::Heap& heap,
                                 const std::string& endpoint) = 0;

    virtual void install_worker_factories(const std::string& module_search_path,
                                          StreamSink stdout_sink = {},
                                          StreamSink stderr_sink = {}) = 0;

    virtual void on_actor_lifecycle(std::function<void(const ActorEvent&)> on_event) = 0;

    [[nodiscard]] virtual ActorProcessManager* process_manager() noexcept = 0;
    [[nodiscard]] virtual const ActorProcessManager* process_manager() const noexcept = 0;
    [[nodiscard]] virtual runtime::nanbox::LispVal mailbox() const noexcept = 0;
    [[nodiscard]] virtual runtime::nanbox::LispVal* mailbox_slot() noexcept = 0;
};

} // namespace eta::native
