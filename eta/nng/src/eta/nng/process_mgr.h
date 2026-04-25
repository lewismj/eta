#pragma once

/**
 * @file process_mgr.h
 * @brief Process lifecycle manager for the Eta actor model.
 *
 * Spawns child etai OS-processes connected via nng PAIR sockets.
 * Can also spawn in-process actor threads, each with an independent VM,
 *          communicating via nng inproc:// PAIR sockets.
 *
 *   ProcessManager pm;
 *   auto sock = pm.spawn("worker.eta", heap, intern, "/path/to/etai");
 *   pm.kill_child(sock);
 *
 *   ProcessManager pm;
 *   pm.set_worker_factory(my_factory);
 *   auto sock = pm.spawn_thread_with("worker.eta","my-fn",{}, heap, intern);
 *   pm.join_thread(sock);
 */

#include <atomic>
#include <expected>
#include <functional>
#include <future>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#else
#  include <csignal>
#  include <sys/wait.h>
#  include <unistd.h>
#endif

#include <nng/nng.h>
#include <nng/protocol/pair0/pair.h>

#include <eta/runtime/nanbox.h>
#include <eta/runtime/error.h>
#include <eta/runtime/memory/heap.h>
#include <eta/runtime/memory/intern_table.h>

#include "nng_socket_ptr.h"
#include "nng_factory.h"

namespace eta::nng {

using namespace eta::runtime;
using namespace eta::runtime::nanbox;
using namespace eta::runtime::error;
using namespace eta::runtime::memory::heap;
using namespace eta::runtime::memory::intern;
using namespace eta::runtime::memory::factory;

/**
 * Manages child Eta processes spawned by the actor model.
 *
 * Thread safety: all public methods are guarded by mu_.
 * nng sockets are thread-safe by default, so no additional locking
 * is needed for send!/recv! operations on the returned socket values.
 */
class ProcessManager {
public:
    /// ChildHandle

    struct ChildHandle {
#ifdef _WIN32
        HANDLE  process_handle{INVALID_HANDLE_VALUE};
        DWORD   process_id{0};
#else
        pid_t   process_id{-1};
#endif
        std::string endpoint;     ///< nng IPC endpoint used for this child
        std::string module_path;  ///< .eta file loaded by the child
        LispVal socket{Nil};      ///< Parent-side PAIR socket (boxed heap object)

        /// Check whether the process is still running.
        bool is_alive() const {
#ifdef _WIN32
            if (process_handle == INVALID_HANDLE_VALUE) return false;
            DWORD code = STILL_ACTIVE;
            GetExitCodeProcess(process_handle, &code);
            return code == STILL_ACTIVE;
#else
            if (process_id <= 0) return false;
            return ::kill(process_id, 0) == 0;
#endif
        }
    };

    /// ChildInfo: summary for DAP / list view

    struct ChildInfo {
        int         pid{0};
        std::string endpoint;
        std::string module_path;
        bool        alive{false};
    };


    /**
     * Signature of the factory that runs inside the new thread.
     * Responsible for: dialing endpoint, loading the module, calling the
     * function with text_args, signalling alive=false when done.
     */
    using ThreadWorkerFn = std::function<void(
        const std::string& module_path,   ///< .eta file to load
        const std::string& func_name,     ///< top-level function to call
        const std::string& endpoint,      ///< inproc:// endpoint to dial
        std::vector<std::string> text_args, ///< s-expression text arguments
        std::shared_ptr<std::atomic<bool>> alive ///< set false when done
    )>;

    struct ThreadHandle {
        std::thread thread;
        std::string endpoint;
        std::string module_path;
        std::string func_name;
        LispVal     socket{Nil};
        Heap*       heap{nullptr}; ///< owning heap for socket lookup/close
        std::shared_ptr<std::atomic<bool>> alive_;

        ThreadHandle() : alive_(std::make_shared<std::atomic<bool>>(true)) {}
        ThreadHandle(ThreadHandle&&) = default;
        ThreadHandle& operator=(ThreadHandle&&) = default;
        ThreadHandle(const ThreadHandle&) = delete;
        ThreadHandle& operator=(const ThreadHandle&) = delete;
    };

