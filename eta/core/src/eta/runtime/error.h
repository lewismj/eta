#pragma once

#include <cstdint>
#include <string>
#include <variant>
#include <vector>

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
        SandboxViolation, ///< mutating opcode/primitive attempted while VM is in sandbox mode
    };

    struct VMErrorField {
        std::string key;
        std::variant<int64_t, double, std::string> value;
    };

    struct VMError {
        RuntimeErrorCode code;
        std::string message;
        std::string tag_override{};
        std::vector<VMErrorField> fields{};
    };

    //! Internal runtime error, the Compiler/VM can add Span information.
    using RuntimeError = std::variant<NaNBoxError, HeapError, InternTableError, VMError>;

    inline RuntimeError make_tagged_error(
        std::string tag,
        std::string message,
        std::vector<VMErrorField> fields = {}) {
        VMError err{RuntimeErrorCode::UserError, std::move(message)};
        err.tag_override = std::move(tag);
        err.fields = std::move(fields);
        return RuntimeError{std::move(err)};
    }

}
