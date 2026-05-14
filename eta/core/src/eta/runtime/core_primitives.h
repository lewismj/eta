#pragma once

#include "eta/runtime/builtin_env.h"

namespace eta::runtime::vm {
class VM;
}

namespace eta::runtime {

/**
 * @brief Register all core primitives into a BuiltinEnvironment.
 *
 * @param env Builtin registration environment.
 * @param heap Runtime heap used by primitive closures.
 * @param intern_table Symbol/string interning table.
 * @param vm Optional running VM for primitives requiring VM services.
 */
void register_core_primitives(BuiltinEnvironment& env, Heap& heap, InternTable& intern_table,
                              vm::VM* vm = nullptr);

} ///< namespace eta::runtime
