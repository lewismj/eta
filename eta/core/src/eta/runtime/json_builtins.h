#pragma once

#include "eta/runtime/builtin_env.h"
#include "eta/runtime/memory/heap.h"
#include "eta/runtime/memory/intern_table.h"

namespace eta::runtime {

/**
 * @brief Register JSON runtime primitives.
 *
 * Registers:
 *   - %json-read
 *   - %json-read-string
 *   - %json-write
 *   - %json-write-string
 */
void register_json_builtins(BuiltinEnvironment& env,
                            memory::heap::Heap& heap,
                            memory::intern::InternTable& intern_table);

} // namespace eta::runtime

