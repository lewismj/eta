#pragma once

/**
 * @file all_primitives.h
 * @brief Single source of truth for ALL live primitive registrations.
 *
 * Every executable that runs Eta code must call register_all_primitives()
 * (which handles core + port + io + time + torch + stats), then call
 * eta::nng::register_nng_primitives() with driver-specific arguments.
 *
 * Canonical registration order  (MUST match builtin_names.h exactly):
 *   1. core_primitives.h
 *   2. port_primitives.h
 *   3. io_primitives.h
 *   4. time_primitives.h
 *   5. torch_primitives.h
 *   6. stats_primitives.h
 *   7. nng_primitives.h  (registered separately by the Driver)
 *
 * For analysis-only tools (LSP), builtin_names.h provides null-func
 */

#include "eta/runtime/core_primitives.h"
#include "eta/runtime/port_primitives.h"
#include "eta/runtime/io_primitives.h"
#include "eta/runtime/time_primitives.h"
#include <eta/torch/torch_primitives.h>
#include <eta/stats/stats_primitives.h>
/**
 * driver-specific arguments (ProcessManager, etai path, mailbox, etc.)
 * and must be called by the Driver after register_all_primitives().
 */

namespace eta::interpreter {

/**
 * Register all core+port+io+time+torch+stats primitive implementations.
 *
 * Call nng::register_nng_primitives() after this with the appropriate
 * driver-specific arguments to complete the full builtin set.
 */
inline void register_all_primitives(
    runtime::BuiltinEnvironment& env,
    runtime::memory::heap::Heap& heap,
    runtime::memory::intern::InternTable& intern,
    runtime::vm::VM& vm)
{
    /// Order MUST match builtin_names.h  (see canonical order above)
    runtime::register_core_primitives(env, heap, intern, &vm);
    runtime::register_port_primitives(env, heap, intern, vm);
    runtime::register_io_primitives(env, heap, intern, vm);
    runtime::register_time_primitives(env, heap, intern, &vm);
    torch_bindings::register_torch_primitives(env, heap, intern, &vm);
    stats_bindings::register_stats_primitives(env, heap, intern, &vm);
}

} ///< namespace eta::interpreter

