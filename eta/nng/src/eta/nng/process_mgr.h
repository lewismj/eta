#pragma once

/// @file process_mgr.h
/// @brief Process lifecycle manager for the Eta actor model (Phase 4).
///
/// Spawns child etai processes connected to the parent via nng PAIR sockets.
/// Each spawned child runs its own VM instance and communicates with the parent
/// exclusively through the PAIR socket returned by spawn().
///
/// Usage:
///   ProcessManager pm;
///   auto sock = pm.spawn("worker.eta", heap, intern, "/path/to/etai");
///   // send! / recv! via sock
///   pm.kill_child(sock);

#include <atomic>
#include <expected>
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

/// Manages child Eta processes spawned by the actor model.
///
/// Thread safety: all public methods are guarded by mu_.
/// nng sockets are thread-safe by default, so no additional locking
/// is needed for send!/recv! operations on the returned socket values.
class ProcessManager {
public:
    // ── ChildHandle ───────────────────────────────────────────────────────

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
        /// Uses kill(pid, 0) on POSIX — non-destructive, works on zombies too.
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

    // ── ChildInfo: summary for DAP / list view ────────────────────────────

    struct ChildInfo {
        int         pid{0};
        std::string endpoint;
        std::string module_path;
        bool        alive{false};
    };

    // ── Lifecycle ─────────────────────────────────────────────────────────

    ProcessManager() = default;

    /// Destructor: SIGTERM / TerminateProcess all remaining children.
    ~ProcessManager();

    // Non-copyable, non-movable (children_ holds OS handles)
    ProcessManager(const ProcessManager&)            = delete;
    ProcessManager& operator=(const ProcessManager&) = delete;

    // ── spawn ─────────────────────────────────────────────────────────────

    /// Spawn a child Eta process.
    ///
    /// 1. Generates a unique IPC endpoint.
    /// 2. Creates a parent-side PAIR socket and calls nng_listen().
    /// 3. Forks/creates the child: `etai <module_path> --mailbox <endpoint>`.
    /// 4. Stores the child handle and returns the boxed parent-side socket.
    ///
    /// @param module_path        Path to the .eta module to run in the child.
    /// @param heap               Heap for socket allocation.
    /// @param intern             Intern table (kept for API symmetry).
    /// @param etai_path          Full path to the etai executable.
    /// @param module_search_path Colon/semicolon-separated search path to
    ///                           forward to the child via ETA_MODULE_PATH.
    ///                           Only applied if ETA_MODULE_PATH is NOT already
    ///                           set in the environment (env var wins).
    std::expected<LispVal, RuntimeError>
    spawn(const std::string& module_path,
          Heap& heap, InternTable& intern,
          const std::string& etai_path,
          const std::string& module_search_path = {});

    // ── wait / kill ───────────────────────────────────────────────────────

    /// Block until the child associated with sock_val exits.
    /// Returns the exit code, or -1 if the socket handle is not found.
    int wait_for(LispVal sock_val);

    /// Send SIGTERM (POSIX) / TerminateProcess (Windows) to the child.
    /// Returns true if the signal was delivered.
    bool kill_child(LispVal sock_val);

    // ── inspection ───────────────────────────────────────────────────────

    /// Snapshot of all children for the DAP child process tree view.
    std::vector<ChildInfo> list_children() const;

    /// Mutex guarding children_ (exposed for Phase 7 thread safety).
    mutable std::mutex mu_;

private:
    std::vector<ChildHandle>  children_;
    std::atomic<int>          spawn_counter_{0};

    /// Return a pointer to the ChildHandle whose socket == sock_val.
    /// Caller MUST hold mu_.
    ChildHandle* find_by_socket(LispVal sock_val);

    /// Generate the next unique IPC endpoint string.
    std::string next_endpoint();
};

// ─── Inline implementation ────────────────────────────────────────────────────

inline ProcessManager::~ProcessManager() {
    std::lock_guard<std::mutex> lk(mu_);
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

    // ── 1. Generate a unique endpoint ─────────────────────────────────────
    std::string endpoint = next_endpoint();

    // ── 2. Create parent-side PAIR socket and listen ───────────────────────
    NngSocketPtr sp;
    sp.protocol = NngProtocol::Pair;
    int rv = nng_pair0_open(&sp.socket);
    if (rv != 0) {
        return std::unexpected(RuntimeError{VMError{
            RuntimeErrorCode::InternalError,
            "spawn: nng_pair0_open failed: " + std::string(nng_strerror(rv))}});
    }

    // Default recv timeout (1 s) so recv! on the parent doesn't block forever
    nng_socket_set_ms(sp.socket, NNG_OPT_RECVTIMEO, 1000);

    rv = nng_listen(sp.socket, endpoint.c_str(), nullptr, 0);
    if (rv != 0) {
        return std::unexpected(RuntimeError{VMError{
            RuntimeErrorCode::InternalError,
            "spawn: nng_listen failed on " + endpoint + ": " +
            std::string(nng_strerror(rv))}});
    }
    sp.listening = true;

    // ── 3. Allocate the socket on the Eta heap ─────────────────────────────
    auto sock_val_res = eta::nng::factory::make_nng_socket(heap, std::move(sp));
    if (!sock_val_res) return std::unexpected(sock_val_res.error());
    LispVal sock_val = *sock_val_res;

    // ── 4. Launch the child process ────────────────────────────────────────
    ChildHandle child;
    child.endpoint    = endpoint;
    child.module_path = module_path;
    child.socket      = sock_val;

#ifdef _WIN32
    // ── Windows ────────────────────────────────────────────────────────────
    // Propagate module search path via ETA_MODULE_PATH only if the variable
    // is not already set in the current environment.
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

    // Restore the environment variable state
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
    // ── POSIX ──────────────────────────────────────────────────────────────
    pid_t pid = ::fork();
    if (pid < 0) {
        return std::unexpected(RuntimeError{VMError{
            RuntimeErrorCode::InternalError,
            "spawn: fork() failed"}});
    }
    if (pid == 0) {
        // Child: propagate search path via ETA_MODULE_PATH only if not set.
        // setenv with overwrite=0 leaves an existing value untouched.
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

} // namespace eta::nng

