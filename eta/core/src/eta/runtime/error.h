#pragma once

#include <string>
#include <variant>

#include "nanbox.h"
#include "memory/heap.h"
#include "memory/intern_table.h"

namespace eta::runtime::error {

    using namespace eta::runtime::nanbox;
    using namespace eta::runtime::memory::heap;
    using namespace eta::runtime::memory::intern;

    enum class RuntimeErrorCode : std::uint8_t {
        NotImplemented,
        InternalError,
        StackOverflow,
        StackUnderflow,
        FrameOverflow,
        InvalidInstruction,
        InvalidArity,
        TypeError,
        UndefinedGlobal,
        UserError,
        UserThrow,   ///< unhandled (raise tag value) with no matching catch frame
    };

    struct VMError {
        RuntimeErrorCode code;
        std::string message;
    };

    //! Internal runtime error, the Compiler/VM can add Span information.
    using RuntimeError = std::variant<NaNBoxError, HeapError, InternTableError, VMError>;

}