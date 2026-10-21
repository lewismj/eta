#pragma once

#include <expected>
#include <string>

#include "eta/runtime/core_primitives_internal.h"
#include "eta/runtime/numeric_ops.h"
#include "eta/runtime/numeric_value.h"
#include "eta/runtime/types/tape_ref.h"

namespace eta::runtime::detail::core_primitives_arithmetic {

/**
 * @brief Compare two operands where at least one is a TapeRef.
 *
 * Comparison is non-differentiable: strict policy rejects it, otherwise the
 * primal values are compared. Shared by the comparison primitives and the
 * VM comparison opcode.
 */
inline std::expected<bool, RuntimeError> compare_tape_operands(
    Heap& heap,
    vm::VM* vm,
    const char* name,
    numeric_ops::CmpOp op,
    LispVal a,
    LispVal b) {
    if (PrimReg::policy_is_strict(vm)) {
        return std::unexpected(PrimReg::make_nondiff_error(name, "comparison"));
    }

    auto tape = PrimReg::get_active_tape_for_op(heap, vm, name);
    if (!tape) return std::unexpected(tape.error());

    auto extract = [&](LispVal value, const char* role)
        -> std::expected<double, RuntimeError> {
        if (!types::tape_ref::is_tape_ref(value)) {
            auto numeric = numeric_ops::classify(value, heap);
            if (!numeric.is_valid()) {
                return std::unexpected(RuntimeError{VMError{
                    RuntimeErrorCode::TypeError,
                    std::string(name) + ": argument is not a number"}});
            }
            return numeric.as_double();
        }

        auto index = PrimReg::validate_ref_for_tape(*tape, value, name, role);
        if (!index) return std::unexpected(index.error());
        return (*tape)->entries[*index].primal;
    };

    auto lhs = extract(a, "lhs");
    if (!lhs) return std::unexpected(lhs.error());
    auto rhs = extract(b, "rhs");
    if (!rhs) return std::unexpected(rhs.error());
    return numeric_ops::apply_compare(op, *lhs, *rhs);
}

/**
 * @brief Build numeric comparison primitive closures with TapeRef support.
 */
inline auto make_comparison_primitive(
    Heap& heap,
    vm::VM* vm,
    const char* name,
    numeric_ops::CmpOp op) {
    return [&heap, vm, name, op](
               PrimReg::Args args) -> std::expected<LispVal, RuntimeError> {
        if (args.size() < 2) {
            return std::unexpected(RuntimeError{VMError{
                RuntimeErrorCode::InvalidArity,
                std::string(name) + ": requires at least 2 arguments"}});
        }

        for (std::size_t i = 0; i + 1 < args.size(); ++i) {
            if (types::tape_ref::is_tape_ref(args[i])
                || types::tape_ref::is_tape_ref(args[i + 1])) {
                auto r = compare_tape_operands(heap, vm, name, op, args[i], args[i + 1]);
                if (!r) return std::unexpected(r.error());
                if (!*r) return nanbox::False;
                continue;
            }

            auto r = numeric_ops::compare(heap, op, args[i], args[i + 1]);
            if (!r) {
                return std::unexpected(RuntimeError{VMError{
                    RuntimeErrorCode::TypeError,
                    std::string(name) + ": argument is not a number"}});
            }
            if (!*r) return nanbox::False;
        }

        return nanbox::True;
    };
}

} // namespace eta::runtime::detail::core_primitives_arithmetic