    /**
     * Serialized closure for spawn-thread.
     * Contains the closure's bytecode (etac-format, 0-based) plus
     * serialized captures (upvalues + referenced globals) for transfer to a
     * new in-process thread.
     */
    struct SerializedClosure {
        std::vector<uint8_t> funcs_bytes;      ///< etac-format bytecode (entry at index 0)
        std::vector<uint8_t> captures_bytes;   ///< closure/module capture payload
    };

    /// Factory for spawn-thread: receives endpoint + serialized closure, dials, executes.
    using ClosureWorkerFn = std::function<void(
        const std::string& endpoint,        ///< inproc:// endpoint to dial
        SerializedClosure sc,               ///< serialized closure to execute
        std::shared_ptr<std::atomic<bool>> alive)>;

    struct ThreadInfo {
        int         index{0};
        std::string endpoint;
        std::string module_path;
        std::string func_name;
        bool        alive{false};
    };

    /**
     * Debug listener: invoked by the Driver-supplied worker lambda after a
     * child VM/Driver have been constructed (Started) and just before the
     * worker thread exits (Exited).  Used by the DAP server to attach a
     * per-thread stop callback and to route per-threadId requests to the
     * correct child VM.
     *
     * `vm` and `driver` are opaque pointers (so this header has no
     * dependency on interpreter::Driver / runtime::vm::VM).  The DAP server
     * casts them back to the concrete types.  Pointers are only valid
     * between the matching Started and Exited events.
     */
    struct ThreadDebugEvent {
        enum class Kind { Started, Exited };
        Kind        kind{Kind::Started};
        int         index{-1};            ///< process-manager-local thread index
        void*       vm{nullptr};          ///< runtime::vm::VM*  (Started only)
        void*       driver{nullptr};      ///< interpreter::Driver* (Started only)
        std::string name;                 ///< friendly name (Started only)
    };
    using ThreadDebugListener = std::function<void(const ThreadDebugEvent&)>;

    /// Install / replace the debug listener.  May be called from any thread.
    void set_debug_listener(ThreadDebugListener fn) {
        std::lock_guard<std::mutex> lk(mu_);
        debug_listener_ = std::move(fn);
    }

    /// Returns a copy of the current debug listener (may be empty).
    ThreadDebugListener debug_listener_copy() const {
        std::lock_guard<std::mutex> lk(mu_);
        return debug_listener_;
    }

    /**
     * Called by the Driver-supplied worker lambda from inside the spawned
     * thread, *after* it has constructed the child VM/Driver.
     *
     * Looks up the current thread's process-manager index from a thread-local
     * cookie installed by spawn_thread_with / spawn_thread, then invokes the
     * debug listener (if any) with a Started event.  Safe to call when no
     * listener is installed.
     */
    void notify_thread_started(void* vm, void* driver, std::string name) {
        ThreadDebugListener cb = debug_listener_copy();
        if (!cb) return;
        ThreadDebugEvent ev;
        ev.kind   = ThreadDebugEvent::Kind::Started;
        ev.index  = current_thread_index();
        ev.vm     = vm;
        ev.driver = driver;
        ev.name   = std::move(name);
        try { cb(ev); } catch (...) {}
    }

    /// Counterpart of notify_thread_started, called just before worker exit.
    void notify_thread_exited(void* vm = nullptr) {
        ThreadDebugListener cb = debug_listener_copy();
        if (!cb) return;
        ThreadDebugEvent ev;
        ev.kind  = ThreadDebugEvent::Kind::Exited;
        ev.index = current_thread_index();
        ev.vm    = vm;
        try { cb(ev); } catch (...) {}
    }

    /// Returns the process-manager index for the *current* worker thread, or -1.
    static int current_thread_index() noexcept { return current_thread_index_tls_(); }

    /// Lifecycle

    ProcessManager() = default;

    /// Destructor: SIGTERM / TerminateProcess all remaining children.
    ~ProcessManager();

    /// Non-copyable, non-movable (children_ holds OS handles)
    ProcessManager(const ProcessManager&)            = delete;
    ProcessManager& operator=(const ProcessManager&) = delete;

    /// spawn

