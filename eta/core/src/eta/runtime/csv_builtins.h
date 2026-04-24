#pragma once

#include "eta/runtime/builtin_env.h"
#include "eta/runtime/memory/heap.h"
#include "eta/runtime/memory/intern_table.h"

namespace eta::runtime {

    /**
     * @brief Register CSV-related runtime primitives.
     *
     * Registers `%csv-*` builtins backed by `vincentlaucsb/csv-parser`.
     */
    void register_csv_builtins(BuiltinEnvironment& env,
                               memory::heap::Heap& heap,
                               memory::intern::InternTable& intern_table);

} ///< namespace eta::runtime

