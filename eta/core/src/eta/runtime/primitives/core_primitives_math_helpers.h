#pragma once

#include <cstdint>
#include <expected>
#include <string>

#include "eta/runtime/aad_unary_helpers.h"
#include "eta/runtime/core_primitives_internal.h"
#include "eta/runtime/numeric_value.h"
#include "eta/runtime/types/tape.h"
#include "eta/runtime/types/tape_ref.h"

namespace eta::runtime::detail::core_primitives_math {

/**
 * @brief Resolve a numeric argument to a tape node index for math primitives.
 */
inline std::expected<uint32_t, RuntimeError> resolve_tape_numeric(
    Heap& heap,
    types::Tape* tape,
    LispVal value,
    const char* op_name,
    const char* role) {
    if (types::tape_ref::is_tape_ref(value)) {
        return PrimReg::validate_ref_for_tape(tape, value, op_name, role);
    }

    auto numeric = classify_numeric(value, heap);
    if (!numeric.is_valid()) {
        return std::unexpected(RuntimeError{VMError{
            RuntimeErrorCode::TypeError,
            std::string(op_name) + ": argument is not a number"}});
    }

    const uint32_t index = tape->push_const(numeric.as_double());
    if (index > types::tape_ref::MAX_NODE_INDEX) {
        return std::unexpected(RuntimeError{VMError{
            RuntimeErrorCode::InternalError,
            std::string(op_name) + ": tape node index exceeds TapeRef capacity"}});
    }
    return index;
}

/**
 * @brief Build a TapeRef return value for a computed math tape node.
 */
inline std::expected<LispVal, RuntimeError> make_tape_ref_result(
    types::Tape* tape,
    uint32_t index,
    const char* op_name) {
    return detail::aad_unary::make_tape_ref_result(tape, index, op_name);
}

} // namespace eta::runtime::detail::core_primitives_math