    /**
     * Spawn a child Eta process.
     *
     * 1. Generates a unique IPC endpoint.
     * 2. Creates a parent-side PAIR socket and calls nng_listen().
     * 3. Forks/creates the child: `etai <module_path> --mailbox <endpoint>`.
     * 4. Stores the child handle and returns the boxed parent-side socket.
     *
     * @param module_path        Path to the .eta module to run in the child.
     * @param heap               Heap for socket allocation.
     * @param intern             Intern table (kept for API symmetry).
     * @param etai_path          Full path to the etai executable.
     * @param module_search_path Colon/semicolon-separated search path to
     *                           forward to the child via ETA_MODULE_PATH.
     *                           Only applied if ETA_MODULE_PATH is NOT already
     *                           set in the environment (env var wins).
     */
    std::expected<LispVal, RuntimeError>
    spawn(const std::string& module_path,
          Heap& heap, InternTable& intern,
          const std::string& etai_path,
          const std::string& module_search_path = {});

    /// wait / kill

    /**
     * Block until the child associated with sock_val exits.
     * Returns the exit code, or -1 if the socket handle is not found.
     */
    int wait_for(LispVal sock_val);

    /**
     * Send SIGTERM (POSIX) / TerminateProcess (Windows) to the child.
     * Returns true if the signal was delivered.
     */
    bool kill_child(LispVal sock_val);

    /// inspection

    /// Snapshot of all children for the DAP child process tree view.
    std::vector<ChildInfo> list_children() const;


    /**
     * Install the factory that runs inside each spawned thread.
     * Must be called before spawn_thread_with().
     */
    void set_worker_factory(ThreadWorkerFn fn) {
        std::lock_guard<std::mutex> lk(mu_);
        worker_factory_ = std::move(fn);
    }

    /**
     * Install the factory for spawn-thread.
     * Must be called before spawn_thread().
     */
    void set_closure_factory(ClosureWorkerFn fn) {
        std::lock_guard<std::mutex> lk(mu_);
        closure_worker_factory_ = std::move(fn);
    }

    /**
     * Spawn an in-process actor thread.
     *
     * 1. Generates a unique inproc:// endpoint.
     * 2. Creates a parent-side PAIR socket and calls nng_listen().
     * 3. Launches a std::thread running worker_factory_ with the given args.
     * 4. Returns the boxed parent-side socket.
     */
    std::expected<LispVal, RuntimeError>
    spawn_thread_with(const std::string& module_path,
                      const std::string& func_name,
                      std::vector<std::string> text_args,
                      Heap& heap, InternTable& intern);

    /**
     * Spawn an in-process actor thread from a serialized closure.
     *
     * 1. Generates a unique inproc:// endpoint.
     * 2. Creates a parent-side PAIR socket and calls nng_listen().
     * 3. Launches a std::thread running closure_worker_factory_ with @p sc.
     * 4. Returns the boxed parent-side socket.
     */
    std::expected<LispVal, RuntimeError>
    spawn_thread(SerializedClosure sc, Heap& heap, InternTable& intern);

    /**
     * Block until the actor thread associated with sock_val completes.
     * Returns 0 on success, -1 if not found or already joined.
     */
    int join_thread(LispVal sock_val);

    /// Check if the actor thread associated with sock_val is still running.
    bool is_thread_alive(LispVal sock_val) const;

    /// Snapshot of all actor threads for DAP display.
    std::vector<ThreadInfo> list_threads() const;

    /**
     * Best-effort termination for an in-process actor thread by index from
     * list_threads(). This closes the parent-side mailbox socket and detaches
     * any joinable std::thread so the caller is not blocked.
     *
     * Returns true if a termination action was attempted.
     */
    bool terminate_thread_by_index(int index);

    /// Mutex guarding children_
    mutable std::mutex mu_;

private:
    std::vector<ChildHandle>  children_;
    std::atomic<int>          spawn_counter_{0};

    std::vector<ThreadHandle> threads_;
    ThreadWorkerFn            worker_factory_;
    ClosureWorkerFn           closure_worker_factory_;
    ThreadDebugListener       debug_listener_;

    /**
     * Thread-local cookie holding the process-manager index of the *current*
     * worker thread.  Populated by the thread wrapper in spawn_thread_with /
     * spawn_thread before invoking the user-supplied factory; read by
     * current_thread_index() / notify_thread_*.
     */
    static int& current_thread_index_tls_() noexcept {
        thread_local int idx = -1;
        return idx;
    }

    /**
     * Return a pointer to the ChildHandle whose socket == sock_val.
     * Caller MUST hold mu_.
     */
    ChildHandle* find_by_socket(LispVal sock_val);

