#pragma once

#include "eta/runtime/builtin_env.h"
#include "eta/runtime/memory/heap.h"
#include "eta/runtime/memory/intern_table.h"
#include "eta/runtime/vm/vm.h"

namespace eta::runtime {

/**
 * @brief Register subprocess/runtime process primitives.
 *
 * Registers:
 *   - %process-run
 *   - %process-spawn
 *   - %process-wait
 *   - %process-kill
 *   - %process-terminate
 *   - %process-pid
 *   - %process-alive?
 *   - %process-exit-code
 *   - %process-handle?
 *   - %process-stdin-port
 *   - %process-stdout-port
 *   - %process-stderr-port
 */
void register_process_primitives(BuiltinEnvironment& env,
                                 memory::heap::Heap& heap,
                                 memory::intern::InternTable& intern_table,
                                 vm::VM& vm);

} // namespace eta::runtime
