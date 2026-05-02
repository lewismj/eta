#pragma once

#include <atomic>
#include <memory>

#include "eta/runtime/nanbox.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <sys/types.h>
#endif

namespace eta::runtime::types {

/**
 * @brief Native subprocess handle tracked by the Eta heap.
 *
 * The object stores lifecycle state and optional stdio port references.
 * Native process resources are released in the destructor without blocking.
 */
struct ProcessHandle {
#ifdef _WIN32
    HANDLE process_handle{nullptr};
    DWORD pid{0};
#else
    pid_t pid{-1};
#endif

    std::atomic<bool> running{true};
    std::atomic<bool> waited{false};
    std::atomic<int> exit_code{-1};

    LispVal stdin_port{nanbox::Nil};
    LispVal stdout_port{nanbox::Nil};
    LispVal stderr_port{nanbox::Nil};

    ~ProcessHandle() {
#ifdef _WIN32
        if (process_handle != nullptr && process_handle != INVALID_HANDLE_VALUE) {
            ::CloseHandle(process_handle);
            process_handle = nullptr;
        }
#endif
    }
};

/**
 * @brief Heap wrapper for shared subprocess lifecycle state.
 */
struct ProcessHandleObject {
    std::shared_ptr<ProcessHandle> handle;
};

} ///< namespace eta::runtime::types