    /**
     * Return a pointer to the ThreadHandle whose socket == sock_val.
     * Caller MUST hold mu_.
     */
    ThreadHandle* find_thread_by_socket(LispVal sock_val);

    /// Generate the next unique IPC endpoint string (for OS processes).
    std::string next_endpoint();

    /// Generate the next unique inproc endpoint string (for threads).
    std::string next_inproc_endpoint();
};

/// Inline implementation

inline ProcessManager::~ProcessManager() {
    std::lock_guard<std::mutex> lk(mu_);
    /// Kill / reap child processes
    for (auto& ch : children_) {
        if (!ch.is_alive()) continue;
#ifdef _WIN32
        TerminateProcess(ch.process_handle, 1);
        WaitForSingleObject(ch.process_handle, 2000);
        CloseHandle(ch.process_handle);
        ch.process_handle = INVALID_HANDLE_VALUE;
#else
        ::kill(ch.process_id, SIGTERM);
        int st;
        ::waitpid(ch.process_id, &st, 0);
        ch.process_id = -1;
#endif
    }
#ifdef _WIN32
    for (auto& ch : children_) {
        if (ch.process_handle != INVALID_HANDLE_VALUE) {
            CloseHandle(ch.process_handle);
            ch.process_handle = INVALID_HANDLE_VALUE;
        }
    }
#endif
    /// Detach actor threads (don't block on potentially-blocked recv!)
    for (auto& th : threads_) {
        if (th.thread.joinable()) th.thread.detach();
    }
}

inline std::string ProcessManager::next_endpoint() {
    int counter = spawn_counter_.fetch_add(1);
#ifdef _WIN32
    return "ipc://eta-" + std::to_string(static_cast<int>(GetCurrentProcessId()))
           + "-" + std::to_string(counter);
#else
    return "ipc:///tmp/eta-" + std::to_string(static_cast<int>(::getpid()))
           + "-" + std::to_string(counter) + ".sock";
#endif
}

inline std::string ProcessManager::next_inproc_endpoint() {
    int counter = spawn_counter_.fetch_add(1);
    return "inproc://eta-thread-" + std::to_string(counter);
}

inline ProcessManager::ChildHandle* ProcessManager::find_by_socket(LispVal sock_val) {
    for (auto& ch : children_) {
        if (ch.socket == sock_val) return &ch;
    }
    return nullptr;
}

inline std::expected<LispVal, RuntimeError>
ProcessManager::spawn(const std::string& module_path,
                      Heap& heap, InternTable& /*intern*/,
                      const std::string& etai_path,
                      const std::string& module_search_path)
{
    if (etai_path.empty()) {
        return std::unexpected(RuntimeError{VMError{
            RuntimeErrorCode::InternalError,
            "spawn: etai executable path is not configured"}});
    }

    /// 1. Generate a unique endpoint
    std::string endpoint = next_endpoint();

    /// 2. Create parent-side PAIR socket and listen
    NngSocketPtr sp;
    sp.protocol = NngProtocol::Pair;
    int rv = nng_pair0_open(&sp.socket);
    if (rv != 0) {
        return std::unexpected(RuntimeError{VMError{
            RuntimeErrorCode::InternalError,
            "spawn: nng_pair0_open failed: " + std::string(nng_strerror(rv))}});
    }

    /// Default recv timeout (1 s) so recv! on the parent doesn't block forever
    nng_socket_set_ms(sp.socket, NNG_OPT_RECVTIMEO, 1000);

    rv = nng_listen(sp.socket, endpoint.c_str(), nullptr, 0);
    if (rv != 0) {
        return std::unexpected(RuntimeError{VMError{
            RuntimeErrorCode::InternalError,
            "spawn: nng_listen failed on " + endpoint + ": " +
            std::string(nng_strerror(rv))}});
    }
    sp.listening = true;

    /// 3. Allocate the socket on the Eta heap
    NngSocketPtr sp_tmp;
    sp_tmp = std::move(sp);
    sp_tmp.endpoint_hint = endpoint;  ///< store endpoint for supervision down-messages
    auto sock_val_res = eta::nng::factory::make_nng_socket(heap, std::move(sp_tmp));
    if (!sock_val_res) return std::unexpected(sock_val_res.error());
    LispVal sock_val = *sock_val_res;

    /// 4. Launch the child process
    ChildHandle child;
    child.endpoint    = endpoint;
    child.module_path = module_path;
    child.socket      = sock_val;

#ifdef _WIN32
    /**
     * Windows
     * Propagate module search path via ETA_MODULE_PATH only if the variable
     * is not already set in the current environment.
     */
    bool set_env_var = false;
    if (!module_search_path.empty()) {
        char existing[32767];
        DWORD len = GetEnvironmentVariableA("ETA_MODULE_PATH", existing,
                                             static_cast<DWORD>(sizeof(existing)));
        if (len == 0 && GetLastError() == ERROR_ENVVAR_NOT_FOUND) {
            SetEnvironmentVariableA("ETA_MODULE_PATH", module_search_path.c_str());
            set_env_var = true;
        }
    }

    std::string cmdline =
        "\"" + etai_path + "\" \"" + module_path +
        "\" --mailbox \"" + endpoint + "\"";

    STARTUPINFOA si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};

