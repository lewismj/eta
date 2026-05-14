#include <cmath>
#include <expected>
#include <string>

#include "eta/runtime/core_primitives_internal.h"
#include "eta/runtime/primitives/core_primitives_math_helpers.h"
#include "eta/runtime/aad_unary_helpers.h"
#include "eta/runtime/factory.h"
#include "eta/runtime/numeric_value.h"
#include "eta/runtime/string_view.h"
#include "eta/runtime/types/tape.h"
#include "eta/runtime/types/tape_ref.h"
#include "eta/runtime/vm/vm.h"

namespace eta::runtime {

void PrimReg::register_math() {
    using Args = PrimReg::Args;
    auto& env = this->env;
    auto& heap = this->heap;
    auto& intern_table = this->intern_table;
    auto* vm = this->vm;

    /**
     * Transcendental math: sin cos tan asin acos atan exp log sqrt pow
     */

    env.register_builtin("sin", 1, false,
        [&heap, vm](Args args)
            -> std::expected<LispVal, RuntimeError> {
            if (vm && types::tape_ref::is_tape_ref(args[0])) {
                auto tape = PrimReg::get_active_tape_for_op(heap, vm, "sin");
                if (!tape) return std::unexpected(tape.error());
                auto idx = PrimReg::validate_ref_for_tape(*tape, args[0], "sin", "arg");
                if (!idx) return std::unexpected(idx.error());
                const double val = std::sin((*tape)->entries[*idx].primal);
                return detail::aad_unary::push_unary_tape_entry(
                    *tape, types::TapeOp::Sin, *idx, val, "sin");
            }
            return detail::aad_unary::dispatch_numeric_fallback(
                heap,
                args[0],
                "sin",
                [](NumericValue n) -> std::expected<LispVal, RuntimeError> {
                    return make_flonum(std::sin(n.as_double()));
                });
        });

    env.register_builtin("cos", 1, false,
        [&heap, vm](Args args)
            -> std::expected<LispVal, RuntimeError> {
            if (vm && types::tape_ref::is_tape_ref(args[0])) {
                auto tape = PrimReg::get_active_tape_for_op(heap, vm, "cos");
                if (!tape) return std::unexpected(tape.error());
                auto idx = PrimReg::validate_ref_for_tape(*tape, args[0], "cos", "arg");
                if (!idx) return std::unexpected(idx.error());
                const double val = std::cos((*tape)->entries[*idx].primal);
                return detail::aad_unary::push_unary_tape_entry(
                    *tape, types::TapeOp::Cos, *idx, val, "cos");
            }
            return detail::aad_unary::dispatch_numeric_fallback(
                heap,
                args[0],
                "cos",
                [](NumericValue n) -> std::expected<LispVal, RuntimeError> {
                    return make_flonum(std::cos(n.as_double()));
                });
        });

    env.register_builtin("tan", 1, false,
        [&heap, vm](Args args)
            -> std::expected<LispVal, RuntimeError> {
            if (vm && types::tape_ref::is_tape_ref(args[0])) {
                auto tape = PrimReg::get_active_tape_for_op(heap, vm, "tan");
                if (!tape) return std::unexpected(tape.error());
                auto idx = PrimReg::validate_ref_for_tape(*tape, args[0], "tan", "arg");
                if (!idx) return std::unexpected(idx.error());
                const double val = std::tan((*tape)->entries[*idx].primal);
                return detail::aad_unary::push_unary_tape_entry(
                    *tape, types::TapeOp::Tan, *idx, val, "tan");
            }
            return detail::aad_unary::dispatch_numeric_fallback(
                heap,
                args[0],
                "tan",
                [](NumericValue n) -> std::expected<LispVal, RuntimeError> {
                    return make_flonum(std::tan(n.as_double()));
                });
        });

    env.register_builtin("asin", 1, false,
        [&heap, vm](Args args)
            -> std::expected<LispVal, RuntimeError> {
            if (vm && types::tape_ref::is_tape_ref(args[0])) {
                auto tape = PrimReg::get_active_tape_for_op(heap, vm, "asin");
                if (!tape) return std::unexpected(tape.error());
                auto idx = PrimReg::validate_ref_for_tape(*tape, args[0], "asin", "arg");
                if (!idx) return std::unexpected(idx.error());
                const double x = (*tape)->entries[*idx].primal;
                if (x < -1.0 || x > 1.0) {
                    return std::unexpected(PrimReg::make_unary_domain_error("asin", x, "requires -1 <= x <= 1"));
                }
                const double val = std::asin(x);
                return detail::aad_unary::push_unary_tape_entry(
                    *tape, types::TapeOp::Asin, *idx, val, "asin");
            }
            return detail::aad_unary::dispatch_numeric_fallback(
                heap,
                args[0],
                "asin",
                [](NumericValue n) -> std::expected<LispVal, RuntimeError> {
                    return make_flonum(std::asin(n.as_double()));
                });
        });

    env.register_builtin("acos", 1, false,
        [&heap, vm](Args args)
            -> std::expected<LispVal, RuntimeError> {
            if (vm && types::tape_ref::is_tape_ref(args[0])) {
                auto tape = PrimReg::get_active_tape_for_op(heap, vm, "acos");
                if (!tape) return std::unexpected(tape.error());
                auto idx = PrimReg::validate_ref_for_tape(*tape, args[0], "acos", "arg");
                if (!idx) return std::unexpected(idx.error());
                const double x = (*tape)->entries[*idx].primal;
                if (x < -1.0 || x > 1.0) {
                    return std::unexpected(PrimReg::make_unary_domain_error("acos", x, "requires -1 <= x <= 1"));
                }
                const double val = std::acos(x);
                return detail::aad_unary::push_unary_tape_entry(
                    *tape, types::TapeOp::Acos, *idx, val, "acos");
            }
            return detail::aad_unary::dispatch_numeric_fallback(
                heap,
                args[0],
                "acos",
                [](NumericValue n) -> std::expected<LispVal, RuntimeError> {
                    return make_flonum(std::acos(n.as_double()));
                });
        });

    env.register_builtin("atan", 1, true,
        [&heap, vm](Args args)
            -> std::expected<LispVal, RuntimeError> {
            if (args.size() == 1 && vm && types::tape_ref::is_tape_ref(args[0])) {
                auto tape = PrimReg::get_active_tape_for_op(heap, vm, "atan");
                if (!tape) return std::unexpected(tape.error());
                auto idx = PrimReg::validate_ref_for_tape(*tape, args[0], "atan", "arg");
                if (!idx) return std::unexpected(idx.error());
                const double val = std::atan((*tape)->entries[*idx].primal);
                return detail::aad_unary::push_unary_tape_entry(
                    *tape, types::TapeOp::Atan, *idx, val, "atan");
            }
            auto a = classify_numeric(args[0], heap);
            if (!a.is_valid()) {
                return std::unexpected(RuntimeError{VMError{
                    RuntimeErrorCode::TypeError, "atan: argument is not a number"}});
            }
            if (args.size() == 2) {
                auto b = classify_numeric(args[1], heap);
                if (!b.is_valid()) {
                    return std::unexpected(RuntimeError{VMError{
                        RuntimeErrorCode::TypeError, "atan: argument is not a number"}});
                }
                return make_flonum(std::atan2(a.as_double(), b.as_double()));
            }
            return make_flonum(std::atan(a.as_double()));
        });

    env.register_builtin("exp", 1, false,
        [&heap, vm](Args args)
            -> std::expected<LispVal, RuntimeError> {
            if (vm && types::tape_ref::is_tape_ref(args[0])) {
                auto tape = PrimReg::get_active_tape_for_op(heap, vm, "exp");
                if (!tape) return std::unexpected(tape.error());
                auto idx = PrimReg::validate_ref_for_tape(*tape, args[0], "exp", "arg");
                if (!idx) return std::unexpected(idx.error());
                const double val = std::exp((*tape)->entries[*idx].primal);
                return detail::aad_unary::push_unary_tape_entry(
                    *tape, types::TapeOp::Exp, *idx, val, "exp");
            }
            return detail::aad_unary::dispatch_numeric_fallback(
                heap,
                args[0],
                "exp",
                [](NumericValue n) -> std::expected<LispVal, RuntimeError> {
                    return make_flonum(std::exp(n.as_double()));
                });
        });

    env.register_builtin("log", 1, false,
        [&heap, vm](Args args)
            -> std::expected<LispVal, RuntimeError> {
            if (vm && types::tape_ref::is_tape_ref(args[0])) {
                auto tape = PrimReg::get_active_tape_for_op(heap, vm, "log");
                if (!tape) return std::unexpected(tape.error());
                auto idx = PrimReg::validate_ref_for_tape(*tape, args[0], "log", "arg");
                if (!idx) return std::unexpected(idx.error());
                const double x = (*tape)->entries[*idx].primal;
                if (x <= 0.0) {
                    return std::unexpected(PrimReg::make_unary_domain_error("log", x, "requires x > 0"));
                }
                const double val = std::log(x);
                return detail::aad_unary::push_unary_tape_entry(
                    *tape, types::TapeOp::Log, *idx, val, "log");
            }
            return detail::aad_unary::dispatch_numeric_fallback(
                heap,
                args[0],
                "log",
                [](NumericValue n) -> std::expected<LispVal, RuntimeError> {
                    return make_flonum(std::log(n.as_double()));
                });
        });

    env.register_builtin("sqrt", 1, false,
        [&heap, vm](Args args)
            -> std::expected<LispVal, RuntimeError> {
            if (vm && types::tape_ref::is_tape_ref(args[0])) {
                auto tape = PrimReg::get_active_tape_for_op(heap, vm, "sqrt");
                if (!tape) return std::unexpected(tape.error());
                auto idx = PrimReg::validate_ref_for_tape(*tape, args[0], "sqrt", "arg");
                if (!idx) return std::unexpected(idx.error());
                const double x = (*tape)->entries[*idx].primal;
                if (x < 0.0) {
                    return std::unexpected(PrimReg::make_unary_domain_error("sqrt", x, "requires x >= 0"));
                }
                const double val = std::sqrt(x);
                return detail::aad_unary::push_unary_tape_entry(
                    *tape, types::TapeOp::Sqrt, *idx, val, "sqrt");
            }
            return detail::aad_unary::dispatch_numeric_fallback(
                heap,
                args[0],
                "sqrt",
                [](NumericValue n) -> std::expected<LispVal, RuntimeError> {
                    return make_flonum(std::sqrt(n.as_double()));
                });
        });

    env.register_builtin("pow", 2, false,
        [&heap, vm](Args args) -> std::expected<LispVal, RuntimeError> {
            auto lhs_is_ref = types::tape_ref::is_tape_ref(args[0]);
            auto rhs_is_ref = types::tape_ref::is_tape_ref(args[1]);

            if (vm && (lhs_is_ref || rhs_is_ref)) {
                auto tape = PrimReg::get_active_tape_for_op(heap, vm, "pow");
                if (!tape) return std::unexpected(tape.error());

                auto lhs_idx = detail::core_primitives_math::resolve_tape_numeric(
                    heap, *tape, args[0], "pow", "base");
                if (!lhs_idx) return std::unexpected(lhs_idx.error());
                auto rhs_idx = detail::core_primitives_math::resolve_tape_numeric(
                    heap, *tape, args[1], "pow", "exponent");
                if (!rhs_idx) return std::unexpected(rhs_idx.error());

                const double base = (*tape)->entries[*lhs_idx].primal;
                const double exponent = (*tape)->entries[*rhs_idx].primal;
                const bool exponent_is_integer = std::isfinite(exponent) && std::floor(exponent) == exponent;

                if (base < 0.0 && !exponent_is_integer) {
                    return std::unexpected(PrimReg::make_domain_error(
                        "pow", base, exponent, "negative base requires an integer exponent"));
                }
                if (base == 0.0 && exponent < 0.0) {
                    return std::unexpected(PrimReg::make_domain_error(
                        "pow", base, exponent, "0 raised to a negative exponent is undefined"));
                }
                if (PrimReg::policy_is_strict(vm)) {
                    if (base == 0.0 && exponent == 0.0) {
                        return std::unexpected(PrimReg::make_domain_error(
                            "pow", base, exponent, "strict mode rejects derivative at pow(0, 0)"));
                    }
                    if (base == 0.0 && exponent > 0.0 && exponent < 1.0) {
                        return std::unexpected(PrimReg::make_domain_error(
                            "pow", base, exponent, "strict mode rejects singular derivative at base=0"));
                    }
                }

                double primal = 0.0;
                if (base == 0.0 && exponent == 0.0) {
                    primal = 1.0;
                } else {
                    primal = std::pow(base, exponent);
                }

                const uint32_t out = (*tape)->push({
                    types::TapeOp::Pow, *lhs_idx, *rhs_idx, primal, 0.0});
                return detail::core_primitives_math::make_tape_ref_result(*tape, out, "pow");
            }

            auto a = classify_numeric(args[0], heap);
            auto b = classify_numeric(args[1], heap);
            if (!a.is_valid() || !b.is_valid()) {
                return std::unexpected(RuntimeError{VMError{
                    RuntimeErrorCode::TypeError, "pow: arguments must be numbers"}});
            }
            return make_flonum(std::pow(a.as_double(), b.as_double()));
        });

    env.register_builtin("set-aad-nondiff-policy!", 1, false,
        [vm, &intern_table](Args args) -> std::expected<LispVal, RuntimeError> {
            if (!vm) {
                return std::unexpected(RuntimeError{VMError{
                    RuntimeErrorCode::TypeError, "set-aad-nondiff-policy!: requires a running VM"}});
            }
            auto name = get_symbol_name(args[0], intern_table);
            if (!name) {
                return std::unexpected(RuntimeError{VMError{
                    RuntimeErrorCode::TypeError,
                    "set-aad-nondiff-policy!: argument must be a symbol ('strict or 'zero-subgrad)"}});
            }
            if (*name == "strict") {
                vm->set_aad_nondiff_policy(vm::VM::AadNondiffPolicy::Strict);
                return True;
            }
            if (*name == "zero-subgrad") {
                vm->set_aad_nondiff_policy(vm::VM::AadNondiffPolicy::ZeroSubgrad);
                return True;
            }
            return std::unexpected(RuntimeError{VMError{
                RuntimeErrorCode::TypeError,
                "set-aad-nondiff-policy!: expected 'strict or 'zero-subgrad"}});
        });

    env.register_builtin("aad-nondiff-policy", 0, false,
        [vm, &intern_table](Args /*args*/) -> std::expected<LispVal, RuntimeError> {
            if (!vm) {
                return std::unexpected(RuntimeError{VMError{
                    RuntimeErrorCode::TypeError, "aad-nondiff-policy: requires a running VM"}});
            }
            const char* name =
                (vm->aad_nondiff_policy() == vm::VM::AadNondiffPolicy::Strict)
                ? "strict"
                : "zero-subgrad";
            return make_symbol(intern_table, name);
        });
}

} // namespace eta::runtime
