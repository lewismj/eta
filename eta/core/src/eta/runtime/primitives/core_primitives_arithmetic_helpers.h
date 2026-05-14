#pragma once

#include <expected>
#include <string>

#include "eta/runtime/core_primitives_internal.h"
#include "eta/runtime/numeric_value.h"
#include "eta/runtime/types/tape_ref.h"

namespace eta::runtime::detail::core_primitives_arithmetic {

/**
 * @brief Build numeric comparison primitive closures with TapeRef support.
 */
template <typename CompareInt, typename CompareFloat>
auto make_comparison_primitive(
    Heap& heap,
    vm::VM* vm,
    const char* name,
    CompareInt cmp_int,
    CompareFloat cmp_float) {
    return [&heap, vm, name, cmp_int, cmp_float](
               PrimReg::Args args) -> std::expected<LispVal, RuntimeError> {
        if (args.size() < 2) {
            return std::unexpected(RuntimeError{VMError{
                RuntimeErrorCode::InvalidArity,
                std::string(name) + ": requires at least 2 arguments"}});
        }

        for (std::size_t i = 0; i + 1 < args.size(); ++i) {
            const bool a_is_ref = types::tape_ref::is_tape_ref(args[i]);
            const bool b_is_ref = types::tape_ref::is_tape_ref(args[i + 1]);

            if (a_is_ref || b_is_ref) {
                if (PrimReg::policy_is_strict(vm)) {
                    return std::unexpected(
                        PrimReg::make_nondiff_error(name, "comparison"));
                }

                auto tape = PrimReg::get_active_tape_for_op(heap, vm, name);
                if (!tape) return std::unexpected(tape.error());

                auto extract_ref = [&](LispVal value, bool is_ref, const char* role)
                    -> std::expected<double, RuntimeError> {
                    if (!is_ref) {
                        auto numeric = classify_numeric(value, heap);
                        if (!numeric.is_valid()) {
                            return std::unexpected(RuntimeError{VMError{
                                RuntimeErrorCode::TypeError,
                                std::string(name)
                                    + ": argument is not a number"}});
                        }
                        return numeric.as_double();
                    }

                    auto index =
                        PrimReg::validate_ref_for_tape(*tape, value, name, role);
                    if (!index) return std::unexpected(index.error());
                    return (*tape)->entries[*index].primal;
                };

                auto lhs = extract_ref(args[i], a_is_ref, "lhs");
                if (!lhs) return std::unexpected(lhs.error());
                auto rhs = extract_ref(args[i + 1], b_is_ref, "rhs");
                if (!rhs) return std::unexpected(rhs.error());
                if (!cmp_float(*lhs, *rhs)) return nanbox::False;
                continue;
            }

            auto lhs = classify_numeric(args[i], heap);
            auto rhs = classify_numeric(args[i + 1], heap);
            if (!lhs.is_valid() || !rhs.is_valid()) {
                return std::unexpected(RuntimeError{VMError{
                    RuntimeErrorCode::TypeError,
                    std::string(name) + ": argument is not a number"}});
            }

            if (lhs.is_flonum() || rhs.is_flonum()) {
                if (!cmp_float(lhs.as_double(), rhs.as_double())) {
                    return nanbox::False;
                }
            } else if (!cmp_int(lhs.int_val, rhs.int_val)) {
                return nanbox::False;
            }
        }

        return nanbox::True;
    };
}

} // namespace eta::runtime::detail::core_primitives_arithmetic
