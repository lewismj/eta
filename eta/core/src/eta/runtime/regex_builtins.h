#pragma once

#include "eta/runtime/builtin_env.h"
#include "eta/runtime/memory/heap.h"
#include "eta/runtime/memory/intern_table.h"
#include "eta/runtime/vm/vm.h"

namespace eta::runtime {

    /**
     * @brief Register regex-related runtime primitives.
     *
     * Registers `%regex-*` builtins backed by `std::regex`.
     */
    void register_regex_builtins(BuiltinEnvironment& env,
                                 memory::heap::Heap& heap,
                                 memory::intern::InternTable& intern_table,
                                 vm::VM* vm);

} ///< namespace eta::runtime
