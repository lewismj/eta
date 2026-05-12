#pragma once

#include <string>
#include <vector>

#include "eta/native/actor_runtime.h"
#include "eta/runtime/memory/heap.h"
#include "eta/runtime/memory/intern_table.h"
#include "eta/runtime/nanbox.h"
#include "eta/runtime/vm/vm.h"
#include "eta/semantics/emitter.h"

namespace eta::native {

/**
 * @brief Runtime object pointers exposed to sidecar entrypoints.
 *
 * Sidecars can use this context to build primitive callables that bind to the
 * active runtime instance without any core-side hardcoded sidecar activation.
 */
struct SidecarRuntimeBindingV1 {
    runtime::memory::heap::Heap* heap{nullptr};
    runtime::memory::intern::InternTable* intern_table{nullptr};
    runtime::vm::VM* vm{nullptr};
    semantics::BytecodeFunctionRegistry* function_registry{nullptr};
    const std::vector<runtime::nanbox::LispVal>* vm_globals{nullptr};
    runtime::nanbox::LispVal* mailbox_value{nullptr};
    const std::string* etai_path{nullptr};
    const std::string* module_search_path{nullptr};
    /**
     * @brief Generic actor process manager interface for sidecar consumers.
     */
    ActorProcessManager* actor_process_manager{nullptr};
    /**
     * @brief Legacy raw process manager handle kept for ABI-compatible sidecars.
     */
    void* process_manager{nullptr};
};

} // namespace eta::native
