#include <algorithm>
#include <cmath>
#include <cstdint>
#include <expected>
#include <string>

#include "eta/runtime/core_primitives_internal.h"
#include "eta/runtime/primitives/core_primitives_arithmetic_helpers.h"
#include "eta/runtime/aad_unary_helpers.h"
#include "eta/runtime/factory.h"
#include "eta/runtime/numeric_value.h"
#include "eta/runtime/overflow.h"
#include "eta/runtime/types/tape.h"
#include "eta/runtime/types/tape_ref.h"
#include "eta/runtime/vm/vm.h"

namespace eta::runtime {

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
        int64_t isum = 0;
        bool use_float = false;
        double fsum = 0.0;
        for (auto v : args) {
            auto n = classify_numeric(v, heap);
            if (!n.is_valid()) {
                return std::unexpected(RuntimeError{VMError{
                    RuntimeErrorCode::TypeError, "+: argument is not a number"}});
            }
            if (n.is_flonum() || use_float) {
                if (!use_float) {
                    fsum = static_cast<double>(isum);
                    use_float = true;
                }
                fsum += n.as_double();
            } else {
                int64_t new_sum;
                if (detail::add_overflow(isum, n.int_val, &new_sum)) {
                    use_float = true;
                    fsum = static_cast<double>(isum)
                        + static_cast<double>(n.int_val);
                } else {
                    isum = new_sum;
                }
            }
        }
        if (use_float) return make_flonum(fsum);
        return make_fixnum(heap, isum);
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
        auto first = classify_numeric(args[0], heap);
        if (!first.is_valid()) {
            return std::unexpected(RuntimeError{VMError{
                RuntimeErrorCode::TypeError, "-: argument is not a number"}});
        }

        if (args.size() == 1) {
            if (first.is_flonum()) return make_flonum(-first.float_val);
            return make_fixnum(heap, -first.int_val);
        }

        bool use_float = first.is_flonum();
        int64_t iresult = first.int_val;
        double fresult = first.as_double();
        for (size_t i = 1; i < args.size(); ++i) {
            auto n = classify_numeric(args[i], heap);
            if (!n.is_valid()) {
                return std::unexpected(RuntimeError{VMError{
                    RuntimeErrorCode::TypeError, "-: argument is not a number"}});
            }
            if (n.is_flonum() || use_float) {
                if (!use_float) {
                    fresult = static_cast<double>(iresult);
                    use_float = true;
                }
                fresult -= n.as_double();
            } else {
                int64_t new_result;
                if (detail::sub_overflow(iresult, n.int_val, &new_result)) {
                    use_float = true;
                    fresult = static_cast<double>(iresult)
                        - static_cast<double>(n.int_val);
                } else {
                    iresult = new_result;
                }
            }
        }
        if (use_float) return make_flonum(fresult);
        return make_fixnum(heap, iresult);
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
        int64_t iprod = 1;
        bool use_float = false;
        double fprod = 1.0;
        for (auto v : args) {
            auto n = classify_numeric(v, heap);
            if (!n.is_valid()) {
                return std::unexpected(RuntimeError{VMError{
                    RuntimeErrorCode::TypeError, "*: argument is not a number"}});
            }
            if (n.is_flonum() || use_float) {
                if (!use_float) {
                    fprod = static_cast<double>(iprod);
                    use_float = true;
                }
                fprod *= n.as_double();
            } else {
                int64_t new_prod;
                if (detail::mul_overflow(iprod, n.int_val, &new_prod)) {
                    use_float = true;
                    fprod = static_cast<double>(iprod)
                        * static_cast<double>(n.int_val);
                } else {
                    iprod = new_prod;
                }
            }
        }
        if (use_float) return make_flonum(fprod);
        return make_fixnum(heap, iprod);
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
        auto first = classify_numeric(args[0], heap);
        if (!first.is_valid()) {
            return std::unexpected(RuntimeError{VMError{
                RuntimeErrorCode::TypeError, "/: argument is not a number"}});
        }

        if (args.size() == 1) {
            double d = first.as_double();
            if (d == 0.0) {
                return std::unexpected(RuntimeError{VMError{
                    RuntimeErrorCode::TypeError, "/: division by zero"}});
            }
            return make_flonum(1.0 / d);
        }

