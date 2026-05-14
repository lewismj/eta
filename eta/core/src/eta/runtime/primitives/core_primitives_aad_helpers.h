#pragma once

#include <expected>
#include <string>

#include "eta/runtime/core_primitives_internal.h"
#include "eta/runtime/types/tape.h"
#include "eta/runtime/types/tape_ref.h"

namespace eta::runtime::detail::core_primitives_aad {

/**
 * @brief Validate that a primitive argument is a tape object.
 */
inline std::expected<types::Tape*, RuntimeError> expect_tape_arg(
    Heap& heap,
    LispVal value,
    const char* op_name,
    const char* role) {
    if (!ops::is_boxed(value) || ops::tag(value) != Tag::HeapObject) {
        return std::unexpected(RuntimeError{VMError{
            RuntimeErrorCode::TypeError,
            std::string(op_name) + ": " + role + " must be a tape"}});
    }

    auto* tape = heap.try_get_as<ObjectKind::Tape, types::Tape>(ops::payload(value));
    if (!tape) {
        return std::unexpected(RuntimeError{VMError{
            RuntimeErrorCode::TypeError,
            std::string(op_name) + ": " + role + " must be a tape"}});
    }
    return tape;
}

/**
 * @brief Ensure a tape has an id and normalized generation before use.
 */
inline void ensure_tape_identity(types::Tape& tape) {
    if (tape.tape_id == 0) tape.tape_id = PrimReg::allocate_tape_id();
    tape.generation = types::tape_ref::normalize_generation(tape.generation);
}

} // namespace eta::runtime::detail::core_primitives_aad
