 #pragma once

#include <climits>
#include <cmath>
#include <functional>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>

#include "eta/runtime/builtin_env.h"
#include "eta/runtime/numeric_value.h"
#include "eta/runtime/overflow.h"
#include "eta/runtime/factory.h"
#include "eta/runtime/string_view.h"
#include "eta/runtime/value_formatter.h"
#include "eta/runtime/vm/vm.h"
#include "eta/runtime/types/logic_var.h"
#include "eta/runtime/types/tape.h"
#include "eta/runtime/clp/domain.h"
#include "eta/runtime/clp/constraint_store.h"
#include "eta/runtime/clp/alldiff_regin.h"
#include "eta/runtime/stats_math.h"
#include "eta/runtime/stats_extract.h"

namespace eta::runtime {


/**
 * @brief Register all core primitives into a BuiltinEnvironment
 *
 * Primitives registered (in order — determines global slot indices):
 *
 *  Arithmetic:   +  -  *  /
 *  Comparison:   =  <  >  <=  >=
 *  Equivalence:  eq?  eqv?  not
 *  Pairs/Lists:  cons  car  cdr  pair?  null?  list
 *  Type preds:   number?  boolean?  string?  char?  symbol?  procedure?  integer?
 *  Numeric:      zero?  positive?  negative?  abs  min  max  modulo  remainder
 *  Lists:        length  append  reverse  list-ref  list-tail  set-car!  set-cdr!
 *  Association:  assq  assoc  member
 *  Higher-order: apply  map  for-each
 *  Equality:     equal?
 *  Strings:      string-length  string-append  number->string  string->number  string-ref  substring
 *  Symbols:      symbol->string  string->symbol
 *  Vectors:      vector  vector-length  vector-ref  vector-set!  vector?  make-vector
 *  Error:        error
 *  Platform:     platform
 *
 * Note: I/O primitives (display, write, newline) are in io_primitives.h
 * and require VM access for port support.
 *
 * @param vm  Optional pointer to the running VM.  When provided, map and
 *            for-each use a VM trampoline so they work correctly with user
 *            closures.  When nullptr they fall back to primitive-only mode
 *            (closures will return an error — suitable for analysis-only usage).
 *
 * All primitives capture Heap& and/or InternTable& by reference where needed.
 */
inline void register_core_primitives(BuiltinEnvironment& env, Heap& heap, InternTable& intern_table,
                                     vm::VM* vm = nullptr) {
    using Args = const std::vector<LispVal>&;

    // AD TapeRef helper
    // Helper: check whether any element is a TapeRef
    auto has_tape_ref = [](Args args) -> bool {
        for (auto v : args) {
            if (ops::is_boxed(v) && ops::tag(v) == Tag::TapeRef)
                return true;
        }
        return false;
    };

    // ========================================================================
    // Arithmetic: + - * /
    //
    // Each operator checks for TapeRef arguments.  When found, the operation
    // is folded through VM::tape_binary_op() which delegates to
    // do_binary_arithmetic() — the handler that transparently records tape
    // operations for reverse-mode AD.
    // ========================================================================

    env.register_builtin("+", 0, true, [&heap, vm, has_tape_ref](Args args) -> std::expected<LispVal, RuntimeError> {
        if (vm && has_tape_ref(args)) {
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
            if (!n.is_valid()) return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError, "+: argument is not a number"}});
            if (n.is_flonum() || use_float) {
                if (!use_float) { fsum = static_cast<double>(isum); use_float = true; }
                fsum += n.as_double();
            } else {
                // Overflow check: promote to double on overflow
                int64_t new_sum;
                if (detail::add_overflow(isum, n.int_val, &new_sum)) {
                    use_float = true;
                    fsum = static_cast<double>(isum) + static_cast<double>(n.int_val);
                } else {
                    isum = new_sum;
                }
            }
        }
        if (use_float) return make_flonum(fsum);
        return make_fixnum(heap, isum);
    });

    env.register_builtin("-", 1, true, [&heap, vm, has_tape_ref](Args args) -> std::expected<LispVal, RuntimeError> {
        if (args.empty()) return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::InvalidArity, "-: requires at least 1 argument"}});
        if (vm && has_tape_ref(args)) {
            if (args.size() == 1) {
                // Unary negation: 0 - x
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
        if (!first.is_valid()) return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError, "-: argument is not a number"}});

        if (args.size() == 1) {
            // Unary negation
            if (first.is_flonum()) return make_flonum(-first.float_val);
            return make_fixnum(heap, -first.int_val);
        }

        bool use_float = first.is_flonum();
        int64_t iresult = first.int_val;
        double fresult = first.as_double();
        for (size_t i = 1; i < args.size(); ++i) {
            auto n = classify_numeric(args[i], heap);
            if (!n.is_valid()) return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError, "-: argument is not a number"}});
            if (n.is_flonum() || use_float) {
                if (!use_float) { fresult = static_cast<double>(iresult); use_float = true; }
                fresult -= n.as_double();
            } else {
                int64_t new_result;
                if (detail::sub_overflow(iresult, n.int_val, &new_result)) {
                    use_float = true;
                    fresult = static_cast<double>(iresult) - static_cast<double>(n.int_val);
                } else {
                    iresult = new_result;
                }
            }
        }
        if (use_float) return make_flonum(fresult);
        return make_fixnum(heap, iresult);
    });

    env.register_builtin("*", 0, true, [&heap, vm, has_tape_ref](Args args) -> std::expected<LispVal, RuntimeError> {
        if (vm && has_tape_ref(args)) {
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
            if (!n.is_valid()) return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError, "*: argument is not a number"}});
            if (n.is_flonum() || use_float) {
                if (!use_float) { fprod = static_cast<double>(iprod); use_float = true; }
                fprod *= n.as_double();
            } else {
                int64_t new_prod;
                if (detail::mul_overflow(iprod, n.int_val, &new_prod)) {
                    use_float = true;
                    fprod = static_cast<double>(iprod) * static_cast<double>(n.int_val);
                } else {
                    iprod = new_prod;
                }
            }
        }
        if (use_float) return make_flonum(fprod);
        return make_fixnum(heap, iprod);
    });

    env.register_builtin("/", 1, true, [&heap, vm, has_tape_ref](Args args) -> std::expected<LispVal, RuntimeError> {
        if (args.empty()) return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::InvalidArity, "/: requires at least 1 argument"}});
        if (vm && has_tape_ref(args)) {
            if (args.size() == 1) {
                // Unary reciprocal: 1 / x
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
        if (!first.is_valid()) return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError, "/: argument is not a number"}});

        if (args.size() == 1) {
            // Unary: reciprocal
            double d = first.as_double();
            if (d == 0.0) return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError, "/: division by zero"}});
            return make_flonum(1.0 / d);
        }

        bool use_float = first.is_flonum();
        int64_t inum = first.int_val;
        double fnum = first.as_double();
        for (size_t i = 1; i < args.size(); ++i) {
            auto n = classify_numeric(args[i], heap);
            if (!n.is_valid()) return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError, "/: argument is not a number"}});
            if (n.as_double() == 0.0) return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError, "/: division by zero"}});
            if (use_float || n.is_flonum()) {
                if (!use_float) { fnum = static_cast<double>(inum); use_float = true; }
                fnum /= n.as_double();
            } else {
                if (inum % n.int_val != 0) {
                    // Non-exact: promote to double
                    use_float = true;
                    fnum = static_cast<double>(inum) / static_cast<double>(n.int_val);
                } else {
                    inum /= n.int_val;
                }
            }
        }
        if (use_float) return make_flonum(fnum);
        return make_fixnum(heap, inum);
    });

    // ========================================================================
    // Comparison: = < > <= >=
    // ========================================================================

    auto make_comparison = [&heap](const char* name, auto cmp_int, auto cmp_float) {
        return [&heap, name, cmp_int, cmp_float](Args args) -> std::expected<LispVal, RuntimeError> {
            if (args.size() < 2) return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::InvalidArity, std::string(name) + ": requires at least 2 arguments"}});
            for (size_t i = 0; i + 1 < args.size(); ++i) {
                auto a = classify_numeric(args[i], heap);
                auto b = classify_numeric(args[i + 1], heap);
                if (!a.is_valid() || !b.is_valid()) return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError, std::string(name) + ": argument is not a number"}});
                bool result;
                if (a.is_flonum() || b.is_flonum()) {
                    result = cmp_float(a.as_double(), b.as_double());
                } else {
                    result = cmp_int(a.int_val, b.int_val);
                }
                if (!result) return nanbox::False;
            }
            return nanbox::True;
        };
    };

    env.register_builtin("=", 2, true,
        make_comparison("=",
            [](int64_t a, int64_t b) { return a == b; },
            [](double a, double b) { return a == b; }));
    env.register_builtin("<", 2, true,
        make_comparison("<",
            [](int64_t a, int64_t b) { return a < b; },
            [](double a, double b) { return a < b; }));
    env.register_builtin(">", 2, true,
        make_comparison(">",
            [](int64_t a, int64_t b) { return a > b; },
            [](double a, double b) { return a > b; }));
    env.register_builtin("<=", 2, true,
        make_comparison("<=",
            [](int64_t a, int64_t b) { return a <= b; },
            [](double a, double b) { return a <= b; }));
    env.register_builtin(">=", 2, true,
        make_comparison(">=",
            [](int64_t a, int64_t b) { return a >= b; },
            [](double a, double b) { return a >= b; }));

    // ========================================================================
    // Equivalence: eq? eqv? not
    // ========================================================================

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
            if (na.is_fixnum() && nb.is_fixnum()) return (na.int_val == nb.int_val) ? nanbox::True : nanbox::False;
            if (na.is_flonum() && nb.is_flonum()) return (na.float_val == nb.float_val) ? nanbox::True : nanbox::False;
        }
        return nanbox::False;
    });

    env.register_builtin("not", 1, false, [](Args args) -> std::expected<LispVal, RuntimeError> {
        return (args[0] == nanbox::False) ? nanbox::True : nanbox::False;
    });

    // ========================================================================
    // Pairs / Lists: cons car cdr pair? null? list
    // ========================================================================

    env.register_builtin("cons", 2, false, [&heap](Args args) -> std::expected<LispVal, RuntimeError> {
        return make_cons(heap, args[0], args[1]);
    });

    env.register_builtin("car", 1, false, [&heap](Args args) -> std::expected<LispVal, RuntimeError> {
        if (!ops::is_boxed(args[0]) || ops::tag(args[0]) != Tag::HeapObject) {
            return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError, "car: not a pair"}});
        }
        auto* cons = heap.try_get_as<ObjectKind::Cons, types::Cons>(ops::payload(args[0]));
        if (!cons) {
            return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError, "car: not a pair"}});
        }
        return cons->car;
    });

    env.register_builtin("cdr", 1, false, [&heap](Args args) -> std::expected<LispVal, RuntimeError> {
        if (!ops::is_boxed(args[0]) || ops::tag(args[0]) != Tag::HeapObject) {
            return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError, "cdr: not a pair"}});
        }
        auto* cons = heap.try_get_as<ObjectKind::Cons, types::Cons>(ops::payload(args[0]));
        if (!cons) return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError, "cdr: not a pair"}});
        return cons->cdr;
    });

    env.register_builtin("pair?", 1, false, [&heap](Args args) -> std::expected<LispVal, RuntimeError> {
        if (!ops::is_boxed(args[0]) || ops::tag(args[0]) != Tag::HeapObject) return nanbox::False;
        return heap.try_get_as<ObjectKind::Cons, types::Cons>(ops::payload(args[0])) ? nanbox::True : nanbox::False;
    });

    env.register_builtin("null?", 1, false, [](Args args) -> std::expected<LispVal, RuntimeError> {
        return (args[0] == nanbox::Nil) ? nanbox::True : nanbox::False;
    });

    env.register_builtin("list", 0, true, [&heap](Args args) -> std::expected<LispVal, RuntimeError> {
        LispVal result = nanbox::Nil;
        auto roots = heap.make_external_root_frame();
        for (auto v : args) roots.push(v);
        for (auto it = args.rbegin(); it != args.rend(); ++it) {
            auto cons = make_cons(heap, *it, result);
            if (!cons) return cons;
            result = *cons;
            roots.push(result);
        }
        return result;
    });

    // ========================================================================
    // Type predicates
    // ========================================================================

    env.register_builtin("number?", 1, false, [&heap](Args args) -> std::expected<LispVal, RuntimeError> {
        return classify_numeric(args[0], heap).is_valid() ? nanbox::True : nanbox::False;
    });

    env.register_builtin("boolean?", 1, false, [](Args args) -> std::expected<LispVal, RuntimeError> {
        return (args[0] == nanbox::True || args[0] == nanbox::False) ? nanbox::True : nanbox::False;
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
        if (!ops::is_boxed(args[0]) || ops::tag(args[0]) != Tag::HeapObject) return nanbox::False;
        auto id = ops::payload(args[0]);
        if (heap.try_get_as<ObjectKind::Closure, types::Closure>(id)) return nanbox::True;
        if (heap.try_get_as<ObjectKind::Primitive, types::Primitive>(id)) return nanbox::True;
        return nanbox::False;
    });

    env.register_builtin("integer?", 1, false, [&heap](Args args) -> std::expected<LispVal, RuntimeError> {
        auto n = classify_numeric(args[0], heap);
        if (n.is_fixnum()) return nanbox::True;
        if (n.is_flonum()) {
            double d = n.float_val;
            return (std::isfinite(d) && d == std::floor(d)) ? nanbox::True : nanbox::False;
        }
        return nanbox::False;
    });

    // ========================================================================
    // Note: I/O primitives (display, newline) have been moved to io_primitives.h
    // They now support port-based output and require VM access.
    // ========================================================================


    // ========================================================================
    // Numeric predicates: zero? positive? negative?
    // ========================================================================

    env.register_builtin("zero?", 1, false, [&heap](Args args) -> std::expected<LispVal, RuntimeError> {
        auto n = classify_numeric(args[0], heap);
        if (!n.is_valid()) return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError, "zero?: argument is not a number"}});
        if (n.is_flonum()) return (n.float_val == 0.0) ? nanbox::True : nanbox::False;
        return (n.int_val == 0) ? nanbox::True : nanbox::False;
    });

    env.register_builtin("positive?", 1, false, [&heap](Args args) -> std::expected<LispVal, RuntimeError> {
        auto n = classify_numeric(args[0], heap);
        if (!n.is_valid()) return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError, "positive?: argument is not a number"}});
        if (n.is_flonum()) return (n.float_val > 0.0) ? nanbox::True : nanbox::False;
        return (n.int_val > 0) ? nanbox::True : nanbox::False;
    });

    env.register_builtin("negative?", 1, false, [&heap](Args args) -> std::expected<LispVal, RuntimeError> {
        auto n = classify_numeric(args[0], heap);
        if (!n.is_valid()) return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError, "negative?: argument is not a number"}});
        if (n.is_flonum()) return (n.float_val < 0.0) ? nanbox::True : nanbox::False;
        return (n.int_val < 0) ? nanbox::True : nanbox::False;
    });

    // ========================================================================
    // Numeric operations: abs min max modulo remainder
    // ========================================================================

    env.register_builtin("abs", 1, false, [&heap](Args args) -> std::expected<LispVal, RuntimeError> {
        auto n = classify_numeric(args[0], heap);
        if (!n.is_valid()) return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError, "abs: argument is not a number"}});
        if (n.is_flonum()) return make_flonum(std::abs(n.float_val));
        return make_fixnum(heap, n.int_val < 0 ? -n.int_val : n.int_val);
    });

    env.register_builtin("min", 2, true, [&heap](Args args) -> std::expected<LispVal, RuntimeError> {
        auto best = classify_numeric(args[0], heap);
        if (!best.is_valid()) return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError, "min: argument is not a number"}});
        bool use_float = best.is_flonum();
        for (size_t i = 1; i < args.size(); ++i) {
            auto n = classify_numeric(args[i], heap);
            if (!n.is_valid()) return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError, "min: argument is not a number"}});
            if (n.is_flonum() || use_float) {
                use_float = true;
                if (n.as_double() < best.as_double()) best = n;
            } else {
                if (n.int_val < best.int_val) best = n;
            }
        }
        if (use_float) return make_flonum(best.as_double());
        return make_fixnum(heap, best.int_val);
    });

    env.register_builtin("max", 2, true, [&heap](Args args) -> std::expected<LispVal, RuntimeError> {
        auto best = classify_numeric(args[0], heap);
        if (!best.is_valid()) return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError, "max: argument is not a number"}});
        bool use_float = best.is_flonum();
        for (size_t i = 1; i < args.size(); ++i) {
            auto n = classify_numeric(args[i], heap);
            if (!n.is_valid()) return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError, "max: argument is not a number"}});
            if (n.is_flonum() || use_float) {
                use_float = true;
                if (n.as_double() > best.as_double()) best = n;
            } else {
                if (n.int_val > best.int_val) best = n;
            }
        }
        if (use_float) return make_flonum(best.as_double());
        return make_fixnum(heap, best.int_val);
    });

    env.register_builtin("modulo", 2, false, [&heap](Args args) -> std::expected<LispVal, RuntimeError> {
        auto a = classify_numeric(args[0], heap);
        auto b = classify_numeric(args[1], heap);
        if (!a.is_valid() || !b.is_valid()) return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError, "modulo: arguments must be numbers"}});
        if (a.is_flonum() || b.is_flonum()) return make_flonum(std::fmod(a.as_double(), b.as_double()));
        if (b.int_val == 0) return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError, "modulo: division by zero"}});
        // Scheme modulo: result has same sign as divisor
        int64_t r = a.int_val % b.int_val;
        if (r != 0 && ((r < 0) != (b.int_val < 0))) r += b.int_val;
        return make_fixnum(heap, r);
    });

    env.register_builtin("remainder", 2, false, [&heap](Args args) -> std::expected<LispVal, RuntimeError> {
        auto a = classify_numeric(args[0], heap);
        auto b = classify_numeric(args[1], heap);
        if (!a.is_valid() || !b.is_valid()) return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError, "remainder: arguments must be numbers"}});
        if (a.is_flonum() || b.is_flonum()) return make_flonum(std::remainder(a.as_double(), b.as_double()));
        if (b.int_val == 0) return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError, "remainder: division by zero"}});
        return make_fixnum(heap, a.int_val % b.int_val);
    });

    // ========================================================================
    // Transcendental math: sin cos tan asin acos atan atan2 exp log sqrt
    // ========================================================================

    env.register_builtin("sin", 1, false, [&heap, vm](Args args) -> std::expected<LispVal, RuntimeError> {
        // Tape-aware: record sin on active tape
        if (vm && ops::is_boxed(args[0]) && ops::tag(args[0]) == Tag::TapeRef) {
            auto* tape = heap.try_get_as<ObjectKind::Tape, types::Tape>(ops::payload(vm->active_tape()));
            if (!tape) return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError, "sin: no active tape"}});
            auto idx = static_cast<uint32_t>(ops::payload(args[0]));
            double val = std::sin(tape->entries[idx].primal);
            uint32_t new_idx = tape->push({types::TapeOp::Sin, idx, 0, val, 0.0});
            return ops::box(Tag::TapeRef, new_idx);
        }
        auto n = classify_numeric(args[0], heap);
        if (!n.is_valid()) return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError, "sin: argument is not a number"}});
        return make_flonum(std::sin(n.as_double()));
    });

    env.register_builtin("cos", 1, false, [&heap, vm](Args args) -> std::expected<LispVal, RuntimeError> {
        // Tape-aware: record cos on active tape
        if (vm && ops::is_boxed(args[0]) && ops::tag(args[0]) == Tag::TapeRef) {
            auto* tape = heap.try_get_as<ObjectKind::Tape, types::Tape>(ops::payload(vm->active_tape()));
            if (!tape) return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError, "cos: no active tape"}});
            auto idx = static_cast<uint32_t>(ops::payload(args[0]));
            double val = std::cos(tape->entries[idx].primal);
            uint32_t new_idx = tape->push({types::TapeOp::Cos, idx, 0, val, 0.0});
            return ops::box(Tag::TapeRef, new_idx);
        }
        auto n = classify_numeric(args[0], heap);
        if (!n.is_valid()) return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError, "cos: argument is not a number"}});
        return make_flonum(std::cos(n.as_double()));
    });

    env.register_builtin("tan", 1, false, [&heap](Args args) -> std::expected<LispVal, RuntimeError> {
        auto n = classify_numeric(args[0], heap);
        if (!n.is_valid()) return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError, "tan: argument is not a number"}});
        return make_flonum(std::tan(n.as_double()));
    });

    env.register_builtin("asin", 1, false, [&heap](Args args) -> std::expected<LispVal, RuntimeError> {
        auto n = classify_numeric(args[0], heap);
        if (!n.is_valid()) return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError, "asin: argument is not a number"}});
        return make_flonum(std::asin(n.as_double()));
    });

    env.register_builtin("acos", 1, false, [&heap](Args args) -> std::expected<LispVal, RuntimeError> {
        auto n = classify_numeric(args[0], heap);
        if (!n.is_valid()) return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError, "acos: argument is not a number"}});
        return make_flonum(std::acos(n.as_double()));
    });

    env.register_builtin("atan", 1, true, [&heap](Args args) -> std::expected<LispVal, RuntimeError> {
        auto a = classify_numeric(args[0], heap);
        if (!a.is_valid()) return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError, "atan: argument is not a number"}});
        if (args.size() == 2) {
            auto b = classify_numeric(args[1], heap);
            if (!b.is_valid()) return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError, "atan: argument is not a number"}});
            return make_flonum(std::atan2(a.as_double(), b.as_double()));
        }
        return make_flonum(std::atan(a.as_double()));
    });

    env.register_builtin("exp", 1, false, [&heap, vm](Args args) -> std::expected<LispVal, RuntimeError> {
        // Tape-aware: record exp on active tape
        if (vm && ops::is_boxed(args[0]) && ops::tag(args[0]) == Tag::TapeRef) {
            auto* tape = heap.try_get_as<ObjectKind::Tape, types::Tape>(ops::payload(vm->active_tape()));
            if (!tape) return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError, "exp: no active tape"}});
            auto idx = static_cast<uint32_t>(ops::payload(args[0]));
            double val = std::exp(tape->entries[idx].primal);
            uint32_t new_idx = tape->push({types::TapeOp::Exp, idx, 0, val, 0.0});
            return ops::box(Tag::TapeRef, new_idx);
        }
        auto n = classify_numeric(args[0], heap);
        if (!n.is_valid()) return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError, "exp: argument is not a number"}});
        return make_flonum(std::exp(n.as_double()));
    });

    env.register_builtin("log", 1, false, [&heap, vm](Args args) -> std::expected<LispVal, RuntimeError> {
        // Tape-aware: record log on active tape
        if (vm && ops::is_boxed(args[0]) && ops::tag(args[0]) == Tag::TapeRef) {
            auto* tape = heap.try_get_as<ObjectKind::Tape, types::Tape>(ops::payload(vm->active_tape()));
            if (!tape) return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError, "log: no active tape"}});
            auto idx = static_cast<uint32_t>(ops::payload(args[0]));
            double val = std::log(tape->entries[idx].primal);
            uint32_t new_idx = tape->push({types::TapeOp::Log, idx, 0, val, 0.0});
            return ops::box(Tag::TapeRef, new_idx);
        }
        auto n = classify_numeric(args[0], heap);
        if (!n.is_valid()) return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError, "log: argument is not a number"}});
        return make_flonum(std::log(n.as_double()));
    });

    env.register_builtin("sqrt", 1, false, [&heap, vm](Args args) -> std::expected<LispVal, RuntimeError> {
        // Tape-aware: record sqrt on active tape
        if (vm && ops::is_boxed(args[0]) && ops::tag(args[0]) == Tag::TapeRef) {
            auto* tape = heap.try_get_as<ObjectKind::Tape, types::Tape>(ops::payload(vm->active_tape()));
            if (!tape) return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError, "sqrt: no active tape"}});
            auto idx = static_cast<uint32_t>(ops::payload(args[0]));
            double val = std::sqrt(tape->entries[idx].primal);
            uint32_t new_idx = tape->push({types::TapeOp::Sqrt, idx, 0, val, 0.0});
            return ops::box(Tag::TapeRef, new_idx);
        }
        auto n = classify_numeric(args[0], heap);
        if (!n.is_valid()) return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError, "sqrt: argument is not a number"}});
        return make_flonum(std::sqrt(n.as_double()));
    });

    // ========================================================================
    // List operations: length append reverse list-ref
    // ========================================================================

    env.register_builtin("length", 1, false, [&heap](Args args) -> std::expected<LispVal, RuntimeError> {
        int64_t count = 0;
        LispVal cur = args[0];
        while (cur != nanbox::Nil) {
            if (!ops::is_boxed(cur) || ops::tag(cur) != Tag::HeapObject)
                return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError, "length: not a proper list"}});
            auto* cons = heap.try_get_as<ObjectKind::Cons, types::Cons>(ops::payload(cur));
            if (!cons) return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError, "length: not a proper list"}});
            ++count;
            cur = cons->cdr;
        }
        return make_fixnum(heap, count);
    });

    env.register_builtin("append", 0, true, [&heap](Args args) -> std::expected<LispVal, RuntimeError> {
        if (args.empty()) return nanbox::Nil;
        if (args.size() == 1) return args[0];

        // Start from the last argument (which doesn't need to be a list)
        LispVal result = args.back();

        // Process arguments right-to-left (all but the last must be proper lists)
        for (auto it = args.rbegin() + 1; it != args.rend(); ++it) {
            // Collect elements of this list
            std::vector<LispVal> elems;
            LispVal cur = *it;
            while (cur != nanbox::Nil) {
                if (!ops::is_boxed(cur) || ops::tag(cur) != Tag::HeapObject)
                    return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError, "append: not a proper list"}});
                auto* cons = heap.try_get_as<ObjectKind::Cons, types::Cons>(ops::payload(cur));
                if (!cons) return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError, "append: not a proper list"}});
                elems.push_back(cons->car);
                cur = cons->cdr;
            }
            // Build from right to left
            for (auto rit = elems.rbegin(); rit != elems.rend(); ++rit) {
                auto cons = make_cons(heap, *rit, result);
                if (!cons) return cons;
                result = *cons;
            }
        }
        return result;
    });

    env.register_builtin("reverse", 1, false, [&heap](Args args) -> std::expected<LispVal, RuntimeError> {
        LispVal result = nanbox::Nil;
        LispVal cur = args[0];
        while (cur != nanbox::Nil) {
            if (!ops::is_boxed(cur) || ops::tag(cur) != Tag::HeapObject)
                return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError, "reverse: not a proper list"}});
            auto* cons = heap.try_get_as<ObjectKind::Cons, types::Cons>(ops::payload(cur));
            if (!cons) return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError, "reverse: not a proper list"}});
            auto new_cons = make_cons(heap, cons->car, result);
            if (!new_cons) return new_cons;
            result = *new_cons;
            cur = cons->cdr;
        }
        return result;
    });

    env.register_builtin("list-ref", 2, false, [&heap](Args args) -> std::expected<LispVal, RuntimeError> {
        auto idx = classify_numeric(args[1], heap);
        if (!idx.is_valid() || idx.is_flonum() || idx.int_val < 0)
            return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError, "list-ref: index must be a non-negative integer"}});
        LispVal cur = args[0];
        for (int64_t i = 0; i < idx.int_val; ++i) {
            if (!ops::is_boxed(cur) || ops::tag(cur) != Tag::HeapObject)
                return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError, "list-ref: index out of bounds"}});
            auto* cons = heap.try_get_as<ObjectKind::Cons, types::Cons>(ops::payload(cur));
            if (!cons) return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError, "list-ref: index out of bounds"}});
            cur = cons->cdr;
        }
        if (!ops::is_boxed(cur) || ops::tag(cur) != Tag::HeapObject)
            return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError, "list-ref: index out of bounds"}});
        auto* cons = heap.try_get_as<ObjectKind::Cons, types::Cons>(ops::payload(cur));
        if (!cons) return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError, "list-ref: index out of bounds"}});
        return cons->car;
    });

    env.register_builtin("list-tail", 2, false, [&heap](Args args) -> std::expected<LispVal, RuntimeError> {
        auto idx = classify_numeric(args[1], heap);
        if (!idx.is_valid() || idx.is_flonum() || idx.int_val < 0)
            return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError, "list-tail: index must be a non-negative integer"}});
        LispVal cur = args[0];
        for (int64_t i = 0; i < idx.int_val; ++i) {
            if (!ops::is_boxed(cur) || ops::tag(cur) != Tag::HeapObject)
                return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError, "list-tail: index out of bounds"}});
            auto* cons = heap.try_get_as<ObjectKind::Cons, types::Cons>(ops::payload(cur));
            if (!cons) return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError, "list-tail: index out of bounds"}});
            cur = cons->cdr;
        }
        return cur;
    });

    env.register_builtin("set-car!", 2, false, [&heap](Args args) -> std::expected<LispVal, RuntimeError> {
        if (!ops::is_boxed(args[0]) || ops::tag(args[0]) != Tag::HeapObject)
            return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError, "set-car!: not a pair"}});
        auto* cons = heap.try_get_as<ObjectKind::Cons, types::Cons>(ops::payload(args[0]));
        if (!cons) return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError, "set-car!: not a pair"}});
        cons->car = args[1];
        return nanbox::Nil;
    });

    env.register_builtin("set-cdr!", 2, false, [&heap](Args args) -> std::expected<LispVal, RuntimeError> {
        if (!ops::is_boxed(args[0]) || ops::tag(args[0]) != Tag::HeapObject)
            return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError, "set-cdr!: not a pair"}});
        auto* cons = heap.try_get_as<ObjectKind::Cons, types::Cons>(ops::payload(args[0]));
        if (!cons) return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError, "set-cdr!: not a pair"}});
        cons->cdr = args[1];
        return nanbox::Nil;
    });

    env.register_builtin("assq", 2, false, [&heap](Args args) -> std::expected<LispVal, RuntimeError> {
        // (assq key alist) — identity (eq?) comparison on car of each pair
        LispVal key = args[0];
        LispVal cur = args[1];
        while (cur != nanbox::Nil) {
            if (!ops::is_boxed(cur) || ops::tag(cur) != Tag::HeapObject)
                return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError, "assq: not a proper alist"}});
            auto* outer = heap.try_get_as<ObjectKind::Cons, types::Cons>(ops::payload(cur));
            if (!outer) return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError, "assq: not a proper alist"}});
            // Each element must be a pair
            LispVal pair = outer->car;
            if (ops::is_boxed(pair) && ops::tag(pair) == Tag::HeapObject) {
                auto* inner = heap.try_get_as<ObjectKind::Cons, types::Cons>(ops::payload(pair));
                if (inner && inner->car == key) {
                    return pair; // Return the whole pair
                }
            }
            cur = outer->cdr;
        }
        return nanbox::False;
    });

    env.register_builtin("assoc", 2, false, [&heap, &intern_table](Args args) -> std::expected<LispVal, RuntimeError> {
        // (assoc key alist) — structural (equal?) comparison on car of each pair
        LispVal key = args[0];
        LispVal cur = args[1];

        std::function<bool(LispVal, LispVal)> equal_impl = [&](LispVal a, LispVal b) -> bool {
            if (a == b) return true;
            if (!ops::is_boxed(a) || !ops::is_boxed(b)) return false;
            if (ops::tag(a) == Tag::String && ops::tag(b) == Tag::String) {
                auto sa = StringView::try_from(a, intern_table);
                auto sb = StringView::try_from(b, intern_table);
                if (sa && sb) return sa->view() == sb->view();
                return false;
            }
            if (ops::tag(a) != Tag::HeapObject || ops::tag(b) != Tag::HeapObject) return false;
            auto na = classify_numeric(a, heap);
            auto nb = classify_numeric(b, heap);
            if (na.is_valid() && nb.is_valid()) {
                if (na.is_flonum() || nb.is_flonum()) return na.as_double() == nb.as_double();
                return na.int_val == nb.int_val;
            }
            return false;
        };

        while (cur != nanbox::Nil) {
            if (!ops::is_boxed(cur) || ops::tag(cur) != Tag::HeapObject)
                return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError, "assoc: not a proper alist"}});
            auto* outer = heap.try_get_as<ObjectKind::Cons, types::Cons>(ops::payload(cur));
            if (!outer) return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError, "assoc: not a proper alist"}});
            LispVal pair = outer->car;
            if (ops::is_boxed(pair) && ops::tag(pair) == Tag::HeapObject) {
                auto* inner = heap.try_get_as<ObjectKind::Cons, types::Cons>(ops::payload(pair));
                if (inner && equal_impl(inner->car, key)) {
                    return pair;
                }
            }
            cur = outer->cdr;
        }
        return nanbox::False;
    });

    env.register_builtin("member", 2, false, [&heap](Args args) -> std::expected<LispVal, RuntimeError> {
        // (member obj list) — eq? comparison, returns sublist starting at match or #f
        LispVal obj = args[0];
        LispVal cur = args[1];
        while (cur != nanbox::Nil) {
            if (!ops::is_boxed(cur) || ops::tag(cur) != Tag::HeapObject)
                return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError, "member: not a proper list"}});
            auto* cons = heap.try_get_as<ObjectKind::Cons, types::Cons>(ops::payload(cur));
            if (!cons) return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError, "member: not a proper list"}});
            if (cons->car == obj) return cur;
            cur = cons->cdr;
        }
        return nanbox::False;
    });

    env.register_builtin("symbol->string", 1, false, [&heap, &intern_table](Args args) -> std::expected<LispVal, RuntimeError> {
        auto name = get_symbol_name(args[0], intern_table);
        if (!name) return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError, "symbol->string: not a symbol"}});
        return make_string(heap, intern_table, std::string(*name));
    });

    env.register_builtin("string->symbol", 1, false, [&intern_table](Args args) -> std::expected<LispVal, RuntimeError> {
        auto sv = StringView::try_from(args[0], intern_table);
        if (!sv) return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError, "string->symbol: not a string"}});
        return make_symbol(intern_table, std::string(sv->view()));
    });

    // ========================================================================
    // Higher-order: apply map for-each
    //
    // NOTE: (apply proc arg1 ... list) is now a VM-level special form
    // (OpCode::Apply / TailApply).  The SA intercepts calls to `apply` and
    // emits the dedicated opcode, so this primitive is only reachable when
    // `apply` is used as a first-class value (e.g. passed to another function).
    // That case is not yet supported — it would require a VM trampoline.
    //
    // When a VM pointer is provided both map and for-each use call_value() to
    // invoke any callable (primitive or closure).  Without a VM pointer they
    // fall back to primitive-only mode.
    // ========================================================================

    env.register_builtin("apply", 2, true, [](Args /*args*/) -> std::expected<LispVal, RuntimeError> {
        return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError,
            "apply: cannot be used as a first-class value yet (use direct apply syntax instead)"}});
    });

    env.register_builtin("map", 2, false, [&heap, vm](Args args) -> std::expected<LispVal, RuntimeError> {
        // (map proc list) — single-list version; works with any callable when vm != nullptr
        LispVal proc = args[0];
        if (!ops::is_boxed(proc) || ops::tag(proc) != Tag::HeapObject)
            return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError, "map: first argument must be a procedure"}});

        std::vector<LispVal> results;
        LispVal cur = args[1];
        while (cur != nanbox::Nil) {
            if (!ops::is_boxed(cur) || ops::tag(cur) != Tag::HeapObject)
                return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError, "map: not a proper list"}});
            auto* cons = heap.try_get_as<ObjectKind::Cons, types::Cons>(ops::payload(cur));
            if (!cons) return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError, "map: not a proper list"}});

            std::expected<LispVal, RuntimeError> res;
            if (vm) {
                res = vm->call_value(proc, {cons->car});
            } else {
                auto* prim = heap.try_get_as<ObjectKind::Primitive, types::Primitive>(ops::payload(proc));
                if (!prim)
                    return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError,
                        "map: closures require a VM context (internal error — VM not provided)"}});
                std::vector<LispVal> call_args = {cons->car};
                res = prim->func(call_args);
            }
            if (!res) return res;
            results.push_back(*res);
            cur = cons->cdr;
        }
        // Build result list in order
        LispVal result = nanbox::Nil;
        for (auto it = results.rbegin(); it != results.rend(); ++it) {
            auto cons_val = make_cons(heap, *it, result);
            if (!cons_val) return cons_val;
            result = *cons_val;
        }
        return result;
    });

    env.register_builtin("for-each", 2, false, [&heap, vm](Args args) -> std::expected<LispVal, RuntimeError> {
        // (for-each proc list) — single-list version; works with any callable when vm != nullptr
        LispVal proc = args[0];
        if (!ops::is_boxed(proc) || ops::tag(proc) != Tag::HeapObject)
            return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError, "for-each: first argument must be a procedure"}});

        LispVal cur = args[1];
        while (cur != nanbox::Nil) {
            if (!ops::is_boxed(cur) || ops::tag(cur) != Tag::HeapObject)
                return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError, "for-each: not a proper list"}});
            auto* cons = heap.try_get_as<ObjectKind::Cons, types::Cons>(ops::payload(cur));
            if (!cons) return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError, "for-each: not a proper list"}});

            std::expected<LispVal, RuntimeError> res;
            if (vm) {
                res = vm->call_value(proc, {cons->car});
            } else {
                auto* prim = heap.try_get_as<ObjectKind::Primitive, types::Primitive>(ops::payload(proc));
                if (!prim)
                    return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError,
                        "for-each: closures require a VM context (internal error — VM not provided)"}});
                std::vector<LispVal> call_args = {cons->car};
                res = prim->func(call_args);
            }
            if (!res) return res;
            cur = cons->cdr;
        }
        return nanbox::Nil;
    });

    // ========================================================================
    // Deep equality: equal?
    // ========================================================================

    env.register_builtin("equal?", 2, false, [&heap, &intern_table](Args args) -> std::expected<LispVal, RuntimeError> {
        // Recursive structural equality
        std::function<bool(LispVal, LispVal)> equal_impl = [&](LispVal a, LispVal b) -> bool {
            if (a == b) return true;
            if (!ops::is_boxed(a) || !ops::is_boxed(b)) return false;

            // Both are strings?
            if (ops::tag(a) == Tag::String && ops::tag(b) == Tag::String) {
                auto sa = StringView::try_from(a, intern_table);
                auto sb = StringView::try_from(b, intern_table);
                if (sa && sb) return sa->view() == sb->view();
                return false;
            }

            if (ops::tag(a) != Tag::HeapObject || ops::tag(b) != Tag::HeapObject) return false;

            // Numeric equality
            auto na = classify_numeric(a, heap);
            auto nb = classify_numeric(b, heap);
            if (na.is_valid() && nb.is_valid()) {
                if (na.is_flonum() || nb.is_flonum()) return na.as_double() == nb.as_double();
                return na.int_val == nb.int_val;
            }

            // Cons (pair) equality
            auto* ca = heap.try_get_as<ObjectKind::Cons, types::Cons>(ops::payload(a));
            auto* cb = heap.try_get_as<ObjectKind::Cons, types::Cons>(ops::payload(b));
            if (ca && cb) {
                return equal_impl(ca->car, cb->car) && equal_impl(ca->cdr, cb->cdr);
            }

            // Vector equality
            auto* va = heap.try_get_as<ObjectKind::Vector, types::Vector>(ops::payload(a));
            auto* vb = heap.try_get_as<ObjectKind::Vector, types::Vector>(ops::payload(b));
            if (va && vb) {
                if (va->elements.size() != vb->elements.size()) return false;
                for (size_t i = 0; i < va->elements.size(); ++i) {
                    if (!equal_impl(va->elements[i], vb->elements[i])) return false;
                }
                return true;
            }

            return false;
        };
        return equal_impl(args[0], args[1]) ? nanbox::True : nanbox::False;
    });

    // ========================================================================
    // String operations: string-length string-append string-ref number->string string->number
    // ========================================================================

    env.register_builtin("string-length", 1, false, [&heap, &intern_table](Args args) -> std::expected<LispVal, RuntimeError> {
        auto sv = StringView::try_from(args[0], intern_table);
        if (!sv) return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError, "string-length: not a string"}});
        return make_fixnum(heap, static_cast<int64_t>(sv->view().size()));
    });

    env.register_builtin("string-append", 0, true, [&heap, &intern_table](Args args) -> std::expected<LispVal, RuntimeError> {
        std::string result;
        for (auto v : args) {
            auto sv = StringView::try_from(v, intern_table);
            if (!sv) return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError, "string-append: not a string"}});
            result += sv->view();
        }
        return make_string(heap, intern_table, result);
    });

    env.register_builtin("number->string", 1, false, [&heap, &intern_table](Args args) -> std::expected<LispVal, RuntimeError> {
        auto n = classify_numeric(args[0], heap);
        if (!n.is_valid()) return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError, "number->string: not a number"}});
        std::string s;
        if (n.is_flonum()) {
            std::ostringstream oss; oss << n.float_val; s = oss.str();
        } else {
            s = std::to_string(n.int_val);
        }
        return make_string(heap, intern_table, s);
    });

    env.register_builtin("string->number", 1, false, [&heap, &intern_table](Args args) -> std::expected<LispVal, RuntimeError> {
        auto sv = StringView::try_from(args[0], intern_table);
        if (!sv) return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError, "string->number: not a string"}});
        std::string s(sv->view());
        try {
            size_t pos;
            int64_t i = std::stoll(s, &pos);
            if (pos == s.size()) return make_fixnum(heap, i);
        } catch (...) {}
        try {
            size_t pos;
            double d = std::stod(s, &pos);
            if (pos == s.size()) return make_flonum(d);
        } catch (...) {}
        return nanbox::False; // Scheme convention: return #f on failure
    });

    env.register_builtin("string-ref", 2, false, [&heap, &intern_table](Args args) -> std::expected<LispVal, RuntimeError> {
        auto sv = StringView::try_from(args[0], intern_table);
        if (!sv) return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError, "string-ref: not a string"}});
        auto idx = classify_numeric(args[1], heap);
        if (!idx.is_valid() || idx.is_flonum() || idx.int_val < 0)
            return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError, "string-ref: index must be a non-negative integer"}});
        auto view = sv->view();
        if (static_cast<size_t>(idx.int_val) >= view.size())
            return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError, "string-ref: index out of bounds"}});
        // Return the byte at position as a character (treats string as byte-indexed)
        char32_t ch = static_cast<unsigned char>(view[static_cast<size_t>(idx.int_val)]);
        return ops::encode(ch);
    });

    env.register_builtin("substring", 3, false, [&heap, &intern_table](Args args) -> std::expected<LispVal, RuntimeError> {
        auto sv = StringView::try_from(args[0], intern_table);
        if (!sv) return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError, "substring: not a string"}});
        auto start = classify_numeric(args[1], heap);
        auto end   = classify_numeric(args[2], heap);
        if (!start.is_valid() || start.is_flonum() || start.int_val < 0)
            return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError, "substring: start must be a non-negative integer"}});
        if (!end.is_valid() || end.is_flonum() || end.int_val < 0)
            return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError, "substring: end must be a non-negative integer"}});
        auto view = sv->view();
        auto s = static_cast<size_t>(start.int_val);
        auto e = static_cast<size_t>(end.int_val);
        if (s > view.size() || e > view.size() || s > e)
            return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError, "substring: indices out of bounds"}});
        return make_string(heap, intern_table, std::string(view.substr(s, e - s)));
    });

    env.register_builtin("string=?", 2, false, [&heap, &intern_table](Args args) -> std::expected<LispVal, RuntimeError> {
        auto a = StringView::try_from(args[0], intern_table);
        auto b = StringView::try_from(args[1], intern_table);
        if (!a) return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError, "string=?: first argument is not a string"}});
        if (!b) return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError, "string=?: second argument is not a string"}});
        return (a->view() == b->view()) ? nanbox::True : nanbox::False;
    });

    env.register_builtin("string<?", 2, false, [&heap, &intern_table](Args args) -> std::expected<LispVal, RuntimeError> {
        auto a = StringView::try_from(args[0], intern_table);
        auto b = StringView::try_from(args[1], intern_table);
        if (!a) return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError, "string<?: first argument is not a string"}});
        if (!b) return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError, "string<?: second argument is not a string"}});
        return (a->view() < b->view()) ? nanbox::True : nanbox::False;
    });

    env.register_builtin("string>?", 2, false, [&heap, &intern_table](Args args) -> std::expected<LispVal, RuntimeError> {
        auto a = StringView::try_from(args[0], intern_table);
        auto b = StringView::try_from(args[1], intern_table);
        if (!a) return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError, "string>?: first argument is not a string"}});
        if (!b) return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError, "string>?: second argument is not a string"}});
        return (a->view() > b->view()) ? nanbox::True : nanbox::False;
    });

    env.register_builtin("string<=?", 2, false, [&heap, &intern_table](Args args) -> std::expected<LispVal, RuntimeError> {
        auto a = StringView::try_from(args[0], intern_table);
        auto b = StringView::try_from(args[1], intern_table);
        if (!a) return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError, "string<=?: first argument is not a string"}});
        if (!b) return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError, "string<=?: second argument is not a string"}});
        return (a->view() <= b->view()) ? nanbox::True : nanbox::False;
    });

    env.register_builtin("string>=?", 2, false, [&heap, &intern_table](Args args) -> std::expected<LispVal, RuntimeError> {
        auto a = StringView::try_from(args[0], intern_table);
        auto b = StringView::try_from(args[1], intern_table);
        if (!a) return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError, "string>=?: first argument is not a string"}});
        if (!b) return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError, "string>=?: second argument is not a string"}});
        return (a->view() >= b->view()) ? nanbox::True : nanbox::False;
    });

    env.register_builtin("char->integer", 1, false, [&heap](Args args) -> std::expected<LispVal, RuntimeError> {
        if (!ops::is_boxed(args[0]) || ops::tag(args[0]) != Tag::Char)
            return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError, "char->integer: not a character"}});
        auto ch = ops::decode<char32_t>(args[0]);
        if (!ch) return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError, "char->integer: invalid character"}});
        return make_fixnum(heap, static_cast<int64_t>(*ch));
    });

    env.register_builtin("integer->char", 1, false, [&heap](Args args) -> std::expected<LispVal, RuntimeError> {
        auto n = classify_numeric(args[0], heap);
        if (!n.is_valid() || n.is_flonum() || n.int_val < 0)
            return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError, "integer->char: not a non-negative integer"}});
        auto v = ops::encode(static_cast<char32_t>(n.int_val));
        if (!v) return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError, "integer->char: encoding failed"}});
        return *v;
    });

    // ========================================================================
    // Vector operations: vector vector-length vector-ref vector-set!
    // ========================================================================

    env.register_builtin("vector", 0, true, [&heap](Args args) -> std::expected<LispVal, RuntimeError> {
        std::vector<LispVal> elems(args.begin(), args.end());
        return make_vector(heap, std::move(elems));
    });

    env.register_builtin("vector-length", 1, false, [&heap](Args args) -> std::expected<LispVal, RuntimeError> {
        if (!ops::is_boxed(args[0]) || ops::tag(args[0]) != Tag::HeapObject)
            return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError, "vector-length: not a vector"}});
        auto* vec = heap.try_get_as<ObjectKind::Vector, types::Vector>(ops::payload(args[0]));
        if (!vec) return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError, "vector-length: not a vector"}});
        return make_fixnum(heap, static_cast<int64_t>(vec->elements.size()));
    });

    env.register_builtin("vector-ref", 2, false, [&heap](Args args) -> std::expected<LispVal, RuntimeError> {
        if (!ops::is_boxed(args[0]) || ops::tag(args[0]) != Tag::HeapObject)
            return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError, "vector-ref: not a vector"}});
        auto* vec = heap.try_get_as<ObjectKind::Vector, types::Vector>(ops::payload(args[0]));
        if (!vec) return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError, "vector-ref: not a vector"}});
        auto idx = classify_numeric(args[1], heap);
        if (!idx.is_valid() || idx.is_flonum() || idx.int_val < 0 || static_cast<size_t>(idx.int_val) >= vec->elements.size())
            return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError, "vector-ref: index out of bounds"}});
        return vec->elements[static_cast<size_t>(idx.int_val)];
    });

    env.register_builtin("vector-set!", 3, false, [&heap](Args args) -> std::expected<LispVal, RuntimeError> {
        if (!ops::is_boxed(args[0]) || ops::tag(args[0]) != Tag::HeapObject)
            return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError, "vector-set!: not a vector"}});
        auto* vec = heap.try_get_as<ObjectKind::Vector, types::Vector>(ops::payload(args[0]));
        if (!vec) return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError, "vector-set!: not a vector"}});
        auto idx = classify_numeric(args[1], heap);
        if (!idx.is_valid() || idx.is_flonum() || idx.int_val < 0 || static_cast<size_t>(idx.int_val) >= vec->elements.size())
            return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError, "vector-set!: index out of bounds"}});
        vec->elements[static_cast<size_t>(idx.int_val)] = args[2];
        return nanbox::Nil;
    });

    env.register_builtin("vector?", 1, false, [&heap](Args args) -> std::expected<LispVal, RuntimeError> {
        if (!ops::is_boxed(args[0]) || ops::tag(args[0]) != Tag::HeapObject) return nanbox::False;
        return heap.try_get_as<ObjectKind::Vector, types::Vector>(ops::payload(args[0])) ? nanbox::True : nanbox::False;
    });

    env.register_builtin("make-vector", 2, false, [&heap](Args args) -> std::expected<LispVal, RuntimeError> {
        auto len = classify_numeric(args[0], heap);
        if (!len.is_valid() || len.is_flonum() || len.int_val < 0)
            return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError, "make-vector: length must be a non-negative integer"}});
        std::vector<LispVal> elems(static_cast<size_t>(len.int_val), args[1]);
        return make_vector(heap, std::move(elems));
    });

    // ========================================================================
    // Error signaling: error
    // ========================================================================

    env.register_builtin("error", 1, true, [&heap, &intern_table](Args args) -> std::expected<LispVal, RuntimeError> {
        // (error message irritant ...)
        // First arg should be a string message
        std::string msg;
        auto sv = StringView::try_from(args[0], intern_table);
        if (sv) {
            msg = std::string(sv->view());
        } else {
            msg = format_value(args[0], FormatMode::Write, heap, intern_table);
        }
        // Append irritants
        for (size_t i = 1; i < args.size(); ++i) {
            msg += " ";
            msg += format_value(args[i], FormatMode::Write, heap, intern_table);
        }
        return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::UserError, msg}});
    });

    // ========================================================================
    // Platform detection: platform
    // Returns a symbol identifying the executing host OS at runtime.
    // The #if selects which string is compiled into each platform's binary;
    // the primitive is always invoked at VM execution time, so bytecode built
    // on one platform and run on another correctly reports the executing host.
    // ========================================================================

    env.register_builtin("platform", 0, false, [&intern_table](Args) -> std::expected<LispVal, RuntimeError> {
#if defined(_WIN32)
        return make_symbol(intern_table, "Win32");
#elif defined(__APPLE__)
        return make_symbol(intern_table, "Darwin");
#elif defined(__linux__)
        return make_symbol(intern_table, "Linux");
#else
        return make_symbol(intern_table, "Unknown");
#endif
    });

    // ========================================================================
    // Logic variable type predicate: logic-var?
    // ========================================================================

    env.register_builtin("logic-var?", 1, false, [&heap](Args args) -> std::expected<LispVal, RuntimeError> {
        const LispVal v = args[0];
        if (!ops::is_boxed(v) || ops::tag(v) != Tag::HeapObject) return False;
        auto id = ops::payload(v);
        return heap.try_get_as<ObjectKind::LogicVar, types::LogicVar>(id) ? True : False;
    });

    // ========================================================================
    // Attributed variables (Phase 3)
    // ------------------------------------------------------------------------
    // (put-attr v 'module value)  — install/overwrite attribute (trailed)
    // (get-attr v 'module)        — returns value, or #f if missing
    // (del-attr v 'module)        — remove attribute; #t if removed, #f if absent (trailed)
    // (attr-var? v)               — #t iff v is an unbound LogicVar with
    //                               at least one attribute
    // (register-attr-hook! 'module proc)
    //                             — register a hook called as
    //                               (proc var bound-value attr-value)
    //                               when `var` with attribute 'module is
    //                               bound by unify.  Returns #f on failure
    //                               (which unifies fails).  Hook registry
    //                               is VM-lifetime and NOT trailed.
    // ========================================================================

    // Helper: extract the InternId from a symbol LispVal, or nullopt.
    auto get_symbol_id = [](LispVal v) -> std::optional<memory::intern::InternId> {
        if (!ops::is_boxed(v) || ops::tag(v) != Tag::Symbol) return std::nullopt;
        return static_cast<memory::intern::InternId>(ops::payload(v));
    };

    env.register_builtin("put-attr", 3, false,
        [&heap, get_symbol_id, vm](Args args) -> std::expected<LispVal, RuntimeError> {
            const LispVal v   = args[0];
            const LispVal mod = args[1];
            const LispVal val = args[2];
            if (!ops::is_boxed(v) || ops::tag(v) != Tag::HeapObject)
                return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError,
                    "put-attr: first arg must be a logic variable"}});
            auto* lv = heap.try_get_as<ObjectKind::LogicVar, types::LogicVar>(ops::payload(v));
            if (!lv)
                return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError,
                    "put-attr: first arg must be a logic variable"}});
            auto key = get_symbol_id(mod);
            if (!key)
                return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError,
                    "put-attr: second arg must be a symbol (module name)"}});
            // Trail the prior state so backtracking undoes the write.
            auto it = lv->attrs.find(*key);
            if (vm) {
                vm::TrailEntry e{};
                e.kind       = vm::TrailEntry::Kind::Attr;
                e.var        = v;
                e.module_key = *key;
                if (it != lv->attrs.end()) { e.had_prev = true;  e.prev_value = it->second; }
                else                       { e.had_prev = false; e.prev_value = nanbox::Nil; }
                vm->trail_stack().push_back(e);
            }
            lv->attrs[*key] = val;
            return True;
        });

    env.register_builtin("get-attr", 2, false,
        [&heap, get_symbol_id](Args args) -> std::expected<LispVal, RuntimeError> {
            const LispVal v   = args[0];
            const LispVal mod = args[1];
            if (!ops::is_boxed(v) || ops::tag(v) != Tag::HeapObject) return False;
            auto* lv = heap.try_get_as<ObjectKind::LogicVar, types::LogicVar>(ops::payload(v));
            if (!lv) return False;
            auto key = get_symbol_id(mod);
            if (!key)
                return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError,
                    "get-attr: second arg must be a symbol (module name)"}});
            auto it = lv->attrs.find(*key);
            return (it == lv->attrs.end()) ? False : it->second;
        });

    env.register_builtin("del-attr", 2, false,
        [&heap, get_symbol_id, vm](Args args) -> std::expected<LispVal, RuntimeError> {
            const LispVal v   = args[0];
            const LispVal mod = args[1];
            if (!ops::is_boxed(v) || ops::tag(v) != Tag::HeapObject) return False;
            auto* lv = heap.try_get_as<ObjectKind::LogicVar, types::LogicVar>(ops::payload(v));
            if (!lv) return False;
            auto key = get_symbol_id(mod);
            if (!key)
                return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError,
                    "del-attr: second arg must be a symbol (module name)"}});
            auto it = lv->attrs.find(*key);
            if (it == lv->attrs.end()) return False;
            // Trail so backtracking re-installs the attribute.
            if (vm) {
                vm::TrailEntry e{};
                e.kind       = vm::TrailEntry::Kind::Attr;
                e.var        = v;
                e.module_key = *key;
                e.had_prev   = true;
                e.prev_value = it->second;
                vm->trail_stack().push_back(e);
            }
            lv->attrs.erase(it);
            return True;
        });

    env.register_builtin("attr-var?", 1, false,
        [&heap](Args args) -> std::expected<LispVal, RuntimeError> {
            const LispVal v = args[0];
            if (!ops::is_boxed(v) || ops::tag(v) != Tag::HeapObject) return False;
            auto* lv = heap.try_get_as<ObjectKind::LogicVar, types::LogicVar>(ops::payload(v));
            if (!lv) return False;
            // An attributed variable must be unbound and have at least one attr.
            return (!lv->binding.has_value() && !lv->attrs.empty()) ? True : False;
        });

    env.register_builtin("register-attr-hook!", 2, false,
        [get_symbol_id, vm](Args args) -> std::expected<LispVal, RuntimeError> {
            if (!vm) return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError,
                "register-attr-hook!: requires a running VM"}});
            auto key = get_symbol_id(args[0]);
            if (!key)
                return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError,
                    "register-attr-hook!: first arg must be a symbol (module name)"}});
            vm->attr_unify_hooks()[*key] = args[1];
            return True;
        });

    // ------------------------------------------------------------------
    // logic-var/named : create a fresh unbound LogicVar with a debug name
    // ------------------------------------------------------------------
    // (logic-var/named 'x)           → a fresh unbound logic var labelled "x"
    // (logic-var/named "my-var")     → same, name taken from a string
    //
    // The name has no effect on unification semantics — it is purely for
    // `(var-name v)` introspection, tracing, and future error messages.
    env.register_builtin("logic-var/named", 1, false,
        [&heap, &intern_table](Args args) -> std::expected<LispVal, RuntimeError> {
            std::string name;
            const LispVal v = args[0];
            if (ops::is_boxed(v) && ops::tag(v) == Tag::Symbol) {
                auto s = get_symbol_name(v, intern_table);
                if (s) name = std::string(*s);
            } else if (auto sv = StringView::try_from(v, intern_table)) {
                name = std::string(sv->view());
            } else {
                return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError,
                    "logic-var/named: name must be a symbol or string"}});
            }
            return memory::factory::make_logic_var(heap, std::move(name));
        });

    // ------------------------------------------------------------------
    // var-name : return the debug name of a LogicVar, or #f if none / not a var
    // ------------------------------------------------------------------
    env.register_builtin("var-name", 1, false,
        [&heap, &intern_table](Args args) -> std::expected<LispVal, RuntimeError> {
            const LispVal v = args[0];
            if (!ops::is_boxed(v) || ops::tag(v) != Tag::HeapObject) return False;
            auto* lv = heap.try_get_as<ObjectKind::LogicVar, types::LogicVar>(ops::payload(v));
            if (!lv || lv->name.empty()) return False;
            return make_string(heap, intern_table, lv->name);
        });

    // ------------------------------------------------------------------
    // Occurs-check policy (Phase 1 of the logic/CLP roadmap)
    //
    // (set-occurs-check! 'always)  ; run occurs-check, fail on cycle (default)
    // (set-occurs-check! 'never)   ; skip occurs-check (ISO-Prolog default; faster)
    // (set-occurs-check! 'error)   ; run occurs-check, raise error on cycle
    // (occurs-check-mode)          ; → 'always / 'never / 'error
    // ------------------------------------------------------------------
    env.register_builtin("set-occurs-check!", 1, false,
        [&intern_table, vm](Args args) -> std::expected<LispVal, RuntimeError> {
            if (!vm) return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError,
                "set-occurs-check!: requires a running VM"}});
            const LispVal v = args[0];
            if (!ops::is_boxed(v) || ops::tag(v) != Tag::Symbol)
                return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError,
                    "set-occurs-check!: expected a symbol ('always / 'never / 'error)"}});
            auto sname = get_symbol_name(v, intern_table);
            if (!sname) return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError,
                "set-occurs-check!: invalid symbol"}});
            if (*sname == "always")      vm->set_occurs_check_mode(vm::VM::OccursCheckMode::Always);
            else if (*sname == "never")  vm->set_occurs_check_mode(vm::VM::OccursCheckMode::Never);
            else if (*sname == "error")  vm->set_occurs_check_mode(vm::VM::OccursCheckMode::Error);
            else return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::UserError,
                "set-occurs-check!: mode must be 'always, 'never, or 'error"}});
            return True;
        });

    env.register_builtin("occurs-check-mode", 0, false,
        [&intern_table, vm](Args) -> std::expected<LispVal, RuntimeError> {
            if (!vm) return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError,
                "occurs-check-mode: requires a running VM"}});
            switch (vm->occurs_check_mode()) {
                case vm::VM::OccursCheckMode::Always: return make_symbol(intern_table, "always");
                case vm::VM::OccursCheckMode::Never:  return make_symbol(intern_table, "never");
                case vm::VM::OccursCheckMode::Error:  return make_symbol(intern_table, "error");
            }
            return make_symbol(intern_table, "always");
        });

    // ========================================================================
    // Ground check: ground?
    // Returns #t iff the term contains no unbound logic variables.
    // Recurses into Cons pairs and Vectors; treats all other heap objects
    // (strings, closures, ports, …) as ground.
    // ========================================================================

    env.register_builtin("ground?", 1, false, [&heap](Args args) -> std::expected<LispVal, RuntimeError> {
        std::function<bool(LispVal)> is_ground = [&](LispVal v) -> bool {
            LispVal curr = v;
            for (;;) {
                if (!ops::is_boxed(curr) || ops::tag(curr) != Tag::HeapObject)
                    return true;  // inline primitive — always ground
                auto id = ops::payload(curr);
                if (auto* lv = heap.try_get_as<ObjectKind::LogicVar, types::LogicVar>(id)) {
                    if (!lv->binding.has_value()) return false;  // unbound
                    curr = *lv->binding;                          // follow chain
                } else if (auto* cons = heap.try_get_as<ObjectKind::Cons, types::Cons>(id)) {
                    return is_ground(cons->car) && is_ground(cons->cdr);
                } else if (auto* vec = heap.try_get_as<ObjectKind::Vector, types::Vector>(id)) {
                    for (const auto& elem : vec->elements)
                        if (!is_ground(elem)) return false;
                    return true;
                } else if (auto* ct = heap.try_get_as<ObjectKind::CompoundTerm, types::CompoundTerm>(id)) {
                    for (const auto& a : ct->args)
                        if (!is_ground(a)) return false;
                    return true;
                } else {
                    return true;  // string, closure, port, etc.
                }
            }
        };
        return is_ground(args[0]) ? True : False;
    });

    // ========================================================================
    // Compound terms: term / functor / arity / arg / compound?
    //
    // A `CompoundTerm` is a structured logic term with a symbol functor and
    // zero or more argument values, e.g. (term 'f x 1) ≡ f(x, 1) in Prolog.
    // Unifies structurally with other compound terms of the same functor+arity.
    // See docs/logic.md and docs/logic-next-steps.md for the Phase 1 rationale.
    // ========================================================================

    env.register_builtin("compound?", 1, false,
        [&heap](Args args) -> std::expected<LispVal, RuntimeError> {
            const LispVal v = args[0];
            if (!ops::is_boxed(v) || ops::tag(v) != Tag::HeapObject) return False;
            return heap.try_get_as<ObjectKind::CompoundTerm, types::CompoundTerm>(ops::payload(v))
                ? True : False;
        });

    // (term functor . args) — allocate a CompoundTerm
    env.register_builtin("term", 1, true,
        [&heap](Args args) -> std::expected<LispVal, RuntimeError> {
            const LispVal fn = args[0];
            if (!ops::is_boxed(fn) || ops::tag(fn) != Tag::Symbol)
                return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError,
                    "term: functor must be a symbol"}});
            std::vector<LispVal> targs;
            targs.reserve(args.size() > 0 ? args.size() - 1 : 0);
            for (std::size_t i = 1; i < args.size(); ++i) targs.push_back(args[i]);
            return memory::factory::make_compound(heap, fn, std::move(targs));
        });

    // (functor t) — return the functor symbol of a compound term, or #f otherwise
    env.register_builtin("functor", 1, false,
        [&heap](Args args) -> std::expected<LispVal, RuntimeError> {
            const LispVal v = args[0];
            if (!ops::is_boxed(v) || ops::tag(v) != Tag::HeapObject) return False;
            auto* ct = heap.try_get_as<ObjectKind::CompoundTerm, types::CompoundTerm>(ops::payload(v));
            if (!ct) return False;
            return ct->functor;
        });

    // (arity t) — return the number of arguments of a compound term as a fixnum
    env.register_builtin("arity", 1, false,
        [&heap](Args args) -> std::expected<LispVal, RuntimeError> {
            const LispVal v = args[0];
            if (!ops::is_boxed(v) || ops::tag(v) != Tag::HeapObject)
                return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError,
                    "arity: argument must be a compound term"}});
            auto* ct = heap.try_get_as<ObjectKind::CompoundTerm, types::CompoundTerm>(ops::payload(v));
            if (!ct) return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError,
                "arity: argument must be a compound term"}});
            auto enc = ops::encode<int64_t>(static_cast<int64_t>(ct->args.size()));
            if (!enc) return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError,
                "arity: arity does not fit in a fixnum"}});
            return *enc;
        });

    // (arg i t) — 1-based argument access (Prolog convention).  Out of range → error.
    env.register_builtin("arg", 2, false,
        [&heap](Args args) -> std::expected<LispVal, RuntimeError> {
            auto idx_opt = ops::decode<int64_t>(args[0]);
            if (!idx_opt || !ops::is_boxed(args[0]) || ops::tag(args[0]) != Tag::Fixnum)
                return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError,
                    "arg: first argument must be a fixnum index"}});
            const LispVal v = args[1];
            if (!ops::is_boxed(v) || ops::tag(v) != Tag::HeapObject)
                return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError,
                    "arg: second argument must be a compound term"}});
            auto* ct = heap.try_get_as<ObjectKind::CompoundTerm, types::CompoundTerm>(ops::payload(v));
            if (!ct) return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError,
                "arg: second argument must be a compound term"}});
            int64_t i = *idx_opt;
            if (i < 1 || static_cast<std::size_t>(i) > ct->args.size())
                return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::UserError,
                    "arg: index out of range"}});
            return ct->args[static_cast<std::size_t>(i - 1)];
        });

    // ========================================================================
    // AD Dual stubs (removed — kept as no-ops for slot stability)
    //
    // These builtins were removed along with the Dual heap type.
    // They are retained as error stubs so that the global builtin slot indices
    // remain stable (existing compiled bytecode references slots by index).
    // ========================================================================

    env.register_builtin("dual?", 1, false, [](Args) -> std::expected<LispVal, RuntimeError> {
        return False;  // Nothing is a Dual any more
    });

    env.register_builtin("dual-primal", 1, false, [](Args args) -> std::expected<LispVal, RuntimeError> {
        return args[0];  // pass-through
    });

    env.register_builtin("dual-backprop", 1, false, [&heap](Args) -> std::expected<LispVal, RuntimeError> {
        // Return a no-op backpropagator
        return make_primitive(heap,
            [](const std::vector<LispVal>&) -> std::expected<LispVal, RuntimeError> { return Nil; },
            1, false);
    });

    env.register_builtin("make-dual", 2, false, [](Args) -> std::expected<LispVal, RuntimeError> {
        return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError,
            "make-dual: Dual AD has been removed — use tape-based AD instead"}});
    });

    // ========================================================================
    // CLP domain primitives: %clp-domain-z!  %clp-domain-fd!  %clp-get-domain
    //
    // These are internal builtins consumed by std.clp.  They are prefixed with
    // % to signal that user code should call the std.clp wrapper instead.
    //
    // Domain check at unification time is handled inside VM::unify() using
    // the constraint_store_ field; these builtins only manage the store.
    // ========================================================================

    // (%clp-domain-z! var lo hi)
    // Constrain `var` (unbound logic variable) to the integer interval [lo, hi].
    // Adds the domain to the constraint store (trailed for backtracking).
    env.register_builtin("%clp-domain-z!", 3, false,
        [&heap, &intern_table, vm](Args args) -> std::expected<LispVal, RuntimeError> {
            if (!vm) return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError,
                "%clp-domain-z!: requires a running VM"}});
            // Resolve the variable through any binding chain
            LispVal var = args[0];
            for (;;) {
                if (!ops::is_boxed(var) || ops::tag(var) != Tag::HeapObject) break;
                auto id2 = ops::payload(var);
                auto* lv2 = heap.try_get_as<ObjectKind::LogicVar, types::LogicVar>(id2);
                if (!lv2 || !lv2->binding.has_value()) break;
                var = *lv2->binding;
            }
            if (!ops::is_boxed(var) || ops::tag(var) != Tag::HeapObject)
                return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError,
                    "%clp-domain-z!: first argument must be a logic variable"}});
            auto id = ops::payload(var);
            if (!heap.try_get_as<ObjectKind::LogicVar, types::LogicVar>(id))
                return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError,
                    "%clp-domain-z!: first argument must be an unbound logic variable"}});
            auto nlo = classify_numeric(args[1], heap);
            auto nhi = classify_numeric(args[2], heap);
            if (!nlo.is_valid() || nlo.is_flonum() || !nhi.is_valid() || nhi.is_flonum())
                return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError,
                    "%clp-domain-z!: lo and hi must be integers"}});
            clp::ZDomain dom{ nlo.int_val, nhi.int_val };
            if (dom.empty())
                return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::UserError,
                    "%clp-domain-z!: empty domain (lo > hi)"}});
            vm->trail_set_domain(id, std::move(dom));
            return True;
        });

    // (%clp-domain-fd! var values-list)
    // Constrain `var` to the finite set of integers given as an Eta proper list.
    env.register_builtin("%clp-domain-fd!", 2, false,
        [&heap, vm](Args args) -> std::expected<LispVal, RuntimeError> {
            if (!vm) return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError,
                "%clp-domain-fd!: requires a running VM"}});
            LispVal var = args[0];
            for (;;) {
                if (!ops::is_boxed(var) || ops::tag(var) != Tag::HeapObject) break;
                auto id2 = ops::payload(var);
                auto* lv2 = heap.try_get_as<ObjectKind::LogicVar, types::LogicVar>(id2);
                if (!lv2 || !lv2->binding.has_value()) break;
                var = *lv2->binding;
            }
            if (!ops::is_boxed(var) || ops::tag(var) != Tag::HeapObject)
                return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError,
                    "%clp-domain-fd!: first argument must be a logic variable"}});
            auto id = ops::payload(var);
            if (!heap.try_get_as<ObjectKind::LogicVar, types::LogicVar>(id))
                return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError,
                    "%clp-domain-fd!: first argument must be an unbound logic variable"}});
            clp::FDDomain dom;
            LispVal lst = args[1];
            while (ops::is_boxed(lst) && ops::tag(lst) == Tag::HeapObject) {
                auto* c = heap.try_get_as<ObjectKind::Cons, types::Cons>(ops::payload(lst));
                if (!c) break;
                auto n = classify_numeric(c->car, heap);
                if (!n.is_valid() || n.is_flonum())
                    return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError,
                        "%clp-domain-fd!: domain values must be integers"}});
                dom.values.push_back(n.int_val);
                lst = c->cdr;
            }
            std::sort(dom.values.begin(), dom.values.end());
            dom.values.erase(std::unique(dom.values.begin(), dom.values.end()), dom.values.end());
            if (dom.empty())
                return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::UserError,
                    "%clp-domain-fd!: domain list is empty"}});
            vm->trail_set_domain(id, std::move(dom));
            return True;
        });

    // (%clp-get-domain var)
    // Returns the domain of `var` as an Eta value:
    //   #f                    — no domain / var is already ground
    //   (z lo hi)             — clp(Z) interval [lo, hi]
    //   (fd v1 v2 ...)        — clp(FD) explicit value list
    env.register_builtin("%clp-get-domain", 1, false,
        [&heap, &intern_table, vm](Args args) -> std::expected<LispVal, RuntimeError> {
            if (!vm) return False;
            // Deref the variable
            LispVal var = args[0];
            for (;;) {
                if (!ops::is_boxed(var) || ops::tag(var) != Tag::HeapObject) return False;
                auto id2 = ops::payload(var);
                auto* lv2 = heap.try_get_as<ObjectKind::LogicVar, types::LogicVar>(id2);
                if (!lv2) return False;           // not a logic variable
                if (!lv2->binding.has_value()) break; // found unbound variable
                var = *lv2->binding;
            }
            auto id = ops::payload(var);
            const clp::Domain* dom = vm->constraint_store().get_domain(id);
            if (!dom) return False;

            using namespace memory::factory;
            if (const auto* z = std::get_if<clp::ZDomain>(dom)) {
                // Build (z lo hi)
                auto sym = make_symbol(intern_table, "z");
                auto lo  = make_fixnum(heap, z->lo);
                auto hi  = make_fixnum(heap, z->hi);
                if (!sym || !lo || !hi) return False;
                auto hi_c  = make_cons(heap, *hi,  Nil);
                auto lo_c  = make_cons(heap, *lo,  hi_c ? *hi_c  : Nil);
                auto result= make_cons(heap, *sym, lo_c ? *lo_c  : Nil);
                if (!hi_c || !lo_c || !result) return False;
                return *result;
            } else {
                // FD: build (fd v1 v2 ...)
                const auto& fd = std::get<clp::FDDomain>(*dom);
                auto sym = make_symbol(intern_table, "fd");
                if (!sym) return False;
                LispVal lst = Nil;
                for (int i = static_cast<int>(fd.values.size()) - 1; i >= 0; --i) {
                    auto v = make_fixnum(heap, fd.values[static_cast<std::size_t>(i)]);
                    if (!v) return False;
                    auto c = make_cons(heap, *v, lst);
                    if (!c) return False;
                    lst = *c;
                }
                auto result = make_cons(heap, *sym, lst);
                if (!result) return False;
                return *result;
            }
        });

    // ========================================================================
    // CLP(FD) native bounds-consistency propagators (Phase 4b)
    //
    //   %clp-fd-plus!        (x y z)    — posts z = x + y, narrows bounds
    //   %clp-fd-plus-offset! (y x k)    — posts y = x + k (k a fixnum)
    //   %clp-fd-abs!         (y x)      — posts y = |x|
    //
    // Each returns #t on success (including "nothing to do"), #f on detected
    // inconsistency (empty domain).  Narrowing goes through the trailed
    // ConstraintStore::set_domain so backtracking correctly restores state.
    //
    // These are the bounds kernel only; re-firing on variable binding is
    // installed in Eta-level `std.clp` via a `clp.prop` attribute hook.
    // ========================================================================
    {
        // Helper: deref a LispVal through any binding chain.
        auto deref = [&heap](LispVal v) -> LispVal {
            for (;;) {
                if (!ops::is_boxed(v) || ops::tag(v) != Tag::HeapObject) return v;
                auto id = ops::payload(v);
                auto* lv = heap.try_get_as<ObjectKind::LogicVar, types::LogicVar>(id);
                if (!lv || !lv->binding.has_value()) return v;
                v = *lv->binding;
            }
        };

        // Bounds snapshot of a CLP(FD) argument: ground integer, unbound var
        // with Z or FD domain, or unbound var with no domain (unbounded).
        struct Bounds {
            int64_t  lo     = 0;
            int64_t  hi     = 0;
            bool     finite = false;        // has finite [lo,hi]
            bool     is_var = false;        // unbound logic var
            ObjectId id     = 0;            // heap id when is_var
            bool     is_fd  = false;        // FD domain (else Z or none)
        };

        // Extract a Bounds for arg. Returns std::nullopt on type error
        // (e.g. a non-numeric ground value).  Empty FD/Z domain → finite=true
        // with lo > hi so caller detects infeasibility uniformly.
        auto extract_bounds = [&heap, vm, deref](LispVal v) -> std::optional<Bounds> {
            LispVal d = deref(v);
            if (ops::is_boxed(d) && ops::tag(d) == Tag::HeapObject) {
                auto id = ops::payload(d);
                auto* lv = heap.try_get_as<ObjectKind::LogicVar, types::LogicVar>(id);
                if (lv && !lv->binding.has_value()) {
                    Bounds b;
                    b.is_var = true;
                    b.id     = id;
                    if (vm) {
                        const auto* dom = vm->constraint_store().get_domain(id);
                        if (dom) {
                            if (auto* z = std::get_if<clp::ZDomain>(dom)) {
                                b.lo = z->lo; b.hi = z->hi; b.finite = true;
                            } else if (auto* fd = std::get_if<clp::FDDomain>(dom)) {
                                b.is_fd = true;
                                if (fd->values.empty()) {
                                    b.lo = 1; b.hi = 0; b.finite = true;  // empty sentinel
                                } else {
                                    b.lo = fd->values.front();
                                    b.hi = fd->values.back();
                                    b.finite = true;
                                }
                            }
                        }
                    }
                    return b;
                }
            }
            // Ground: must be an integer
            auto n = classify_numeric(d, heap);
            if (!n.is_valid() || n.is_flonum()) return std::nullopt;
            Bounds b;
            b.lo = n.int_val; b.hi = n.int_val; b.finite = true;
            return b;
        };

        // Narrow a var's domain to [new_lo, new_hi]. Returns false on empty.
        // Only writes through trail_set_domain (trailed) when something actually
        // changes.  For FD domains, values outside [new_lo, new_hi] are filtered out.
        auto narrow_var = [vm](ObjectId id, int64_t new_lo, int64_t new_hi) -> bool {
            if (new_lo > new_hi) return false;
            if (!vm) return true;
            auto& store = vm->constraint_store();
            const auto* dom = store.get_domain(id);
            if (!dom) {
                vm->trail_set_domain(id, clp::ZDomain{ new_lo, new_hi });
                return true;
            }
            if (auto* z = std::get_if<clp::ZDomain>(dom)) {
                int64_t lo = std::max(z->lo, new_lo);
                int64_t hi = std::min(z->hi, new_hi);
                if (lo > hi) return false;
                if (lo == z->lo && hi == z->hi) return true;  // no change
                vm->trail_set_domain(id, clp::ZDomain{ lo, hi });
                return true;
            }
            // FD
            const auto& fd = std::get<clp::FDDomain>(*dom);
            clp::FDDomain nfd;
            nfd.values.reserve(fd.values.size());
            for (auto v : fd.values) {
                if (v >= new_lo && v <= new_hi) nfd.values.push_back(v);
            }
            if (nfd.values.empty()) return false;
            if (nfd.values.size() == fd.values.size()) return true;  // no change
            vm->trail_set_domain(id, std::move(nfd));
            return true;
        };

        // ── %clp-fd-plus! (x y z)  :  z = x + y ────────────────────────────
        // Bounds consistency (interval form):
        //   z.lo >= x.lo + y.lo   z.hi <= x.hi + y.hi
        //   x.lo >= z.lo - y.hi   x.hi <= z.hi - y.lo
        //   y.lo >= z.lo - x.hi   y.hi <= z.hi - x.lo
        env.register_builtin("%clp-fd-plus!", 3, false,
            [extract_bounds, narrow_var](Args args) -> std::expected<LispVal, RuntimeError> {
                auto bx = extract_bounds(args[0]);
                auto by = extract_bounds(args[1]);
                auto bz = extract_bounds(args[2]);
                if (!bx || !by || !bz)
                    return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError,
                        "%clp-fd-plus!: arguments must be integers or logic variables"}});
                // Any side without finite bounds contributes nothing — just
                // succeed without narrowing (MVP: propagate only when all three
                // are finite).
                if (!bx->finite || !by->finite || !bz->finite) return True;
                // Empty-domain short-circuit.
                if (bx->lo > bx->hi || by->lo > by->hi || bz->lo > bz->hi) return False;
                int64_t nz_lo = bx->lo + by->lo,  nz_hi = bx->hi + by->hi;
                int64_t nx_lo = bz->lo - by->hi,  nx_hi = bz->hi - by->lo;
                int64_t ny_lo = bz->lo - bx->hi,  ny_hi = bz->hi - bx->lo;
                // Narrow each var (ignores ground operands).
                if (bz->is_var && !narrow_var(bz->id, nz_lo, nz_hi)) return False;
                if (bx->is_var && !narrow_var(bx->id, nx_lo, nx_hi)) return False;
                if (by->is_var && !narrow_var(by->id, ny_lo, ny_hi)) return False;
                // For ground operands, verify consistency (e.g. z=5 must satisfy
                // 5 ∈ [x.lo+y.lo, x.hi+y.hi]).
                if (!bz->is_var && (bz->lo < nz_lo || bz->hi > nz_hi)) return False;
                if (!bx->is_var && (bx->lo < nx_lo || bx->hi > nx_hi)) return False;
                if (!by->is_var && (by->lo < ny_lo || by->hi > ny_hi)) return False;
                return True;
            });

        // ── %clp-fd-plus-offset! (y x k)  :  y = x + k, k ∈ ℤ ──────────────
        env.register_builtin("%clp-fd-plus-offset!", 3, false,
            [&heap, extract_bounds, narrow_var](Args args) -> std::expected<LispVal, RuntimeError> {
                auto by = extract_bounds(args[0]);
                auto bx = extract_bounds(args[1]);
                auto nk = classify_numeric(args[2], heap);
                if (!by || !bx)
                    return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError,
                        "%clp-fd-plus-offset!: first two args must be integers or logic variables"}});
                if (!nk.is_valid() || nk.is_flonum())
                    return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError,
                        "%clp-fd-plus-offset!: offset must be an integer"}});
                const int64_t k = nk.int_val;
                if (!bx->finite || !by->finite) return True;
                if (bx->lo > bx->hi || by->lo > by->hi) return False;
                int64_t ny_lo = bx->lo + k, ny_hi = bx->hi + k;
                int64_t nx_lo = by->lo - k, nx_hi = by->hi - k;
                if (by->is_var && !narrow_var(by->id, ny_lo, ny_hi)) return False;
                if (bx->is_var && !narrow_var(bx->id, nx_lo, nx_hi)) return False;
                if (!by->is_var && (by->lo < ny_lo || by->hi > ny_hi)) return False;
                if (!bx->is_var && (bx->lo < nx_lo || bx->hi > nx_hi)) return False;
                return True;
            });

        // ── %clp-fd-abs! (y x)  :  y = |x| ────────────────────────────────
        // Bounds:
        //   y ∈ [0, max(|x.lo|, |x.hi|)]
        //   if x.lo >= 0: y ∈ [x.lo, x.hi]; x ∈ [y.lo, y.hi]
        //   if x.hi <= 0: y ∈ [-x.hi, -x.lo]; x ∈ [-y.hi, -y.lo]
        //   if x straddles 0: y.lo stays 0; x ∈ [-y.hi, y.hi] (weak backward prop)
        env.register_builtin("%clp-fd-abs!", 2, false,
            [extract_bounds, narrow_var](Args args) -> std::expected<LispVal, RuntimeError> {
                auto by = extract_bounds(args[0]);
                auto bx = extract_bounds(args[1]);
                if (!by || !bx)
                    return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError,
                        "%clp-fd-abs!: arguments must be integers or logic variables"}});
                if (!bx->finite || !by->finite) return True;
                if (bx->lo > bx->hi || by->lo > by->hi) return False;
                int64_t ny_lo, ny_hi;
                if (bx->lo >= 0) { ny_lo = bx->lo;  ny_hi = bx->hi; }
                else if (bx->hi <= 0) { ny_lo = -bx->hi; ny_hi = -bx->lo; }
                else { ny_lo = 0; ny_hi = std::max(-bx->lo, bx->hi); }
                // Forward: narrow y.
                if (by->is_var && !narrow_var(by->id, ny_lo, ny_hi)) return False;
                if (!by->is_var && (by->lo < ny_lo || by->hi > ny_hi)) return False;
                // Backward: narrow x using (possibly updated) y bounds.
                int64_t yl = std::max(by->lo, ny_lo);
                int64_t yh = std::min(by->hi, ny_hi);
                if (yl < 0) yl = 0;
                if (yl > yh) return False;
                int64_t nx_lo, nx_hi;
                if (bx->lo >= 0)      { nx_lo = yl;   nx_hi = yh; }
                else if (bx->hi <= 0) { nx_lo = -yh;  nx_hi = -yl; }
                else                  { nx_lo = -yh;  nx_hi = yh; }
                if (bx->is_var && !narrow_var(bx->id, nx_lo, nx_hi)) return False;
                if (!bx->is_var && (bx->lo < nx_lo || bx->hi > nx_hi)) return False;
                return True;
            });

        // ── %clp-fd-times! (z x y)  :  z = x * y ──────────────────────────
        // Bounds consistency via interval multiplication.  The product of
        // two intervals is [min of corners, max of corners].  Division for
        // back-propagation is implemented with explicit floor/ceil to keep
        // results integral; divisors straddling zero leave the quotient
        // variable unconstrained (weak propagation, consistent with SWI).
        auto interval_mul = [](int64_t a, int64_t b, int64_t c, int64_t d,
                               int64_t& out_lo, int64_t& out_hi) {
            int64_t p1 = a * c, p2 = a * d, p3 = b * c, p4 = b * d;
            out_lo = std::min(std::min(p1, p2), std::min(p3, p4));
            out_hi = std::max(std::max(p1, p2), std::max(p3, p4));
        };
        auto idiv_floor = [](int64_t a, int64_t b) -> int64_t {
            int64_t q = a / b, r = a % b;
            if ((r != 0) && ((r < 0) != (b < 0))) --q;
            return q;
        };
        auto idiv_ceil = [](int64_t a, int64_t b) -> int64_t {
            int64_t q = a / b, r = a % b;
            if ((r != 0) && ((r < 0) == (b < 0))) ++q;
            return q;
        };
        env.register_builtin("%clp-fd-times!", 3, false,
            [extract_bounds, narrow_var, interval_mul, idiv_floor, idiv_ceil]
            (Args args) -> std::expected<LispVal, RuntimeError> {
                auto bz = extract_bounds(args[0]);
                auto bx = extract_bounds(args[1]);
                auto by = extract_bounds(args[2]);
                if (!bz || !bx || !by)
                    return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError,
                        "%clp-fd-times!: arguments must be integers or logic variables"}});
                if (!bx->finite || !by->finite || !bz->finite) return True;
                if (bx->lo > bx->hi || by->lo > by->hi || bz->lo > bz->hi) return False;
                // Forward: z = x * y
                int64_t nz_lo, nz_hi;
                interval_mul(bx->lo, bx->hi, by->lo, by->hi, nz_lo, nz_hi);
                if (bz->is_var && !narrow_var(bz->id, nz_lo, nz_hi)) return False;
                if (!bz->is_var && (bz->lo < nz_lo || bz->hi > nz_hi)) return False;
                // Backward x = z / y (only when y does not straddle 0)
                auto narrow_quot = [&](const Bounds& src, const Bounds& div) -> std::optional<std::pair<int64_t,int64_t>> {
                    if (div.lo <= 0 && div.hi >= 0) return std::nullopt;  // straddles zero → skip
                    int64_t q1 = idiv_floor(src.lo, div.lo);
                    int64_t q2 = idiv_floor(src.lo, div.hi);
                    int64_t q3 = idiv_floor(src.hi, div.lo);
                    int64_t q4 = idiv_floor(src.hi, div.hi);
                    int64_t q5 = idiv_ceil(src.lo, div.lo);
                    int64_t q6 = idiv_ceil(src.lo, div.hi);
                    int64_t q7 = idiv_ceil(src.hi, div.lo);
                    int64_t q8 = idiv_ceil(src.hi, div.hi);
                    int64_t lo = std::min({q5,q6,q7,q8});
                    int64_t hi = std::max({q1,q2,q3,q4});
                    return std::make_pair(lo, hi);
                };
                if (bx->is_var) {
                    if (auto q = narrow_quot(*bz, *by))
                        if (!narrow_var(bx->id, q->first, q->second)) return False;
                }
                if (by->is_var) {
                    if (auto q = narrow_quot(*bz, *bx))
                        if (!narrow_var(by->id, q->first, q->second)) return False;
                }
                return True;
            });

        // ── %clp-fd-sum! (xs s)  :  Σ xs = s ──────────────────────────────
        // `xs` is an Eta list of logic vars and/or ground integers.  Bounds:
        //   s ∈ [Σ xᵢ.lo, Σ xᵢ.hi]
        //   xⱼ ∈ [s.lo − Σᵢ≠ⱼ xᵢ.hi,  s.hi − Σᵢ≠ⱼ xᵢ.lo]
        auto walk_list = [&heap](LispVal lst, std::vector<LispVal>& out) -> bool {
            while (ops::is_boxed(lst) && ops::tag(lst) == Tag::HeapObject) {
                auto* c = heap.try_get_as<ObjectKind::Cons, types::Cons>(ops::payload(lst));
                if (!c) return false;
                out.push_back(c->car);
                lst = c->cdr;
            }
            return lst == Nil;
        };
        env.register_builtin("%clp-fd-sum!", 2, false,
            [extract_bounds, narrow_var, walk_list](Args args) -> std::expected<LispVal, RuntimeError> {
                std::vector<LispVal> elems;
                if (!walk_list(args[0], elems))
                    return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError,
                        "%clp-fd-sum!: first arg must be a proper list"}});
                std::vector<Bounds> bs;
                bs.reserve(elems.size());
                for (auto v : elems) {
                    auto b = extract_bounds(v);
                    if (!b)
                        return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError,
                            "%clp-fd-sum!: each list element must be an integer or logic variable"}});
                    if (!b->finite) return True;   // any unbounded ⇒ skip
                    if (b->lo > b->hi) return False;
                    bs.push_back(*b);
                }
                auto bs_b = extract_bounds(args[1]);
                if (!bs_b)
                    return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError,
                        "%clp-fd-sum!: sum must be an integer or logic variable"}});
                if (!bs_b->finite) return True;
                if (bs_b->lo > bs_b->hi) return False;
                int64_t tot_lo = 0, tot_hi = 0;
                for (auto& b : bs) { tot_lo += b.lo; tot_hi += b.hi; }
                // Forward: narrow s
                if (bs_b->is_var && !narrow_var(bs_b->id, tot_lo, tot_hi)) return False;
                if (!bs_b->is_var && (bs_b->lo < tot_lo || bs_b->hi > tot_hi)) return False;
                // Backward: for each var xⱼ narrow to [s.lo − (tot_hi − xⱼ.hi), s.hi − (tot_lo − xⱼ.lo)]
                int64_t s_lo = std::max(bs_b->lo, tot_lo);
                int64_t s_hi = std::min(bs_b->hi, tot_hi);
                for (std::size_t j = 0; j < bs.size(); ++j) {
                    if (!bs[j].is_var) continue;
                    int64_t other_hi = tot_hi - bs[j].hi;
                    int64_t other_lo = tot_lo - bs[j].lo;
                    int64_t nj_lo = s_lo - other_hi;
                    int64_t nj_hi = s_hi - other_lo;
                    if (!narrow_var(bs[j].id, nj_lo, nj_hi)) return False;
                }
                return True;
            });

        // ── %clp-fd-scalar-product! (cs xs s)  :  Σ cᵢ·xᵢ = s ────────────
        // `cs` is a list of ground integer coefficients of the same length
        // as `xs`.  Bounds projection treats each cᵢ·xᵢ interval and uses
        // the same subtractive back-propagation as fd_sum.
        env.register_builtin("%clp-fd-scalar-product!", 3, false,
            [&heap, extract_bounds, narrow_var, walk_list](Args args) -> std::expected<LispVal, RuntimeError> {
                std::vector<LispVal> c_vals, x_vals;
                if (!walk_list(args[0], c_vals) || !walk_list(args[1], x_vals))
                    return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError,
                        "%clp-fd-scalar-product!: coeffs and vars must be proper lists"}});
                if (c_vals.size() != x_vals.size())
                    return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::UserError,
                        "%clp-fd-scalar-product!: coeffs and vars length mismatch"}});
                std::vector<int64_t> cs;
                cs.reserve(c_vals.size());
                for (auto v : c_vals) {
                    auto n = classify_numeric(v, heap);
                    if (!n.is_valid() || n.is_flonum())
                        return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError,
                            "%clp-fd-scalar-product!: coefficients must be integers"}});
                    cs.push_back(n.int_val);
                }
                std::vector<Bounds> bs;
                bs.reserve(x_vals.size());
                for (auto v : x_vals) {
                    auto b = extract_bounds(v);
                    if (!b)
                        return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError,
                            "%clp-fd-scalar-product!: vars must be integers or logic variables"}});
                    if (!b->finite) return True;
                    if (b->lo > b->hi) return False;
                    bs.push_back(*b);
                }
                auto bs_s = extract_bounds(args[2]);
                if (!bs_s)
                    return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError,
                        "%clp-fd-scalar-product!: sum must be an integer or logic variable"}});
                if (!bs_s->finite) return True;
                if (bs_s->lo > bs_s->hi) return False;
                // Per-term interval [cᵢ·xᵢ.lo, cᵢ·xᵢ.hi] — swap ends when cᵢ < 0.
                auto term_bounds = [&](std::size_t i, int64_t& t_lo, int64_t& t_hi) {
                    int64_t a = cs[i] * bs[i].lo, b = cs[i] * bs[i].hi;
                    t_lo = std::min(a, b); t_hi = std::max(a, b);
                };
                int64_t tot_lo = 0, tot_hi = 0;
                std::vector<std::pair<int64_t,int64_t>> term;
                term.reserve(bs.size());
                for (std::size_t i = 0; i < bs.size(); ++i) {
                    int64_t tl, th; term_bounds(i, tl, th);
                    term.emplace_back(tl, th);
                    tot_lo += tl; tot_hi += th;
                }
                // Forward narrow s.
                if (bs_s->is_var && !narrow_var(bs_s->id, tot_lo, tot_hi)) return False;
                if (!bs_s->is_var && (bs_s->lo < tot_lo || bs_s->hi > tot_hi)) return False;
                // Backward narrow each xⱼ
                int64_t s_lo = std::max(bs_s->lo, tot_lo);
                int64_t s_hi = std::min(bs_s->hi, tot_hi);
                for (std::size_t j = 0; j < bs.size(); ++j) {
                    if (!bs[j].is_var || cs[j] == 0) continue;
                    int64_t other_lo = tot_lo - term[j].first;
                    int64_t other_hi = tot_hi - term[j].second;
                    // cⱼ·xⱼ ∈ [s_lo - other_hi, s_hi - other_lo]
                    int64_t t_lo = s_lo - other_hi;
                    int64_t t_hi = s_hi - other_lo;
                    // xⱼ ∈ [⌈t_lo/cⱼ⌉, ⌊t_hi/cⱼ⌋] when cⱼ>0, else swapped and sign-flipped.
                    auto idiv_floor = [](int64_t a, int64_t b)->int64_t{
                        int64_t q=a/b,r=a%b;
                        if((r!=0)&&((r<0)!=(b<0))) --q;
                        return q;
                    };
                    auto idiv_ceil = [](int64_t a, int64_t b)->int64_t{
                        int64_t q=a/b,r=a%b;
                        if((r!=0)&&((r<0)==(b<0))) ++q;
                        return q;
                    };
                    int64_t xj_lo, xj_hi;
                    if (cs[j] > 0) {
                        xj_lo = idiv_ceil(t_lo, cs[j]);
                        xj_hi = idiv_floor(t_hi, cs[j]);
                    } else {
                        xj_lo = idiv_ceil(t_hi, cs[j]);
                        xj_hi = idiv_floor(t_lo, cs[j]);
                    }
                    if (!narrow_var(bs[j].id, xj_lo, xj_hi)) return False;
                }
                return True;
            });

        // ── %clp-fd-element! (i xs v)  :  nth(xs, i) = v, i ∈ [1..|xs|] ──
        // One-based index to match Prolog `element/3` convention.  When `i`
        // is ground, degenerates to a single equality via narrowing.  When
        // `i` is a logic var with an FD domain, we intersect `v`'s bounds
        // with the union of the candidate xs[k]'s bounds, and prune `i`'s
        // domain to values `k` whose xs[k] is consistent with `v`.
        env.register_builtin("%clp-fd-element!", 3, false,
            [&heap, vm, extract_bounds, narrow_var, walk_list]
            (Args args) -> std::expected<LispVal, RuntimeError> {
                std::vector<LispVal> elems;
                if (!walk_list(args[1], elems))
                    return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError,
                        "%clp-fd-element!: second arg must be a proper list"}});
                if (elems.empty())
                    return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::UserError,
                        "%clp-fd-element!: list must be non-empty"}});
                auto bi = extract_bounds(args[0]);
                auto bv = extract_bounds(args[2]);
                if (!bi || !bv)
                    return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError,
                        "%clp-fd-element!: index and value must be integers or logic variables"}});
                const int64_t n = static_cast<int64_t>(elems.size());
                // Force i ∈ [1..n]
                if (bi->is_var && !narrow_var(bi->id, 1, n)) return False;
                if (!bi->is_var && (bi->lo < 1 || bi->hi > n)) return False;
                int64_t i_lo = bi->is_var ? std::max<int64_t>(1, bi->lo) : bi->lo;
                int64_t i_hi = bi->is_var ? std::min<int64_t>(n, bi->hi) : bi->hi;
                // For each candidate k ∈ [i_lo..i_hi], extract bounds of xs[k-1];
                // its bounds union is the possible value for v.  Also collect
                // the set of k's that are consistent with v's current bounds.
                int64_t v_union_lo = INT64_MAX, v_union_hi = INT64_MIN;
                std::vector<int64_t> compatible_ks;
                bool any_infinite_candidate = false;
                for (int64_t k = i_lo; k <= i_hi; ++k) {
                    // If i is FD-domained, skip values not in its FD set.
                    if (bi->is_var && vm) {
                        const auto* dom = vm->constraint_store().get_domain(bi->id);
                        if (dom) if (auto* fd = std::get_if<clp::FDDomain>(dom))
                            if (!fd->contains(k)) continue;
                    }
                    auto bk = extract_bounds(elems[static_cast<std::size_t>(k - 1)]);
                    if (!bk)
                        return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError,
                            "%clp-fd-element!: list element must be integer or logic variable"}});
                    if (!bk->finite) { any_infinite_candidate = true; compatible_ks.push_back(k); continue; }
                    if (bk->lo > bk->hi) continue;  // this candidate is infeasible
                    // Is this candidate consistent with v?
                    int64_t isect_lo = std::max(bv->lo, bk->lo);
                    int64_t isect_hi = std::min(bv->hi, bk->hi);
                    if (bv->finite && isect_lo > isect_hi) continue;
                    compatible_ks.push_back(k);
                    v_union_lo = std::min(v_union_lo, bk->lo);
                    v_union_hi = std::max(v_union_hi, bk->hi);
                }
                if (compatible_ks.empty()) return False;
                // Narrow v to the union of candidate bounds (skip when an
                // unbounded candidate exists — weak propagation).
                if (!any_infinite_candidate) {
                    if (bv->is_var && !narrow_var(bv->id, v_union_lo, v_union_hi)) return False;
                    if (!bv->is_var && (bv->lo < v_union_lo || bv->hi > v_union_hi)) return False;
                }
                // Narrow i to the compatible set.  Use an FD domain if the
                // set is sparse relative to [i_lo..i_hi]; otherwise Z bounds.
                if (bi->is_var && vm) {
                    const int64_t first_k = compatible_ks.front();
                    const int64_t last_k  = compatible_ks.back();
                    bool dense = (static_cast<int64_t>(compatible_ks.size()) == (last_k - first_k + 1));
                    if (dense) {
                        if (!narrow_var(bi->id, first_k, last_k)) return False;
                    } else {
                        clp::FDDomain nd;
                        nd.values.assign(compatible_ks.begin(), compatible_ks.end());
                        std::sort(nd.values.begin(), nd.values.end());
                        nd.values.erase(std::unique(nd.values.begin(), nd.values.end()), nd.values.end());
                        vm->trail_set_domain(bi->id, std::move(nd));
                    }
                }
                return True;
            });

        // ── %clp-fd-all-different! (vars)  :  Régin-matching all-different ─
        // Domain-consistent pruning: removes every value v from D(x) that
        // cannot participate in a valuation satisfying all_different(vars).
        // Strictly stronger than the pairwise attribute-hook version.
        env.register_builtin("%clp-fd-all-different!", 1, false,
            [&heap, vm, deref, walk_list](Args args) -> std::expected<LispVal, RuntimeError> {
                if (!vm) return True;
                std::vector<LispVal> elems;
                if (!walk_list(args[0], elems))
                    return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError,
                        "%clp-fd-all-different!: argument must be a proper list"}});
                if (elems.size() <= 1) return True;

                // Build the algorithm's var table by dereffing each element.
                std::vector<clp::AlldiffVar> avars;
                avars.reserve(elems.size());
                for (auto e : elems) {
                    LispVal d = deref(e);
                    clp::AlldiffVar av;
                    if (ops::is_boxed(d) && ops::tag(d) == Tag::HeapObject) {
                        auto id = ops::payload(d);
                        auto* lv = heap.try_get_as<ObjectKind::LogicVar, types::LogicVar>(id);
                        if (lv && !lv->binding.has_value()) {
                            av.id = id;
                            const auto* dom = vm->constraint_store().get_domain(id);
                            if (!dom) {
                                av.is_free = true;
                            } else if (auto* z = std::get_if<clp::ZDomain>(dom)) {
                                // Materialise Z interval as FD values.
                                // Cap at a reasonable ceiling to avoid runaway
                                // allocation for unbounded-looking domains.
                                constexpr int64_t MAX_SPAN = 1'000'000;
                                if (z->hi - z->lo + 1 > MAX_SPAN) {
                                    av.is_free = true;  // too wide — skip
                                } else {
                                    av.domain.reserve(static_cast<std::size_t>(z->hi - z->lo + 1));
                                    for (int64_t v = z->lo; v <= z->hi; ++v) av.domain.push_back(v);
                                }
                            } else if (auto* fd = std::get_if<clp::FDDomain>(dom)) {
                                av.domain = fd->values;  // already sorted/unique
                                if (av.domain.empty()) return False;
                            }
                            avars.push_back(std::move(av));
                            continue;
                        }
                    }
                    // Ground case — expect integer.
                    auto n = classify_numeric(d, heap);
                    if (!n.is_valid() || n.is_flonum())
                        return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError,
                            "%clp-fd-all-different!: values must be integers or logic variables"}});
                    av.ground_val = n.int_val;
                    av.is_ground = true;
                    avars.push_back(std::move(av));
                }

                // Narrow callback: the algorithm invokes this for each var
                // whose domain shrank.  We install the new domain via the
                // trailed constraint store (preserves ZDomain narrowing if
                // the new domain remains dense — else becomes a FD list).
                auto narrow = [vm](uint64_t id, const std::vector<int64_t>& new_dom) -> bool {
                    if (new_dom.empty()) return false;
                    // If contiguous, collapse to a Z domain; else FD.
                    bool contiguous = true;
                    for (std::size_t i = 1; i < new_dom.size(); ++i) {
                        if (new_dom[i] != new_dom[i - 1] + 1) { contiguous = false; break; }
                    }
                    if (contiguous) {
                        vm->trail_set_domain(id,
                            clp::ZDomain{ new_dom.front(), new_dom.back() });
                    } else {
                        clp::FDDomain fd;
                        fd.values = new_dom;
                        vm->trail_set_domain(id, std::move(fd));
                    }
                    return true;
                };

                return clp::run_regin_alldiff(avars, narrow) ? True : False;
            });
    }

    // ========================================================================
    // AD Tape primitives: tape-new tape-start! tape-stop! tape-var
    //                     tape-backward! tape-adjoint tape-primal
    //                     tape-ref? tape-ref-index tape-size
    //
    // These expose the tape-based (Wengert list) reverse-mode AD to Eta code.
    // Arithmetic +,-,*,/ and transcendentals sin,cos,exp,log,sqrt are
    // automatically recorded when a TapeRef operand is detected.
    // ========================================================================

    env.register_builtin("tape-new", 0, false, [&heap](Args /*args*/) -> std::expected<LispVal, RuntimeError> {
        return make_tape(heap);
    });

    env.register_builtin("tape-start!", 1, false, [&heap, vm](Args args) -> std::expected<LispVal, RuntimeError> {
        if (!vm) return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError, "tape-start!: requires a running VM"}});
        // Verify argument is a tape
        if (!ops::is_boxed(args[0]) || ops::tag(args[0]) != Tag::HeapObject)
            return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError, "tape-start!: argument must be a tape"}});
        if (!heap.try_get_as<ObjectKind::Tape, types::Tape>(ops::payload(args[0])))
            return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError, "tape-start!: argument must be a tape"}});
        vm->push_active_tape(args[0]);
        return True;
    });

    env.register_builtin("tape-stop!", 0, false, [vm](Args /*args*/) -> std::expected<LispVal, RuntimeError> {
        if (!vm) return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError, "tape-stop!: requires a running VM"}});
        vm->pop_active_tape();
        return True;
    });

    env.register_builtin("tape-var", 2, false, [&heap, vm](Args args) -> std::expected<LispVal, RuntimeError> {
        if (!vm) return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError, "tape-var: requires a running VM"}});
        // args[0] = tape, args[1] = numeric value
        if (!ops::is_boxed(args[0]) || ops::tag(args[0]) != Tag::HeapObject)
            return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError, "tape-var: first argument must be a tape"}});
        auto* tape = heap.try_get_as<ObjectKind::Tape, types::Tape>(ops::payload(args[0]));
        if (!tape)
            return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError, "tape-var: first argument must be a tape"}});
        auto n = classify_numeric(args[1], heap);
        if (!n.is_valid())
            return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError, "tape-var: second argument must be a number"}});
        uint32_t idx = tape->push_var(n.as_double());
        return ops::box(Tag::TapeRef, idx);
    });

    env.register_builtin("tape-backward!", 2, false, [&heap](Args args) -> std::expected<LispVal, RuntimeError> {
        // args[0] = tape, args[1] = tape-ref (output node)
        if (!ops::is_boxed(args[0]) || ops::tag(args[0]) != Tag::HeapObject)
            return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError, "tape-backward!: first argument must be a tape"}});
        auto* tape = heap.try_get_as<ObjectKind::Tape, types::Tape>(ops::payload(args[0]));
        if (!tape)
            return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError, "tape-backward!: first argument must be a tape"}});
        if (!ops::is_boxed(args[1]) || ops::tag(args[1]) != Tag::TapeRef)
            return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError, "tape-backward!: second argument must be a tape-ref"}});
        auto output_idx = static_cast<uint32_t>(ops::payload(args[1]));
        tape->backward(output_idx);
        return True;
    });

    env.register_builtin("tape-adjoint", 2, false, [&heap](Args args) -> std::expected<LispVal, RuntimeError> {
        // args[0] = tape, args[1] = tape-ref
        if (!ops::is_boxed(args[0]) || ops::tag(args[0]) != Tag::HeapObject)
            return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError, "tape-adjoint: first argument must be a tape"}});
        auto* tape = heap.try_get_as<ObjectKind::Tape, types::Tape>(ops::payload(args[0]));
        if (!tape)
            return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError, "tape-adjoint: first argument must be a tape"}});
        if (!ops::is_boxed(args[1]) || ops::tag(args[1]) != Tag::TapeRef)
            return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError, "tape-adjoint: second argument must be a tape-ref"}});
        auto idx = static_cast<uint32_t>(ops::payload(args[1]));
        if (idx >= tape->entries.size())
            return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError, "tape-adjoint: index out of range"}});
        return make_flonum(tape->entries[idx].adjoint);
    });

    env.register_builtin("tape-primal", 2, false, [&heap](Args args) -> std::expected<LispVal, RuntimeError> {
        // args[0] = tape, args[1] = tape-ref
        if (!ops::is_boxed(args[0]) || ops::tag(args[0]) != Tag::HeapObject)
            return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError, "tape-primal: first argument must be a tape"}});
        auto* tape = heap.try_get_as<ObjectKind::Tape, types::Tape>(ops::payload(args[0]));
        if (!tape)
            return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError, "tape-primal: first argument must be a tape"}});
        if (!ops::is_boxed(args[1]) || ops::tag(args[1]) != Tag::TapeRef)
            return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError, "tape-primal: second argument must be a tape-ref"}});
        auto idx = static_cast<uint32_t>(ops::payload(args[1]));
        if (idx >= tape->entries.size())
            return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError, "tape-primal: index out of range"}});
        return make_flonum(tape->entries[idx].primal);
    });

    env.register_builtin("tape-ref?", 1, false, [](Args args) -> std::expected<LispVal, RuntimeError> {
        return (ops::is_boxed(args[0]) && ops::tag(args[0]) == Tag::TapeRef) ? True : False;
    });

    env.register_builtin("tape-ref-index", 1, false, [](Args args) -> std::expected<LispVal, RuntimeError> {
        if (!ops::is_boxed(args[0]) || ops::tag(args[0]) != Tag::TapeRef)
            return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError, "tape-ref-index: argument must be a tape-ref"}});
        return ops::encode(static_cast<int64_t>(ops::payload(args[0])));
    });

    env.register_builtin("tape-size", 1, false, [&heap](Args args) -> std::expected<LispVal, RuntimeError> {
        if (!ops::is_boxed(args[0]) || ops::tag(args[0]) != Tag::HeapObject)
            return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError, "tape-size: argument must be a tape"}});
        auto* tape = heap.try_get_as<ObjectKind::Tape, types::Tape>(ops::payload(args[0]));
        if (!tape)
            return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError, "tape-size: argument must be a tape"}});
        return ops::encode(static_cast<int64_t>(tape->entries.size()));
    });

    // tape-ref-value: extract the forward (primal) value of a TapeRef from the
    // active tape.  Returns the value as-is if it is not a TapeRef.  This is
    // needed for non-differentiable comparisons (e.g. branch conditions).
    env.register_builtin("tape-ref-value", 1, false, [&heap, vm](Args args) -> std::expected<LispVal, RuntimeError> {
        if (!ops::is_boxed(args[0]) || ops::tag(args[0]) != Tag::TapeRef)
            return args[0];  // pass-through for non-TapeRef
        if (!vm) return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError, "tape-ref-value: requires a running VM"}});
        auto* tape = heap.try_get_as<ObjectKind::Tape, types::Tape>(ops::payload(vm->active_tape()));
        if (!tape) return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError, "tape-ref-value: no active tape"}});
        auto idx = static_cast<uint32_t>(ops::payload(args[0]));
        if (idx >= tape->entries.size())
            return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError, "tape-ref-value: index out of range"}});
        return make_flonum(tape->entries[idx].primal);
    });

    // Fact-table builtins

    // Helper: extract FactTable* from a LispVal or return a type error.
    auto get_fact_table = [&heap](LispVal v, const char* who) -> std::expected<types::FactTable*, RuntimeError> {
        if (!ops::is_boxed(v) || ops::tag(v) != Tag::HeapObject)
            return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError, std::string(who) + ": argument must be a fact-table"}});
        auto* ft = heap.try_get_as<ObjectKind::FactTable, types::FactTable>(ops::payload(v));
        if (!ft)
            return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError, std::string(who) + ": argument must be a fact-table"}});
        return ft;
    };

    // fact-table? predicate
    env.register_builtin("fact-table?", 1, false, [&heap](Args args) -> std::expected<LispVal, RuntimeError> {
        if (ops::is_boxed(args[0]) && ops::tag(args[0]) == Tag::HeapObject) {
            if (heap.try_get_as<ObjectKind::FactTable, types::FactTable>(ops::payload(args[0])))
                return True;
        }
        return False;
    });

    // %make-fact-table : (col-name-list) → fact-table
    //   col-name-list is an Eta list of symbols or strings.
    env.register_builtin("%make-fact-table", 1, false, [&heap, &intern_table](Args args) -> std::expected<LispVal, RuntimeError> {
        // Walk the Eta list and collect column names
        std::vector<std::string> names;
        LispVal cur = args[0];
        while (cur != Nil) {
            if (!ops::is_boxed(cur) || ops::tag(cur) != Tag::HeapObject)
                return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError, "%make-fact-table: expected a list of column names"}});
            auto* cons = heap.try_get_as<ObjectKind::Cons, types::Cons>(ops::payload(cur));
            if (!cons)
                return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError, "%make-fact-table: expected a list of column names"}});
            LispVal name_val = cons->car;
            if (ops::is_boxed(name_val) && ops::tag(name_val) == Tag::Symbol) {
                auto sv = intern_table.get_string(ops::payload(name_val));
                names.push_back(sv ? std::string(*sv) : "?");
            } else if (ops::is_boxed(name_val) && ops::tag(name_val) == Tag::String) {
                auto sv = intern_table.get_string(ops::payload(name_val));
                names.push_back(sv ? std::string(*sv) : "?");
            } else {
                return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError, "%make-fact-table: column name must be a symbol or string"}});
            }
            cur = cons->cdr;
        }
        if (names.empty())
            return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError, "%make-fact-table: need at least one column"}});
        return make_fact_table(heap, std::move(names));
    });

    // %fact-table-insert! : (table row-list) → #t
    env.register_builtin("%fact-table-insert!", 2, false, [&heap, get_fact_table](Args args) -> std::expected<LispVal, RuntimeError> {
        auto ft_res = get_fact_table(args[0], "%fact-table-insert!");
        if (!ft_res) return std::unexpected(ft_res.error());
        auto* ft = *ft_res;
        // Walk the Eta list to collect row values
        std::vector<LispVal> row;
        LispVal cur = args[1];
        while (cur != Nil) {
            if (!ops::is_boxed(cur) || ops::tag(cur) != Tag::HeapObject)
                return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError, "%fact-table-insert!: second arg must be a list"}});
            auto* cons = heap.try_get_as<ObjectKind::Cons, types::Cons>(ops::payload(cur));
            if (!cons)
                return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError, "%fact-table-insert!: second arg must be a list"}});
            row.push_back(cons->car);
            cur = cons->cdr;
        }
        if (!ft->add_row(row))
            return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError, "%fact-table-insert!: row arity mismatch"}});
        return True;
    });

    // %fact-table-build-index! : (table col-index) → #t
    env.register_builtin("%fact-table-build-index!", 2, false, [get_fact_table](Args args) -> std::expected<LispVal, RuntimeError> {
        auto ft_res = get_fact_table(args[0], "%fact-table-build-index!");
        if (!ft_res) return std::unexpected(ft_res.error());
        auto col_opt = ops::decode<int64_t>(args[1]);
        if (!col_opt)
            return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError, "%fact-table-build-index!: column index must be a fixnum"}});
        (*ft_res)->build_index(static_cast<std::size_t>(*col_opt));
        return True;
    });

    // %fact-table-query : (table col-index key) → list of row-index fixnums
    env.register_builtin("%fact-table-query", 3, false, [&heap, get_fact_table](Args args) -> std::expected<LispVal, RuntimeError> {
        auto ft_res = get_fact_table(args[0], "%fact-table-query");
        if (!ft_res) return std::unexpected(ft_res.error());
        auto col_opt = ops::decode<int64_t>(args[1]);
        if (!col_opt)
            return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError, "%fact-table-query: column index must be a fixnum"}});
        auto rows = (*ft_res)->query(static_cast<std::size_t>(*col_opt), args[2]);
        // Build an Eta list of fixnums
        LispVal result = Nil;
        for (auto it = rows.rbegin(); it != rows.rend(); ++it) {
            auto enc = ops::encode(static_cast<int64_t>(*it));
            if (!enc) return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError, "%fact-table-query: row index too large"}});
            auto cell = make_cons(heap, *enc, result);
            if (!cell) return std::unexpected(cell.error());
            result = *cell;
        }
        return result;
    });

    // %fact-table-ref : (table row-index col-index) → value
    env.register_builtin("%fact-table-ref", 3, false, [get_fact_table](Args args) -> std::expected<LispVal, RuntimeError> {
        auto ft_res = get_fact_table(args[0], "%fact-table-ref");
        if (!ft_res) return std::unexpected(ft_res.error());
        auto row_opt = ops::decode<int64_t>(args[1]);
        auto col_opt = ops::decode<int64_t>(args[2]);
        if (!row_opt || !col_opt)
            return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError, "%fact-table-ref: indices must be fixnums"}});
        return (*ft_res)->get_cell(static_cast<std::size_t>(*row_opt), static_cast<std::size_t>(*col_opt));
    });

    // %fact-table-row-count : (table) → fixnum
    env.register_builtin("%fact-table-row-count", 1, false, [get_fact_table](Args args) -> std::expected<LispVal, RuntimeError> {
        auto ft_res = get_fact_table(args[0], "%fact-table-row-count");
        if (!ft_res) return std::unexpected(ft_res.error());
        return ops::encode(static_cast<int64_t>((*ft_res)->row_count));
    });

    // ================================================================
    // Statistics builtins (stats_math.h + stats_extract.h)
    //
    // All %stats-* primitives accept any numeric sequence (list, vector,
    // or fact-table column) via the polymorphic stats::to_eigen() helper
    // and return numeric results.  They provide the foundation for
    // std.stats.
    // ================================================================

    // %stats-mean : sequence → number
    env.register_builtin("%stats-mean", 1, false, [&heap](Args args) -> std::expected<LispVal, RuntimeError> {
        auto xs = stats::to_eigen(heap, args[0], "%stats-mean");
        if (!xs) return std::unexpected(xs.error());
        if (xs->size() == 0) return make_flonum(0.0);
        return make_flonum(stats::mean(*xs));
    });

    // %stats-variance : sequence → number  (sample variance, N-1)
    env.register_builtin("%stats-variance", 1, false, [&heap](Args args) -> std::expected<LispVal, RuntimeError> {
        auto xs = stats::to_eigen(heap, args[0], "%stats-variance");
        if (!xs) return std::unexpected(xs.error());
        if (xs->size() < 2)
            return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError, "%stats-variance: need at least 2 elements"}});
        return make_flonum(stats::variance(*xs));
    });

    // %stats-stddev : sequence → number
    env.register_builtin("%stats-stddev", 1, false, [&heap](Args args) -> std::expected<LispVal, RuntimeError> {
        auto xs = stats::to_eigen(heap, args[0], "%stats-stddev");
        if (!xs) return std::unexpected(xs.error());
        if (xs->size() < 2)
            return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError, "%stats-stddev: need at least 2 elements"}});
        return make_flonum(stats::stddev(*xs));
    });

    // %stats-sem : sequence → number  (standard error of mean)
    env.register_builtin("%stats-sem", 1, false, [&heap](Args args) -> std::expected<LispVal, RuntimeError> {
        auto xs = stats::to_eigen(heap, args[0], "%stats-sem");
        if (!xs) return std::unexpected(xs.error());
        if (xs->size() < 2)
            return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError, "%stats-sem: need at least 2 elements"}});
        return make_flonum(stats::sem(*xs));
    });

    // %stats-percentile : sequence p → number
    env.register_builtin("%stats-percentile", 2, false, [&heap](Args args) -> std::expected<LispVal, RuntimeError> {
        auto xs = stats::to_eigen(heap, args[0], "%stats-percentile");
        if (!xs) return std::unexpected(xs.error());
        auto pv = classify_numeric(args[1], heap);
        if (!pv.is_valid())
            return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError, "%stats-percentile: p must be a number"}});
        return make_flonum(stats::percentile(std::move(*xs), pv.as_double()));
    });

    // %stats-covariance : sequence1 sequence2 → number
    env.register_builtin("%stats-covariance", 2, false, [&heap](Args args) -> std::expected<LispVal, RuntimeError> {
        auto xs = stats::to_eigen(heap, args[0], "%stats-covariance");
        if (!xs) return std::unexpected(xs.error());
        auto ys = stats::to_eigen(heap, args[1], "%stats-covariance");
        if (!ys) return std::unexpected(ys.error());
        auto r = stats::covariance(*xs, *ys);
        if (!r) return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError, "%stats-covariance: sequences must be same length (≥2)"}});
        return make_flonum(*r);
    });

    // %stats-correlation : sequence1 sequence2 → number
    env.register_builtin("%stats-correlation", 2, false, [&heap](Args args) -> std::expected<LispVal, RuntimeError> {
        auto xs = stats::to_eigen(heap, args[0], "%stats-correlation");
        if (!xs) return std::unexpected(xs.error());
        auto ys = stats::to_eigen(heap, args[1], "%stats-correlation");
        if (!ys) return std::unexpected(ys.error());
        auto r = stats::correlation(*xs, *ys);
        if (!r) return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError, "%stats-correlation: sequences must be same length (≥2), non-constant"}});
        return make_flonum(*r);
    });

    // %stats-t-cdf : t-statistic df → p-value (cumulative)
    env.register_builtin("%stats-t-cdf", 2, false, [&heap](Args args) -> std::expected<LispVal, RuntimeError> {
        auto tv = classify_numeric(args[0], heap);
        auto dv = classify_numeric(args[1], heap);
        if (!tv.is_valid() || !dv.is_valid())
            return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError, "%stats-t-cdf: arguments must be numbers"}});
        return make_flonum(stats::t_cdf(tv.as_double(), dv.as_double()));
    });

    // %stats-t-quantile : p df → t-quantile
    env.register_builtin("%stats-t-quantile", 2, false, [&heap](Args args) -> std::expected<LispVal, RuntimeError> {
        auto pv = classify_numeric(args[0], heap);
        auto dv = classify_numeric(args[1], heap);
        if (!pv.is_valid() || !dv.is_valid())
            return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError, "%stats-t-quantile: arguments must be numbers"}});
        return make_flonum(stats::t_quantile(pv.as_double(), dv.as_double()));
    });

    // %stats-ci : sequence confidence-level → (lower . upper)
    env.register_builtin("%stats-ci", 2, false, [&heap](Args args) -> std::expected<LispVal, RuntimeError> {
        auto xs = stats::to_eigen(heap, args[0], "%stats-ci");
        if (!xs) return std::unexpected(xs.error());
        auto lv = classify_numeric(args[1], heap);
        if (!lv.is_valid())
            return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError, "%stats-ci: confidence level must be a number"}});
        auto ci = stats::ci_mean(*xs, lv.as_double());
        if (!ci)
            return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError, "%stats-ci: need at least 2 elements and 0<level<1"}});
        auto lo = make_flonum(ci->lower);
        auto hi = make_flonum(ci->upper);
        if (!lo || !hi) return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError, "%stats-ci: encoding error"}});
        return make_cons(heap, *lo, *hi);
    });

    // %stats-t-test-2 : sequence1 sequence2 → (t-stat p-value df mean-diff)
    env.register_builtin("%stats-t-test-2", 2, false, [&heap](Args args) -> std::expected<LispVal, RuntimeError> {
        auto xs = stats::to_eigen(heap, args[0], "%stats-t-test-2");
        if (!xs) return std::unexpected(xs.error());
        auto ys = stats::to_eigen(heap, args[1], "%stats-t-test-2");
        if (!ys) return std::unexpected(ys.error());
        auto r = stats::t_test_2(*xs, *ys);
        if (!r) return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError, "%stats-t-test-2: each sequence must have ≥2 elements"}});

        // Build result list: (t-stat p-value df mean-diff)
        auto v_md = make_flonum(r->mean_diff); if (!v_md) return std::unexpected(v_md.error());
        auto v_df = make_flonum(r->df);        if (!v_df) return std::unexpected(v_df.error());
        auto v_pv = make_flonum(r->p_value);   if (!v_pv) return std::unexpected(v_pv.error());
        auto v_ts = make_flonum(r->t_stat);    if (!v_ts) return std::unexpected(v_ts.error());

        auto l4 = make_cons(heap, *v_md, Nil);   if (!l4) return std::unexpected(l4.error());
        auto l3 = make_cons(heap, *v_df, *l4);   if (!l3) return std::unexpected(l3.error());
        auto l2 = make_cons(heap, *v_pv, *l3);   if (!l2) return std::unexpected(l2.error());
        auto l1 = make_cons(heap, *v_ts, *l2);   if (!l1) return std::unexpected(l1.error());
        return *l1;
    });

    // %stats-ols : x-sequence y-sequence → (slope intercept r² se-slope se-intercept t-slope t-intercept p-slope p-intercept)
    env.register_builtin("%stats-ols", 2, false, [&heap](Args args) -> std::expected<LispVal, RuntimeError> {
        auto xs = stats::to_eigen(heap, args[0], "%stats-ols");
        if (!xs) return std::unexpected(xs.error());
        auto ys = stats::to_eigen(heap, args[1], "%stats-ols");
        if (!ys) return std::unexpected(ys.error());
        auto r = stats::ols(*xs, *ys);
        if (!r) return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError, "%stats-ols: sequences must be same length (≥3)"}});

        // Build result list: (slope intercept r² se-slope se-intercept t-slope t-intercept p-slope p-intercept)
        auto v9 = make_flonum(r->p_intercept); if (!v9) return std::unexpected(v9.error());
        auto v8 = make_flonum(r->p_slope);     if (!v8) return std::unexpected(v8.error());
        auto v7 = make_flonum(r->t_intercept); if (!v7) return std::unexpected(v7.error());
        auto v6 = make_flonum(r->t_slope);     if (!v6) return std::unexpected(v6.error());
        auto v5 = make_flonum(r->se_intercept);if (!v5) return std::unexpected(v5.error());
        auto v4 = make_flonum(r->se_slope);    if (!v4) return std::unexpected(v4.error());
        auto v3 = make_flonum(r->r_squared);   if (!v3) return std::unexpected(v3.error());
        auto v2 = make_flonum(r->intercept);   if (!v2) return std::unexpected(v2.error());
        auto v1 = make_flonum(r->slope);       if (!v1) return std::unexpected(v1.error());

        auto l9 = make_cons(heap, *v9, Nil);  if (!l9) return std::unexpected(l9.error());
        auto l8 = make_cons(heap, *v8, *l9);  if (!l8) return std::unexpected(l8.error());
        auto l7 = make_cons(heap, *v7, *l8);  if (!l7) return std::unexpected(l7.error());
        auto l6 = make_cons(heap, *v6, *l7);  if (!l6) return std::unexpected(l6.error());
        auto l5 = make_cons(heap, *v5, *l6);  if (!l5) return std::unexpected(l5.error());
        auto l4 = make_cons(heap, *v4, *l5);  if (!l4) return std::unexpected(l4.error());
        auto l3 = make_cons(heap, *v3, *l4);  if (!l3) return std::unexpected(l3.error());
        auto l2 = make_cons(heap, *v2, *l3);  if (!l2) return std::unexpected(l2.error());
        auto l1 = make_cons(heap, *v1, *l2);  if (!l1) return std::unexpected(l1.error());
        return *l1;
    });

    // ========================================================================
    // Phase 4b — propagation queue
    // ------------------------------------------------------------------------
    // (register-prop-attr! 'key)
    //   Marks attribute key 'key as carrying a list of re-propagator thunks.
    //   When `unify` later binds a logic var carrying this attribute, every
    //   thunk in the attribute's value (a list) is *enqueued* on the VM's
    //   FIFO propagation queue rather than invoked synchronously.  The queue
    //   drains at the outer-`unify` boundary; thunk return values of #f
    //   abort the unify and trigger the standard atomic rollback.
    //   Idempotent on closure identity — the same thunk is never queued twice.
    //
    // (%clp-prop-queue-size)
    //   Returns the current pending-thunk count — useful for tests / REPL
    //   diagnostics; always 0 outside an active unify.
    // ========================================================================

    env.register_builtin("register-prop-attr!", 1, false,
        [get_symbol_id, vm](Args args) -> std::expected<LispVal, RuntimeError> {
            if (!vm) return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError,
                "register-prop-attr!: requires a running VM"}});
            auto key = get_symbol_id(args[0]);
            if (!key)
                return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError,
                    "register-prop-attr!: arg must be a symbol (attribute key)"}});
            vm->async_thunk_attrs().insert(*key);
            return True;
        });

    env.register_builtin("%clp-prop-queue-size", 0, false,
        [vm](Args /*args*/) -> std::expected<LispVal, RuntimeError> {
            if (!vm) return ops::encode<int64_t>(0).value_or(Nil);
            auto enc = ops::encode<int64_t>(static_cast<int64_t>(vm->prop_queue_size()));
            if (!enc) return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError,
                "%clp-prop-queue-size: queue size out of fixnum range"}});
            return *enc;
        });
}

} // namespace eta::runtime

