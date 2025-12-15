#pragma once

#include <variant>

#include "nanbox.h"
#include "memory/heap.h"
#include "memory/intern_table.h"

namespace eta::runtime::error {

    using namespace eta::runtime::nanbox;
    using namespace eta::runtime::memory::heap;
    using namespace eta::runtime::memory::intern;

    //! Internal runtime error, the Compiler/VM can add Span information.
    using RuntimeError = std::variant<NaNBoxError, HeapError, InternTableError>;

}