    bool created = CreateProcessA(
        nullptr,
        const_cast<char*>(cmdline.c_str()),
        nullptr, nullptr, FALSE,
        0, nullptr, nullptr,
        &si, &pi);

    /// Restore the environment variable state
    if (set_env_var) SetEnvironmentVariableA("ETA_MODULE_PATH", nullptr);

    if (!created) {
        return std::unexpected(RuntimeError{VMError{
            RuntimeErrorCode::InternalError,
            "spawn: CreateProcess failed (error " +
            std::to_string(static_cast<int>(GetLastError())) + ")"}});
    }
    CloseHandle(pi.hThread);
    child.process_handle = pi.hProcess;
    child.process_id     = pi.dwProcessId;
#else
    /// POSIX
    pid_t pid = ::fork();
    if (pid < 0) {
        return std::unexpected(RuntimeError{VMError{
            RuntimeErrorCode::InternalError,
            "spawn: fork() failed"}});
    }
    if (pid == 0) {
        /**
         * Child: propagate search path via ETA_MODULE_PATH only if not set.
         * setenv with overwrite=0 leaves an existing value untouched.
         */
        if (!module_search_path.empty()) {
            ::setenv("ETA_MODULE_PATH", module_search_path.c_str(), 0);
        }

        const char* args[] = {
            etai_path.c_str(),
            module_path.c_str(),
            "--mailbox",
            endpoint.c_str(),
            nullptr
        };
        ::execv(etai_path.c_str(), const_cast<char**>(args));
        ::_exit(127);
    }
    child.process_id = pid;
#endif

    {
        std::lock_guard<std::mutex> lk(mu_);
        children_.push_back(std::move(child));
    }

    return sock_val;
}

inline int ProcessManager::wait_for(LispVal sock_val) {
    std::lock_guard<std::mutex> lk(mu_);
    auto* ch = find_by_socket(sock_val);
    if (!ch) return -1;

#ifdef _WIN32
    if (ch->process_handle == INVALID_HANDLE_VALUE) return -1;
    WaitForSingleObject(ch->process_handle, INFINITE);
    DWORD code = static_cast<DWORD>(-1);
    GetExitCodeProcess(ch->process_handle, &code);
    CloseHandle(ch->process_handle);
    ch->process_handle = INVALID_HANDLE_VALUE;
    return static_cast<int>(code);
#else
    if (ch->process_id <= 0) return -1;
    int status = 0;
    pid_t r = ::waitpid(ch->process_id, &status, 0);
    if (r <= 0) return -1;
    ch->process_id = -1;
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
#endif
}

inline bool ProcessManager::kill_child(LispVal sock_val) {
    std::lock_guard<std::mutex> lk(mu_);
    auto* ch = find_by_socket(sock_val);
    if (!ch) return false;

#ifdef _WIN32
    if (ch->process_handle == INVALID_HANDLE_VALUE) return false;
    bool ok = (TerminateProcess(ch->process_handle, 1) != 0);
    if (ok) {
        WaitForSingleObject(ch->process_handle, 5000);
        CloseHandle(ch->process_handle);
        ch->process_handle = INVALID_HANDLE_VALUE;
    }
    return ok;
#else
    if (ch->process_id <= 0) return false;
    int r = ::kill(ch->process_id, SIGTERM);
    if (r == 0) {
        int st;
        ::waitpid(ch->process_id, &st, 0);
        ch->process_id = -1;
    }
    return r == 0;
#endif
}