        bool use_float = first.is_flonum();
        int64_t inum = first.int_val;
        double fnum = first.as_double();
        for (size_t i = 1; i < args.size(); ++i) {
            auto n = classify_numeric(args[i], heap);
            if (!n.is_valid()) {
                return std::unexpected(RuntimeError{VMError{
                    RuntimeErrorCode::TypeError, "/: argument is not a number"}});
            }
            if (n.as_double() == 0.0) {
                return std::unexpected(RuntimeError{VMError{
                    RuntimeErrorCode::TypeError, "/: division by zero"}});
            }
            if (use_float || n.is_flonum()) {
                if (!use_float) {
                    fnum = static_cast<double>(inum);
                    use_float = true;
                }
                fnum /= n.as_double();
            } else if (inum % n.int_val != 0) {
                use_float = true;
                fnum = static_cast<double>(inum)
                    / static_cast<double>(n.int_val);
            } else {
                inum /= n.int_val;
            }
        }
        if (use_float) return make_flonum(fnum);
        return make_fixnum(heap, inum);
    });

    /**
     * Comparison: = < > <= >=
     */

    env.register_builtin(
        "=", 2, true,
        detail::core_primitives_arithmetic::make_comparison_primitive(
            heap, vm, "=",
            [](int64_t a, int64_t b) { return a == b; },
            [](double a, double b) { return a == b; }));
    env.register_builtin(
        "<", 2, true,
        detail::core_primitives_arithmetic::make_comparison_primitive(
            heap, vm, "<",
            [](int64_t a, int64_t b) { return a < b; },
            [](double a, double b) { return a < b; }));
    env.register_builtin(
        ">", 2, true,
        detail::core_primitives_arithmetic::make_comparison_primitive(
            heap, vm, ">",
            [](int64_t a, int64_t b) { return a > b; },
            [](double a, double b) { return a > b; }));
    env.register_builtin(
        "<=", 2, true,
        detail::core_primitives_arithmetic::make_comparison_primitive(
            heap, vm, "<=",
            [](int64_t a, int64_t b) { return a <= b; },
            [](double a, double b) { return a <= b; }));
    env.register_builtin(
        ">=", 2, true,
        detail::core_primitives_arithmetic::make_comparison_primitive(
            heap, vm, ">=",
            [](int64_t a, int64_t b) { return a >= b; },
            [](double a, double b) { return a >= b; }));

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

            auto best = classify_numeric(args[0], heap);
            if (!best.is_valid()) {
                return std::unexpected(RuntimeError{VMError{
                    RuntimeErrorCode::TypeError, "min: argument is not a number"}});
            }
            bool use_float = best.is_flonum();
            for (size_t i = 1; i < args.size(); ++i) {
                auto n = classify_numeric(args[i], heap);
                if (!n.is_valid()) {
                    return std::unexpected(RuntimeError{VMError{
                        RuntimeErrorCode::TypeError, "min: argument is not a number"}});
                }
                if (n.is_flonum() || use_float) {
                    use_float = true;
                    if (n.as_double() < best.as_double()) best = n;
                } else if (n.int_val < best.int_val) {
                    best = n;
                }
            }
            if (use_float) return make_flonum(best.as_double());
            return make_fixnum(heap, best.int_val);
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

            auto best = classify_numeric(args[0], heap);
            if (!best.is_valid()) {
                return std::unexpected(RuntimeError{VMError{
                    RuntimeErrorCode::TypeError, "max: argument is not a number"}});
            }
            bool use_float = best.is_flonum();
            for (size_t i = 1; i < args.size(); ++i) {
                auto n = classify_numeric(args[i], heap);
                if (!n.is_valid()) {
                    return std::unexpected(RuntimeError{VMError{
                        RuntimeErrorCode::TypeError, "max: argument is not a number"}});
                }
                if (n.is_flonum() || use_float) {
                    use_float = true;
                    if (n.as_double() > best.as_double()) best = n;
                } else if (n.int_val > best.int_val) {
                    best = n;
                }
            }
            if (use_float) return make_flonum(best.as_double());
            return make_fixnum(heap, best.int_val);
        });

    env.register_builtin("modulo", 2, false, [&heap](Args args) -> std::expected<LispVal, RuntimeError> {
        auto a = classify_numeric(args[0], heap);
        auto b = classify_numeric(args[1], heap);
        if (!a.is_valid() || !b.is_valid()) {
            return std::unexpected(RuntimeError{VMError{
                RuntimeErrorCode::TypeError,
                "modulo: arguments must be numbers"}});
        }
        if (a.is_flonum() || b.is_flonum()) {
            return make_flonum(std::fmod(a.as_double(), b.as_double()));
        }
        if (b.int_val == 0) {
            return std::unexpected(RuntimeError{VMError{
                RuntimeErrorCode::TypeError, "modulo: division by zero"}});
        }
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
        if (a.is_flonum() || b.is_flonum()) {
            return make_flonum(std::remainder(a.as_double(), b.as_double()));
        }
        if (b.int_val == 0) {
            return std::unexpected(RuntimeError{VMError{
                RuntimeErrorCode::TypeError, "remainder: division by zero"}});
        }
        return make_fixnum(heap, a.int_val % b.int_val);
    });
}

} // namespace eta::runtime
