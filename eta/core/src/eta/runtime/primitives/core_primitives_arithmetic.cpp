#include <algorithm>
#include <cmath>
#include <cstdint>
#include <expected>
#include <limits>
#include <span>
#include <string>

#include "eta/runtime/core_primitives_internal.h"
#include "eta/runtime/primitives/core_primitives_arithmetic_helpers.h"
#include "eta/runtime/aad_unary_helpers.h"
#include "eta/runtime/factory.h"
#include "eta/runtime/numeric_ops.h"
#include "eta/runtime/numeric_value.h"
#include "eta/runtime/types/tape.h"
#include "eta/runtime/types/tape_ref.h"
#include "eta/runtime/vm/vm.h"

namespace eta::runtime {

namespace {

/**
 * min / max on plain numbers, as IEEE 754-2019 minimum / maximum:
 * any NaN gives NaN, and -0.0 orders below +0.0.  Candidates are compared
 * exactly; the result is inexact if any argument is.
 */
std::expected<LispVal, RuntimeError> numeric_extremum(Heap& heap, std::span<const LispVal> args,
                                                      const char* name, numeric_ops::CmpOp better) {
    using numeric_ops::CmpOp;
    bool any_flonum = false;
    bool any_nan = false;
    bool have_best = false;
    NumericValue best{};
    for (const LispVal arg : args) {
        const auto n = numeric_ops::classify(arg, heap);
        if (!n.is_valid()) {
            return std::unexpected(RuntimeError{VMError{
                RuntimeErrorCode::TypeError, std::string(name) + ": argument is not a number"}});
        }
        any_flonum = any_flonum || n.is_flonum();
        if (n.is_flonum() && std::isnan(n.float_val)) {
            any_nan = true;
            continue;
        }
        if (!have_best) {
            best = n;
            have_best = true;
            continue;
        }
        bool replace = numeric_ops::compare(better, n, best);
        if (!replace && n.as_double() == 0.0 && best.as_double() == 0.0) {
            const bool n_neg = std::signbit(n.as_double());
            const bool best_neg = std::signbit(best.as_double());
            replace = better == CmpOp::Lt ? (n_neg && !best_neg) : (!n_neg && best_neg);
        }
        if (replace) best = n;
    }
    if (any_nan) return make_flonum(std::numeric_limits<double>::quiet_NaN());
    if (any_flonum) return make_flonum(best.as_double());
    return make_fixnum(heap, best.int_val);
}

/// IEEE 754 roundToIntegralTiesToEven, independent of the current rounding mode.
double round_ties_to_even(const double x) {
    const double r = std::round(x); ///< ties away from zero
    if (std::fabs(x - std::trunc(x)) != 0.5) return r;
    return 2.0 * std::round(x / 2.0); ///< a tie: take the even neighbour
}

} // namespace

void PrimReg::register_arithmetic() {
    using Args = PrimReg::Args;
    auto& env = this->env;
    auto& heap = this->heap;
    auto* vm = this->vm;

    /**
     * Arithmetic: + - * /
     *
     * Each operator checks for TapeRef arguments. When found, the operation
     * is folded through VM::tape_binary_op().
     */

    env.register_builtin("+", 0, true, [&heap, vm](Args args) -> std::expected<LispVal, RuntimeError> {
        if (vm && PrimReg::has_tape_ref(args)) {
            if (args.empty()) return make_fixnum(heap, int64_t(0));
            LispVal acc = args[0];
            for (size_t i = 1; i < args.size(); ++i) {
                auto r = vm->tape_binary_op(vm::OpCode::Add, acc, args[i]);
                if (!r) return r;
                acc = *r;
            }
            return acc;
        }
        if (args.empty()) return make_fixnum(heap, int64_t(0));
        /// Seed with the first argument, not exact 0, so (+ -0.0 -0.0) is -0.0 like the Add opcode
        NumericValue acc{};
        for (size_t i = 0; i < args.size(); ++i) {
            auto n = numeric_ops::classify(args[i], heap);
            if (!n.is_valid()) {
                return std::unexpected(RuntimeError{VMError{
                    RuntimeErrorCode::TypeError, "+: argument is not a number"}});
            }
            acc = i == 0 ? n : *numeric_ops::arith(numeric_ops::ArithOp::Add, acc, n);
        }
        return numeric_ops::box(heap, acc);
    });

    env.register_builtin("-", 1, true, [&heap, vm](Args args) -> std::expected<LispVal, RuntimeError> {
        if (args.empty()) {
            return std::unexpected(RuntimeError{VMError{
                RuntimeErrorCode::InvalidArity,
                "-: requires at least 1 argument"}});
        }
        if (vm && PrimReg::has_tape_ref(args)) {
            if (args.size() == 1) {
                auto zero = make_fixnum(heap, int64_t(0));
                if (!zero) return zero;
                return vm->tape_binary_op(vm::OpCode::Sub, *zero, args[0]);
            }
            LispVal acc = args[0];
            for (size_t i = 1; i < args.size(); ++i) {
                auto r = vm->tape_binary_op(vm::OpCode::Sub, acc, args[i]);
                if (!r) return r;
                acc = *r;
            }
            return acc;
        }
        auto first = numeric_ops::classify(args[0], heap);
        if (!first.is_valid()) {
            return std::unexpected(RuntimeError{VMError{
                RuntimeErrorCode::TypeError, "-: argument is not a number"}});
        }

        if (args.size() == 1) {
            if (first.is_flonum()) return make_flonum(-first.float_val);
            /// -INT64_MIN overflows: promote, as binary arithmetic does
            if (first.int_val == INT64_MIN) return make_flonum(-static_cast<double>(first.int_val));
            return make_fixnum(heap, -first.int_val);
        }

        NumericValue acc = first;
        for (size_t i = 1; i < args.size(); ++i) {
            auto n = numeric_ops::classify(args[i], heap);
            if (!n.is_valid()) {
                return std::unexpected(RuntimeError{VMError{
                    RuntimeErrorCode::TypeError, "-: argument is not a number"}});
            }
            acc = *numeric_ops::arith(numeric_ops::ArithOp::Sub, acc, n);
        }
        return numeric_ops::box(heap, acc);
    });

    env.register_builtin("*", 0, true, [&heap, vm](Args args) -> std::expected<LispVal, RuntimeError> {
        if (vm && PrimReg::has_tape_ref(args)) {
            if (args.empty()) return make_fixnum(heap, int64_t(1));
            LispVal acc = args[0];
            for (size_t i = 1; i < args.size(); ++i) {
                auto r = vm->tape_binary_op(vm::OpCode::Mul, acc, args[i]);
                if (!r) return r;
                acc = *r;
            }
            return acc;
        }
        NumericValue acc{NumType::Fixnum, 1, 0.0};
        for (auto v : args) {
            auto n = numeric_ops::classify(v, heap);
            if (!n.is_valid()) {
                return std::unexpected(RuntimeError{VMError{
                    RuntimeErrorCode::TypeError, "*: argument is not a number"}});
            }
            acc = *numeric_ops::arith(numeric_ops::ArithOp::Mul, acc, n);
        }
        return numeric_ops::box(heap, acc);
    });

    env.register_builtin("/", 1, true, [&heap, vm](Args args) -> std::expected<LispVal, RuntimeError> {
        if (args.empty()) {
            return std::unexpected(RuntimeError{VMError{
                RuntimeErrorCode::InvalidArity,
                "/: requires at least 1 argument"}});
        }
        if (vm && PrimReg::has_tape_ref(args)) {
            if (args.size() == 1) {
                auto one = make_flonum(1.0);
                if (!one) return one;
                return vm->tape_binary_op(vm::OpCode::Div, *one, args[0]);
            }
            LispVal acc = args[0];
            for (size_t i = 1; i < args.size(); ++i) {
                auto r = vm->tape_binary_op(vm::OpCode::Div, acc, args[i]);
                if (!r) return r;
                acc = *r;
            }
            return acc;
        }
        auto first = numeric_ops::classify(args[0], heap);
        if (!first.is_valid()) {
            return std::unexpected(RuntimeError{VMError{
                RuntimeErrorCode::TypeError, "/: argument is not a number"}});
        }

        NumericValue acc = first;
        if (args.size() == 1) {
            /// (/ x) is (/ 1 x): exact when the reciprocal is, so (/ 1) is 1 like (/ 1 1)
            acc = NumericValue{NumType::Fixnum, 1, 0.0};
            auto q = numeric_ops::arith(numeric_ops::ArithOp::Div, acc, first);
            if (!q) {
                return std::unexpected(RuntimeError{VMError{
                    RuntimeErrorCode::TypeError, "/: division by zero"}});
            }
            return numeric_ops::box(heap, *q);
        }

        for (size_t i = 1; i < args.size(); ++i) {
            auto n = numeric_ops::classify(args[i], heap);
            if (!n.is_valid()) {
                return std::unexpected(RuntimeError{VMError{
                    RuntimeErrorCode::TypeError, "/: argument is not a number"}});
            }
            auto q = numeric_ops::arith(numeric_ops::ArithOp::Div, acc, n);
            if (!q) {
                return std::unexpected(RuntimeError{VMError{
                    RuntimeErrorCode::TypeError, "/: division by zero"}});
            }
            acc = *q;
        }
        return numeric_ops::box(heap, acc);
    });

    /**
     * Comparison: = < > <= >=
     */

    using numeric_ops::CmpOp;
    using detail::core_primitives_arithmetic::make_comparison_primitive;
    env.register_builtin("=",  2, true, make_comparison_primitive(heap, vm, "=",  CmpOp::NumEq));
    env.register_builtin("<",  2, true, make_comparison_primitive(heap, vm, "<",  CmpOp::Lt));
    env.register_builtin(">",  2, true, make_comparison_primitive(heap, vm, ">",  CmpOp::Gt));
    env.register_builtin("<=", 2, true, make_comparison_primitive(heap, vm, "<=", CmpOp::Le));
    env.register_builtin(">=", 2, true, make_comparison_primitive(heap, vm, ">=", CmpOp::Ge));

    /**
     * Equivalence: eq? eqv? not
     */

    env.register_builtin("eq?", 2, false, [](Args args) -> std::expected<LispVal, RuntimeError> {
        return (args[0] == args[1]) ? nanbox::True : nanbox::False;
    });

    env.register_builtin("eqv?", 2, false, [&heap](Args args) -> std::expected<LispVal, RuntimeError> {
        LispVal a = args[0], b = args[1];
        if (a == b) return nanbox::True;
        if (!ops::is_boxed(a) || !ops::is_boxed(b)) return nanbox::False;
        if (ops::tag(a) == Tag::String) return nanbox::False;
        if (ops::tag(a) == Tag::HeapObject && ops::tag(b) == Tag::HeapObject) {
            auto na = classify_numeric(a, heap);
            auto nb = classify_numeric(b, heap);
            if (na.is_fixnum() && nb.is_fixnum()) {
                return (na.int_val == nb.int_val) ? nanbox::True : nanbox::False;
            }
            if (na.is_flonum() && nb.is_flonum()) {
                return (na.float_val == nb.float_val) ? nanbox::True : nanbox::False;
            }
        }
        return nanbox::False;
    });

    env.register_builtin("not", 1, false, [](Args args) -> std::expected<LispVal, RuntimeError> {
        return (args[0] == nanbox::False) ? nanbox::True : nanbox::False;
    });

    register_pair_list_bridge();

    /**
     * Type predicates
     */

    env.register_builtin("number?", 1, false, [&heap](Args args) -> std::expected<LispVal, RuntimeError> {
        return classify_numeric(args[0], heap).is_valid() ? nanbox::True : nanbox::False;
    });

    env.register_builtin("boolean?", 1, false, [](Args args) -> std::expected<LispVal, RuntimeError> {
        return (args[0] == nanbox::True || args[0] == nanbox::False)
                   ? nanbox::True
                   : nanbox::False;
    });

    env.register_builtin("string?", 1, false, [](Args args) -> std::expected<LispVal, RuntimeError> {
        if (!ops::is_boxed(args[0])) return nanbox::False;
        return (ops::tag(args[0]) == Tag::String) ? nanbox::True : nanbox::False;
    });

    env.register_builtin("char?", 1, false, [](Args args) -> std::expected<LispVal, RuntimeError> {
        if (!ops::is_boxed(args[0])) return nanbox::False;
        return (ops::tag(args[0]) == Tag::Char) ? nanbox::True : nanbox::False;
    });

    env.register_builtin("symbol?", 1, false, [](Args args) -> std::expected<LispVal, RuntimeError> {
        if (!ops::is_boxed(args[0])) return nanbox::False;
        return (ops::tag(args[0]) == Tag::Symbol) ? nanbox::True : nanbox::False;
    });

    env.register_builtin("procedure?", 1, false, [&heap](Args args) -> std::expected<LispVal, RuntimeError> {
        if (!ops::is_boxed(args[0]) || ops::tag(args[0]) != Tag::HeapObject) {
            return nanbox::False;
        }
        auto id = ops::payload(args[0]);
        if (heap.try_get_as<ObjectKind::Closure, types::Closure>(id)) {
            return nanbox::True;
        }
        if (heap.try_get_as<ObjectKind::Primitive, types::Primitive>(id)) {
            return nanbox::True;
        }
        return nanbox::False;
    });

    env.register_builtin("integer?", 1, false, [&heap](Args args) -> std::expected<LispVal, RuntimeError> {
        auto n = classify_numeric(args[0], heap);
        if (n.is_fixnum()) return nanbox::True;
        if (n.is_flonum()) {
            double d = n.float_val;
            return (std::isfinite(d) && d == std::floor(d))
                       ? nanbox::True
                       : nanbox::False;
        }
        return nanbox::False;
    });

    /**
     * Numeric predicates: zero? positive? negative?
     */

    env.register_builtin("zero?", 1, false, [&heap](Args args) -> std::expected<LispVal, RuntimeError> {
        auto n = classify_numeric(args[0], heap);
        if (!n.is_valid()) {
            return std::unexpected(RuntimeError{VMError{
                RuntimeErrorCode::TypeError, "zero?: argument is not a number"}});
        }
        if (n.is_flonum()) return (n.float_val == 0.0) ? nanbox::True : nanbox::False;
        return (n.int_val == 0) ? nanbox::True : nanbox::False;
    });

    env.register_builtin("positive?", 1, false, [&heap](Args args) -> std::expected<LispVal, RuntimeError> {
        auto n = classify_numeric(args[0], heap);
        if (!n.is_valid()) {
            return std::unexpected(RuntimeError{VMError{
                RuntimeErrorCode::TypeError,
                "positive?: argument is not a number"}});
        }
        if (n.is_flonum()) return (n.float_val > 0.0) ? nanbox::True : nanbox::False;
        return (n.int_val > 0) ? nanbox::True : nanbox::False;
    });

    env.register_builtin("negative?", 1, false, [&heap](Args args) -> std::expected<LispVal, RuntimeError> {
        auto n = classify_numeric(args[0], heap);
        if (!n.is_valid()) {
            return std::unexpected(RuntimeError{VMError{
                RuntimeErrorCode::TypeError,
                "negative?: argument is not a number"}});
        }
        if (n.is_flonum()) return (n.float_val < 0.0) ? nanbox::True : nanbox::False;
        return (n.int_val < 0) ? nanbox::True : nanbox::False;
    });

    /**
     * Numeric operations: abs min max modulo remainder
     */

    auto resolve_tape_numeric =
        [&heap](types::Tape* tape,
                LispVal value,
                const char* op_name,
                const char* role) -> std::expected<uint32_t, RuntimeError> {
            if (types::tape_ref::is_tape_ref(value)) {
                return PrimReg::validate_ref_for_tape(tape, value, op_name, role);
            }
            auto n = classify_numeric(value, heap);
            if (!n.is_valid()) {
                return std::unexpected(RuntimeError{VMError{
                    RuntimeErrorCode::TypeError,
                    std::string(op_name) + ": argument is not a number"}});
            }
            const uint32_t idx = tape->push_const(n.as_double());
            if (idx > types::tape_ref::MAX_NODE_INDEX) {
                return std::unexpected(RuntimeError{VMError{
                    RuntimeErrorCode::InternalError,
                    std::string(op_name)
                        + ": tape node index exceeds TapeRef capacity"}});
            }
            return idx;
        };

    auto make_tape_ref_result = [](types::Tape* tape, uint32_t idx, const char* op_name)
        -> std::expected<LispVal, RuntimeError> {
        return detail::aad_unary::make_tape_ref_result(tape, idx, op_name);
    };

    env.register_builtin(
        "abs", 1, false,
        [&heap, vm, resolve_tape_numeric, make_tape_ref_result](Args args)
            -> std::expected<LispVal, RuntimeError> {
            if (vm && types::tape_ref::is_tape_ref(args[0])) {
                auto tape = PrimReg::get_active_tape_for_op(heap, vm, "abs");
                if (!tape) return std::unexpected(tape.error());
                auto idx = resolve_tape_numeric(*tape, args[0], "abs", "arg");
                if (!idx) return std::unexpected(idx.error());
                const double x = (*tape)->entries[*idx].primal;
                if (x == 0.0 && PrimReg::policy_is_strict(vm)) {
                    return std::unexpected(
                        PrimReg::make_nondiff_error("abs", "x == 0"));
                }
                const uint32_t out =
                    (*tape)->push({types::TapeOp::Abs, *idx, *idx, std::abs(x), 0.0});
                return make_tape_ref_result(*tape, out, "abs");
            }
            auto n = classify_numeric(args[0], heap);
            if (!n.is_valid()) {
                return std::unexpected(RuntimeError{VMError{
                    RuntimeErrorCode::TypeError, "abs: argument is not a number"}});
            }
            if (n.is_flonum()) return make_flonum(std::abs(n.float_val));
            /// |INT64_MIN| overflows: promote, as binary arithmetic does
            if (n.int_val == INT64_MIN) return make_flonum(-static_cast<double>(n.int_val));
            return make_fixnum(heap, n.int_val < 0 ? -n.int_val : n.int_val);
        });

    env.register_builtin(
        "min", 2, true,
        [&heap, vm, resolve_tape_numeric, make_tape_ref_result](Args args)
            -> std::expected<LispVal, RuntimeError> {
            if (vm && PrimReg::has_tape_ref(args)) {
                auto tape = PrimReg::get_active_tape_for_op(heap, vm, "min");
                if (!tape) return std::unexpected(tape.error());
                auto best_idx = resolve_tape_numeric(*tape, args[0], "min", "arg[0]");
                if (!best_idx) return std::unexpected(best_idx.error());
                for (size_t i = 1; i < args.size(); ++i) {
                    auto cur_idx = resolve_tape_numeric(
                        *tape, args[i], "min", ("arg[" + std::to_string(i) + "]").c_str());
                    if (!cur_idx) return std::unexpected(cur_idx.error());
                    const double a = (*tape)->entries[*best_idx].primal;
                    const double b = (*tape)->entries[*cur_idx].primal;
                    if (a == b && PrimReg::policy_is_strict(vm)) {
                        return std::unexpected(
                            PrimReg::make_nondiff_error("min", "tie (a == b)"));
                    }
                    const uint32_t out = (*tape)->push(
                        {types::TapeOp::Min, *best_idx, *cur_idx, std::min(a, b), 0.0});
                    if (out > types::tape_ref::MAX_NODE_INDEX) {
                        return std::unexpected(RuntimeError{VMError{
                            RuntimeErrorCode::InternalError,
                            "min: tape node index exceeds TapeRef capacity"}});
                    }
                    best_idx = out;
                }
                return make_tape_ref_result(*tape, *best_idx, "min");
            }
            return numeric_extremum(heap, args, "min", numeric_ops::CmpOp::Lt);
        });

    env.register_builtin(
        "max", 2, true,
        [&heap, vm, resolve_tape_numeric, make_tape_ref_result](Args args)
            -> std::expected<LispVal, RuntimeError> {
            if (vm && PrimReg::has_tape_ref(args)) {
                auto tape = PrimReg::get_active_tape_for_op(heap, vm, "max");
                if (!tape) return std::unexpected(tape.error());
                auto best_idx = resolve_tape_numeric(*tape, args[0], "max", "arg[0]");
                if (!best_idx) return std::unexpected(best_idx.error());
                for (size_t i = 1; i < args.size(); ++i) {
                    auto cur_idx = resolve_tape_numeric(
                        *tape, args[i], "max", ("arg[" + std::to_string(i) + "]").c_str());
                    if (!cur_idx) return std::unexpected(cur_idx.error());
                    const double a = (*tape)->entries[*best_idx].primal;
                    const double b = (*tape)->entries[*cur_idx].primal;
                    if (a == b && PrimReg::policy_is_strict(vm)) {
                        return std::unexpected(
                            PrimReg::make_nondiff_error("max", "tie (a == b)"));
                    }
                    const uint32_t out = (*tape)->push(
                        {types::TapeOp::Max, *best_idx, *cur_idx, std::max(a, b), 0.0});
                    if (out > types::tape_ref::MAX_NODE_INDEX) {
                        return std::unexpected(RuntimeError{VMError{
                            RuntimeErrorCode::InternalError,
                            "max: tape node index exceeds TapeRef capacity"}});
                    }
                    best_idx = out;
                }
                return make_tape_ref_result(*tape, *best_idx, "max");
            }
            return numeric_extremum(heap, args, "max", numeric_ops::CmpOp::Gt);
        });

    env.register_builtin("modulo", 2, false, [&heap](Args args) -> std::expected<LispVal, RuntimeError> {
        auto a = classify_numeric(args[0], heap);
        auto b = classify_numeric(args[1], heap);
        if (!a.is_valid() || !b.is_valid()) {
            return std::unexpected(RuntimeError{VMError{
                RuntimeErrorCode::TypeError,
                "modulo: arguments must be numbers"}});
        }
        /// An exact zero divisor is an error, as for /; an inexact one gives NaN (IEEE)
        if (b.is_fixnum() && b.int_val == 0) {
            return std::unexpected(RuntimeError{VMError{
                RuntimeErrorCode::TypeError, "modulo: division by zero"}});
        }
        if (a.is_flonum() || b.is_flonum()) {
            /// Floored: the result takes the sign of the divisor, as for fixnums
            const double divisor = b.as_double();
            double r = std::fmod(a.as_double(), divisor);
            if (r != 0.0 && ((r < 0.0) != (divisor < 0.0))) r += divisor;
            if (r == 0.0) r = std::copysign(0.0, divisor);
            return make_flonum(r);
        }
        /// INT64_MIN % -1 overflows (and traps on x86)
        if (b.int_val == -1) return make_fixnum(heap, int64_t(0));
        int64_t r = a.int_val % b.int_val;
        if (r != 0 && ((r < 0) != (b.int_val < 0))) r += b.int_val;
        return make_fixnum(heap, r);
    });

    env.register_builtin("remainder", 2, false, [&heap](Args args) -> std::expected<LispVal, RuntimeError> {
        auto a = classify_numeric(args[0], heap);
        auto b = classify_numeric(args[1], heap);
        if (!a.is_valid() || !b.is_valid()) {
            return std::unexpected(RuntimeError{VMError{
                RuntimeErrorCode::TypeError,
                "remainder: arguments must be numbers"}});
        }
        /// An exact zero divisor is an error, as for /; an inexact one gives NaN (IEEE)
        if (b.is_fixnum() && b.int_val == 0) {
            return std::unexpected(RuntimeError{VMError{
                RuntimeErrorCode::TypeError, "remainder: division by zero"}});
        }
        if (a.is_flonum() || b.is_flonum()) {
            /// Truncated (sign of the dividend); std::remainder rounds to nearest instead
            return make_flonum(std::fmod(a.as_double(), b.as_double()));
        }
        /// INT64_MIN % -1 overflows (and traps on x86)
        if (b.int_val == -1) return make_fixnum(heap, int64_t(0));
        return make_fixnum(heap, a.int_val % b.int_val);
    });

    /**
     * Rounding: floor ceiling truncate round
     *
     * Exact integers are returned unchanged. Flonums follow IEEE 754
     * roundToIntegral: NaN and infinities pass through, the sign of zero is
     * kept ((ceiling -0.5) is -0.0), and round breaks ties to even.
     */
    auto register_rounding = [&env, &heap](const char* name, double (*round_fn)(double)) {
        env.register_builtin(name, 1, false, [&heap, name, round_fn](Args args) -> std::expected<LispVal, RuntimeError> {
            const auto n = numeric_ops::classify(args[0], heap);
            if (!n.is_valid()) {
                return std::unexpected(RuntimeError{VMError{
                    RuntimeErrorCode::TypeError, std::string(name) + ": argument is not a number"}});
            }
            if (n.is_fixnum()) return args[0];
            return make_flonum(round_fn(n.float_val));
        });
    };
    register_rounding("floor",    [](double x) { return std::floor(x); });
    register_rounding("ceiling",  [](double x) { return std::ceil(x); });
    register_rounding("truncate", [](double x) { return std::trunc(x); });
    register_rounding("round",    round_ties_to_even);
}

} // namespace eta::runtime