inline std::vector<ProcessManager::ChildInfo> ProcessManager::list_children() const {
    std::lock_guard<std::mutex> lk(mu_);
    std::vector<ChildInfo> result;
    result.reserve(children_.size());
    for (const auto& ch : children_) {
        ChildInfo ci;
#ifdef _WIN32
        ci.pid = static_cast<int>(ch.process_id);
#else
        ci.pid = static_cast<int>(ch.process_id);
#endif
        ci.endpoint    = ch.endpoint;
        ci.module_path = ch.module_path;
        ci.alive       = ch.is_alive();
        result.push_back(std::move(ci));
    }
    return result;
}

/// In-process thread implementation

inline ProcessManager::ThreadHandle*
ProcessManager::find_thread_by_socket(LispVal sock_val) {
    for (auto& th : threads_) {
        if (th.socket == sock_val) return &th;
    }
    return nullptr;
}

inline std::expected<LispVal, RuntimeError>
ProcessManager::spawn_thread_with(const std::string& module_path,
                                   const std::string& func_name,
                                   std::vector<std::string> text_args,
                                   Heap& heap, InternTable& /*intern*/)
{
    std::lock_guard<std::mutex> lk(mu_);

    if (!worker_factory_) {
        return std::unexpected(RuntimeError{VMError{
            RuntimeErrorCode::InternalError,
            "spawn-thread-with: no thread worker factory configured "
            "(Driver was not set up for in-process threads)"}});
    }

    /// 1. Generate a unique inproc endpoint
    std::string endpoint = next_inproc_endpoint();

    /// 2. Create parent-side PAIR socket and listen
    NngSocketPtr sp;
    sp.protocol = NngProtocol::Pair;
    int rv = nng_pair0_open(&sp.socket);
    if (rv != 0) {
        return std::unexpected(RuntimeError{VMError{
            RuntimeErrorCode::InternalError,
            "spawn-thread-with: nng_pair0_open failed: " +
            std::string(nng_strerror(rv))}});
    }
    nng_socket_set_ms(sp.socket, NNG_OPT_RECVTIMEO, 1000);

    rv = nng_listen(sp.socket, endpoint.c_str(), nullptr, 0);
    if (rv != 0) {
        return std::unexpected(RuntimeError{VMError{
            RuntimeErrorCode::InternalError,
            "spawn-thread-with: nng_listen failed on " + endpoint + ": " +
            std::string(nng_strerror(rv))}});
    }
    sp.listening = true;
    sp.endpoint_hint = endpoint;  ///< store endpoint for supervision down-messages

    /// 3. Allocate socket on the Eta heap
    auto sock_val_res = eta::nng::factory::make_nng_socket(heap, std::move(sp));
    if (!sock_val_res) return std::unexpected(sock_val_res.error());
    LispVal sock_val = *sock_val_res;

    /// 4. Build and launch the actor thread
    ThreadHandle th;
    th.endpoint    = endpoint;
    th.module_path = module_path;
    th.func_name   = func_name;
    th.socket      = sock_val;
    th.heap        = &heap;

    auto alive_ptr = th.alive_;  ///< shared with the thread
    ThreadWorkerFn factory_copy = worker_factory_;

    const int spawned_index = static_cast<int>(threads_.size());
    th.thread = std::thread([factory_copy, module_path, func_name,
                              endpoint, text_args, alive_ptr, spawned_index]() mutable {
        current_thread_index_tls_() = spawned_index;
        try {
            factory_copy(module_path, func_name, endpoint,
                         std::move(text_args), alive_ptr);
        } catch (...) {}
        alive_ptr->store(false, std::memory_order_release);
    });

    threads_.push_back(std::move(th));
    return sock_val;
}

inline std::expected<LispVal, RuntimeError>
ProcessManager::spawn_thread(SerializedClosure sc, Heap& heap, InternTable& /*intern*/)
{
    std::lock_guard<std::mutex> lk(mu_);

    if (!closure_worker_factory_) {
        return std::unexpected(RuntimeError{VMError{
            RuntimeErrorCode::InternalError,
            "spawn-thread: no closure worker factory configured "
            "(Driver was not set up for in-process threads)"}});
    }

    /// 1. Generate a unique inproc endpoint
    std::string endpoint = next_inproc_endpoint();

    /// 2. Create parent-side PAIR socket and listen
    NngSocketPtr sp;
    sp.protocol = NngProtocol::Pair;
    int rv = nng_pair0_open(&sp.socket);
    if (rv != 0) {
        return std::unexpected(RuntimeError{VMError{
            RuntimeErrorCode::InternalError,
            "spawn-thread: nng_pair0_open failed: " +
            std::string(nng_strerror(rv))}});
    }
    nng_socket_set_ms(sp.socket, NNG_OPT_RECVTIMEO, 1000);

    rv = nng_listen(sp.socket, endpoint.c_str(), nullptr, 0);
    if (rv != 0) {
        return std::unexpected(RuntimeError{VMError{
            RuntimeErrorCode::InternalError,
            "spawn-thread: nng_listen failed on " + endpoint + ": " +
            std::string(nng_strerror(rv))}});
    }
    sp.listening = true;
    sp.endpoint_hint = endpoint;

    /// 3. Allocate socket on the Eta heap
    auto sock_val_res = eta::nng::factory::make_nng_socket(heap, std::move(sp));
    if (!sock_val_res) return std::unexpected(sock_val_res.error());
    LispVal sock_val = *sock_val_res;

    /// 4. Build and launch the actor thread
    ThreadHandle th;
    th.endpoint    = endpoint;
    th.module_path = "(spawn-thread)";
    th.func_name   = "(thunk)";
    th.socket      = sock_val;
    th.heap        = &heap;

    auto alive_ptr = th.alive_;
    ClosureWorkerFn factory_copy = closure_worker_factory_;

    const int spawned_index = static_cast<int>(threads_.size());
    th.thread = std::thread([factory_copy, endpoint, sc = std::move(sc), alive_ptr, spawned_index]() mutable {
        current_thread_index_tls_() = spawned_index;
        try {
            factory_copy(endpoint, std::move(sc), alive_ptr);
        } catch (...) {}
        alive_ptr->store(false, std::memory_order_release);
    });

    threads_.push_back(std::move(th));
    return sock_val;
}

inline int ProcessManager::join_thread(LispVal sock_val) {
    std::unique_lock<std::mutex> lk(mu_);
    auto* th = find_thread_by_socket(sock_val);
    if (!th) return -1;
    if (!th->thread.joinable()) return 0;
    /// Release lock while joining so other threads can proceed
    std::thread t = std::move(th->thread);
    lk.unlock();
    t.join();
    return 0;
}

inline bool ProcessManager::is_thread_alive(LispVal sock_val) const {
    std::lock_guard<std::mutex> lk(mu_);
    for (const auto& th : threads_) {
        if (th.socket == sock_val) {
            return th.alive_ && th.alive_->load(std::memory_order_acquire);
        }
    }
    return false;
}

inline std::vector<ProcessManager::ThreadInfo>
ProcessManager::list_threads() const {
    std::lock_guard<std::mutex> lk(mu_);
    std::vector<ThreadInfo> result;
    result.reserve(threads_.size());
    int idx = 0;
    for (const auto& th : threads_) {
        ThreadInfo ti;
        ti.index       = idx++;
        ti.endpoint    = th.endpoint;
        ti.module_path = th.module_path;
        ti.func_name   = th.func_name;
        ti.alive       = th.alive_ && th.alive_->load(std::memory_order_acquire);
        result.push_back(std::move(ti));
    }
    return result;
}

inline bool ProcessManager::terminate_thread_by_index(int index) {
    std::lock_guard<std::mutex> lk(mu_);
    if (index < 0 || static_cast<std::size_t>(index) >= threads_.size()) return false;

    auto& th = threads_[static_cast<std::size_t>(index)];
    bool attempted = false;

    if (th.heap &&
        ops::is_boxed(th.socket) &&
        ops::tag(th.socket) == Tag::HeapObject) {
        auto* sp = th.heap->try_get_as<ObjectKind::NngSocket, NngSocketPtr>(ops::payload(th.socket));
        if (sp && !sp->closed) {
            if (sp->monitor_state) {
                sp->monitor_state->closing_normally.store(true, std::memory_order_release);
                if (sp->monitor_state->heartbeat) {
                    sp->monitor_state->heartbeat->stop.store(true, std::memory_order_release);
                }
            }
            nng_close(sp->socket);
            sp->closed = true;
            attempted = true;
        }
    }

    if (th.thread.joinable()) {
        th.thread.detach();
        attempted = true;
    }

    return attempted;
}

} ///< namespace eta::nng

