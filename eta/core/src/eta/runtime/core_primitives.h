#pragma once

#include <algorithm>
#include <atomic>
#include <bit>
#include <climits>
#include <cmath>
#include <cstdint>
#include <functional>
#include <iostream>
#include <limits>
#include <optional>
#include <sstream>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "eta/runtime/builtin_env.h"
#include "eta/runtime/ad_error.h"
#include "eta/runtime/numeric_value.h"
#include "eta/runtime/overflow.h"
#include "eta/runtime/factory.h"
#include "eta/runtime/string_view.h"
#include "eta/runtime/value_formatter.h"
#include "eta/runtime/csv_builtins.h"
#include "eta/runtime/regex_builtins.h"
#include "eta/runtime/vm/vm.h"
#include "eta/runtime/types/logic_var.h"
#include "eta/runtime/types/tape.h"
#include "eta/runtime/types/tape_ref.h"
#include "eta/runtime/clp/domain.h"
#include "eta/runtime/clp/constraint_store.h"
#include "eta/runtime/clp/alldiff_regin.h"
#include "eta/runtime/clp/linear.h"
#include "eta/runtime/clp/quadratic.h"
#include "eta/runtime/clp/qp_solver.h"
#include "eta/runtime/clp/fm.h"
#include "eta/runtime/clp/simplex.h"
#include "eta/runtime/stats_math.h"
#include "eta/runtime/stats_extract.h"

namespace eta::runtime {


/**
 * @brief Register all core primitives into a BuiltinEnvironment
 *
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
 *
 * All primitives capture Heap& and/or InternTable& by reference where needed.
 */
inline void register_core_primitives(BuiltinEnvironment& env, Heap& heap, InternTable& intern_table,
                                     vm::VM* vm = nullptr) {
    using Args = std::span<const LispVal>;

    /**
     * AD TapeRef helper
     * Helper: check whether any element is a TapeRef
     */
    auto has_tape_ref = [](Args args) -> bool {
        for (auto v : args) {
            if (ops::is_boxed(v) && ops::tag(v) == Tag::TapeRef)
                return true;
        }
        return false;
    };

    /**
     * Allocate compact tape IDs used by TapeRef ownership checks.
     * 0 is reserved as invalid.
     */
    auto allocate_tape_id = []() -> uint32_t {
        static std::atomic<uint32_t> next_id{1};
        const uint32_t raw = next_id.fetch_add(1, std::memory_order_relaxed);
        return static_cast<uint32_t>(((raw - 1u) % types::tape_ref::MAX_TAPE_ID) + 1u);
    };

    auto make_ad_runtime_error = [](const char* tag, std::string message,
                                    std::vector<error::VMErrorField> fields = {}) -> RuntimeError {
        return ad::make_error(tag, std::move(message), std::move(fields));
    };

    auto validate_ref_for_tape =
        [make_ad_runtime_error](types::Tape* tape,
                                 LispVal ref,
                                 const char* op_name,
                                 const char* role) -> std::expected<uint32_t, RuntimeError> {
            const auto parts = types::tape_ref::decode(ref);
            if (parts.tape_id != tape->tape_id) {
                return std::unexpected(make_ad_runtime_error(
                    ad::kTagMixedTape,
                    std::string(op_name) + ": reference belongs to a different tape",
                    {
                        ad::field("op", std::string(op_name)),
                        ad::field("role", std::string(role)),
                        ad::field("expected-tape-id", tape->tape_id),
                        ad::field("actual-tape-id", parts.tape_id),
                        ad::field("generation", parts.generation),
                        ad::field("node-index", parts.node_index)
                    }));
            }
            if (parts.generation != tape->generation) {
                return std::unexpected(make_ad_runtime_error(
                    ad::kTagStaleRef,
                    std::string(op_name) + ": stale TapeRef generation",
                    {
                        ad::field("op", std::string(op_name)),
                        ad::field("role", std::string(role)),
                        ad::field("tape-id", tape->tape_id),
                        ad::field("expected-gen", tape->generation),
                        ad::field("actual-gen", parts.generation),
                        ad::field("node-index", parts.node_index)
                    }));
            }
            if (parts.node_index >= tape->entries.size()) {
                return std::unexpected(make_ad_runtime_error(
                    ad::kTagStaleRef,
                    std::string(op_name) + ": TapeRef index out of range",
                    {
                        ad::field("op", std::string(op_name)),
                        ad::field("role", std::string(role)),
                        ad::field("tape-id", tape->tape_id),
                        ad::field("expected-gen", tape->generation),
                        ad::field("actual-gen", parts.generation),
                        ad::field("node-index", parts.node_index)
                    }));
            }
            return parts.node_index;
        };

    auto policy_is_strict = [vm]() -> bool {
        return vm && vm->aad_nondiff_policy() == vm::VM::AadNondiffPolicy::Strict;
    };

    auto make_nondiff_error = [make_ad_runtime_error](std::string op, std::string detail) -> RuntimeError {
        return make_ad_runtime_error(
            ad::kTagNondiffStrict,
            op + ": non-differentiable point reached in strict mode",
            {
                ad::field("op", std::move(op)),
                ad::field("detail", std::move(detail))
            });
    };

    auto make_domain_error = [make_ad_runtime_error](std::string op, double base, double exponent, std::string detail)
        -> RuntimeError {
        return make_ad_runtime_error(
            ad::kTagDomain,
            op + ": domain violation",
            {
                ad::field("op", std::move(op)),
                ad::field("base", base),
                ad::field("exponent", exponent),
                ad::field("detail", std::move(detail))
            });
    };

    auto make_unary_domain_error = [make_ad_runtime_error](std::string op, double value, std::string detail)
        -> RuntimeError {
        return make_ad_runtime_error(
            ad::kTagDomain,
            op + ": domain violation",
            {
                ad::field("op", std::move(op)),
                ad::field("value", value),
                ad::field("detail", std::move(detail))
            });
    };

    auto get_active_tape_for_op = [&heap, vm, make_ad_runtime_error](const char* op_name)
        -> std::expected<types::Tape*, RuntimeError> {
        if (!vm) {
            return std::unexpected(RuntimeError{VMError{
                RuntimeErrorCode::TypeError, std::string(op_name) + ": requires a running VM"}});
        }
        const LispVal active = vm->active_tape();
        auto* tape = (ops::is_boxed(active) && ops::tag(active) == Tag::HeapObject)
            ? heap.try_get_as<ObjectKind::Tape, types::Tape>(ops::payload(active))
            : nullptr;
        if (!tape) {
            return std::unexpected(make_ad_runtime_error(
                ad::kTagNoActiveTape,
                std::string(op_name) + ": no active tape",
                {ad::field("op", std::string(op_name))}));
        }
        return tape;
    };

    /**
     * Arithmetic: + - * /
     *
     * Each operator checks for TapeRef arguments.  When found, the operation
     * is folded through VM::tape_binary_op() which delegates to
     * operations for reverse-mode AD.
     */

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
                /// Overflow check: promote to double on overflow
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
                /// Unary negation: 0 - x
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
            /// Unary negation
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
                /// Unary reciprocal: 1 / x
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
            /// Unary: reciprocal
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
                    /// Non-exact: promote to double
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

    /**
     * Comparison: = < > <= >=
     */

    auto make_comparison = [&heap, vm, validate_ref_for_tape, get_active_tape_for_op, policy_is_strict, make_nondiff_error]
        (const char* name, auto cmp_int, auto cmp_float) {
        return [&heap, vm, validate_ref_for_tape, get_active_tape_for_op, policy_is_strict, make_nondiff_error,
                name, cmp_int, cmp_float](Args args) -> std::expected<LispVal, RuntimeError> {
            if (args.size() < 2) return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::InvalidArity, std::string(name) + ": requires at least 2 arguments"}});
            for (size_t i = 0; i + 1 < args.size(); ++i) {
                const bool a_is_ref = types::tape_ref::is_tape_ref(args[i]);
                const bool b_is_ref = types::tape_ref::is_tape_ref(args[i + 1]);

                if (a_is_ref || b_is_ref) {
                    if (policy_is_strict()) {
                        return std::unexpected(make_nondiff_error(name, "comparison"));
                    }
                    auto tape = get_active_tape_for_op(name);
                    if (!tape) return std::unexpected(tape.error());

                    auto extract_ref = [&](LispVal v, bool is_ref, const char* role)
                        -> std::expected<double, RuntimeError> {
                        if (!is_ref) {
                            auto n = classify_numeric(v, heap);
                            if (!n.is_valid()) {
                                return std::unexpected(RuntimeError{VMError{
                                    RuntimeErrorCode::TypeError, std::string(name) + ": argument is not a number"}});
                            }
                            return n.as_double();
                        }
                        auto idx = validate_ref_for_tape(*tape, v, name, role);
                        if (!idx) return std::unexpected(idx.error());
                        return (*tape)->entries[*idx].primal;
                    };

                    auto da = extract_ref(args[i], a_is_ref, "lhs");
                    if (!da) return std::unexpected(da.error());
                    auto db = extract_ref(args[i + 1], b_is_ref, "rhs");
                    if (!db) return std::unexpected(db.error());
                    if (!cmp_float(*da, *db)) return nanbox::False;
                    continue;
                }

                auto a = classify_numeric(args[i], heap);
                auto b = classify_numeric(args[i + 1], heap);
                if (!a.is_valid() || !b.is_valid()) {
                    return std::unexpected(RuntimeError{VMError{
                        RuntimeErrorCode::TypeError, std::string(name) + ": argument is not a number"}});
                }
                if (a.is_flonum() || b.is_flonum()) {
                    if (!cmp_float(a.as_double(), b.as_double())) return nanbox::False;
                } else {
                    if (!cmp_int(a.int_val, b.int_val)) return nanbox::False;
                }
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
            if (na.is_fixnum() && nb.is_fixnum()) return (na.int_val == nb.int_val) ? nanbox::True : nanbox::False;
            if (na.is_flonum() && nb.is_flonum()) return (na.float_val == nb.float_val) ? nanbox::True : nanbox::False;
        }
        return nanbox::False;
    });

    env.register_builtin("not", 1, false, [](Args args) -> std::expected<LispVal, RuntimeError> {
        return (args[0] == nanbox::False) ? nanbox::True : nanbox::False;
    });

    /**
     * Pairs / Lists: cons car cdr pair? null? list
     */

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

    /**
     * Type predicates
     */

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

    /**
     * Note: I/O primitives (display, newline) have been moved to io_primitives.h
     * They now support port-based output and require VM access.
     */


    /**
     * Numeric predicates: zero? positive? negative?
     */

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

    /**
     * Numeric operations: abs min max modulo remainder
     */

    auto resolve_tape_numeric =
        [&heap, validate_ref_for_tape](types::Tape* tape,
                                       LispVal value,
                                       const char* op_name,
                                       const char* role) -> std::expected<uint32_t, RuntimeError> {
            if (types::tape_ref::is_tape_ref(value)) {
                return validate_ref_for_tape(tape, value, op_name, role);
            }
            auto n = classify_numeric(value, heap);
            if (!n.is_valid()) {
                return std::unexpected(RuntimeError{VMError{
                    RuntimeErrorCode::TypeError, std::string(op_name) + ": argument is not a number"}});
            }
            const uint32_t idx = tape->push_const(n.as_double());
            if (idx > types::tape_ref::MAX_NODE_INDEX) {
                return std::unexpected(RuntimeError{VMError{
                    RuntimeErrorCode::InternalError,
                    std::string(op_name) + ": tape node index exceeds TapeRef capacity"}});
            }
            return idx;
        };

    auto make_tape_ref_result = [](types::Tape* tape, uint32_t idx, const char* op_name)
        -> std::expected<LispVal, RuntimeError> {
        if (idx > types::tape_ref::MAX_NODE_INDEX) {
            return std::unexpected(RuntimeError{VMError{
                RuntimeErrorCode::InternalError,
                std::string(op_name) + ": tape node index exceeds TapeRef capacity"}});
        }
        return types::tape_ref::make(tape->tape_id, tape->generation, idx);
    };

    env.register_builtin("abs", 1, false,
        [&heap, vm, policy_is_strict, make_nondiff_error, get_active_tape_for_op,
         resolve_tape_numeric, make_tape_ref_result](Args args) -> std::expected<LispVal, RuntimeError> {
            if (vm && types::tape_ref::is_tape_ref(args[0])) {
                auto tape = get_active_tape_for_op("abs");
                if (!tape) return std::unexpected(tape.error());
                auto idx = resolve_tape_numeric(*tape, args[0], "abs", "arg");
                if (!idx) return std::unexpected(idx.error());
                const double x = (*tape)->entries[*idx].primal;
                if (x == 0.0 && policy_is_strict()) {
                    return std::unexpected(make_nondiff_error("abs", "x == 0"));
                }
                const uint32_t out = (*tape)->push({types::TapeOp::Abs, *idx, *idx, std::abs(x), 0.0});
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

    env.register_builtin("min", 2, true,
        [&heap, vm, has_tape_ref, policy_is_strict, make_nondiff_error,
         get_active_tape_for_op, resolve_tape_numeric, make_tape_ref_result]
        (Args args) -> std::expected<LispVal, RuntimeError> {
            if (vm && has_tape_ref(args)) {
                auto tape = get_active_tape_for_op("min");
                if (!tape) return std::unexpected(tape.error());
                auto best_idx = resolve_tape_numeric(*tape, args[0], "min", "arg[0]");
                if (!best_idx) return std::unexpected(best_idx.error());
                for (size_t i = 1; i < args.size(); ++i) {
                    auto cur_idx = resolve_tape_numeric(
                        *tape, args[i], "min", ("arg[" + std::to_string(i) + "]").c_str());
                    if (!cur_idx) return std::unexpected(cur_idx.error());
                    const double a = (*tape)->entries[*best_idx].primal;
                    const double b = (*tape)->entries[*cur_idx].primal;
                    if (a == b && policy_is_strict()) {
                        return std::unexpected(make_nondiff_error("min", "tie (a == b)"));
                    }
                    const uint32_t out = (*tape)->push({
                        types::TapeOp::Min, *best_idx, *cur_idx, std::min(a, b), 0.0});
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

    env.register_builtin("max", 2, true,
        [&heap, vm, has_tape_ref, policy_is_strict, make_nondiff_error,
         get_active_tape_for_op, resolve_tape_numeric, make_tape_ref_result]
        (Args args) -> std::expected<LispVal, RuntimeError> {
            if (vm && has_tape_ref(args)) {
                auto tape = get_active_tape_for_op("max");
                if (!tape) return std::unexpected(tape.error());
                auto best_idx = resolve_tape_numeric(*tape, args[0], "max", "arg[0]");
                if (!best_idx) return std::unexpected(best_idx.error());
                for (size_t i = 1; i < args.size(); ++i) {
                    auto cur_idx = resolve_tape_numeric(
                        *tape, args[i], "max", ("arg[" + std::to_string(i) + "]").c_str());
                    if (!cur_idx) return std::unexpected(cur_idx.error());
                    const double a = (*tape)->entries[*best_idx].primal;
                    const double b = (*tape)->entries[*cur_idx].primal;
                    if (a == b && policy_is_strict()) {
                        return std::unexpected(make_nondiff_error("max", "tie (a == b)"));
                    }
                    const uint32_t out = (*tape)->push({
                        types::TapeOp::Max, *best_idx, *cur_idx, std::max(a, b), 0.0});
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
        if (!a.is_valid() || !b.is_valid()) return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError, "modulo: arguments must be numbers"}});
        if (a.is_flonum() || b.is_flonum()) return make_flonum(std::fmod(a.as_double(), b.as_double()));
        if (b.int_val == 0) return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError, "modulo: division by zero"}});
        /// Scheme modulo: result has same sign as divisor
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

    /**
     * Transcendental math: sin cos tan asin acos atan atan2 exp log sqrt
     */

    env.register_builtin("sin", 1, false, [&heap, vm, validate_ref_for_tape, make_ad_runtime_error](Args args) -> std::expected<LispVal, RuntimeError> {
        /// Tape-aware: record sin on active tape
        if (vm && ops::is_boxed(args[0]) && ops::tag(args[0]) == Tag::TapeRef) {
            const LispVal active = vm->active_tape();
            auto* tape = (ops::is_boxed(active) && ops::tag(active) == Tag::HeapObject)
                ? heap.try_get_as<ObjectKind::Tape, types::Tape>(ops::payload(active))
                : nullptr;
            if (!tape) {
                return std::unexpected(make_ad_runtime_error(
                    ad::kTagNoActiveTape,
                    "sin: no active tape",
                    {ad::field("op", std::string("sin"))}));
            }
            auto idx = validate_ref_for_tape(tape, args[0], "sin", "arg");
            if (!idx) return std::unexpected(idx.error());
            double val = std::sin(tape->entries[*idx].primal);
            uint32_t new_idx = tape->push({types::TapeOp::Sin, *idx, 0, val, 0.0});
            if (new_idx > types::tape_ref::MAX_NODE_INDEX) {
                return std::unexpected(RuntimeError{VMError{
                    RuntimeErrorCode::InternalError,
                    "sin: tape node index exceeds TapeRef capacity"}});
            }
            return types::tape_ref::make(tape->tape_id, tape->generation, new_idx);
        }
        auto n = classify_numeric(args[0], heap);
        if (!n.is_valid()) return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError, "sin: argument is not a number"}});
        return make_flonum(std::sin(n.as_double()));
    });

    env.register_builtin("cos", 1, false, [&heap, vm, validate_ref_for_tape, make_ad_runtime_error](Args args) -> std::expected<LispVal, RuntimeError> {
        /// Tape-aware: record cos on active tape
        if (vm && ops::is_boxed(args[0]) && ops::tag(args[0]) == Tag::TapeRef) {
            const LispVal active = vm->active_tape();
            auto* tape = (ops::is_boxed(active) && ops::tag(active) == Tag::HeapObject)
                ? heap.try_get_as<ObjectKind::Tape, types::Tape>(ops::payload(active))
                : nullptr;
            if (!tape) {
                return std::unexpected(make_ad_runtime_error(
                    ad::kTagNoActiveTape,
                    "cos: no active tape",
                    {ad::field("op", std::string("cos"))}));
            }
            auto idx = validate_ref_for_tape(tape, args[0], "cos", "arg");
            if (!idx) return std::unexpected(idx.error());
            double val = std::cos(tape->entries[*idx].primal);
            uint32_t new_idx = tape->push({types::TapeOp::Cos, *idx, 0, val, 0.0});
            if (new_idx > types::tape_ref::MAX_NODE_INDEX) {
                return std::unexpected(RuntimeError{VMError{
                    RuntimeErrorCode::InternalError,
                    "cos: tape node index exceeds TapeRef capacity"}});
            }
            return types::tape_ref::make(tape->tape_id, tape->generation, new_idx);
        }
        auto n = classify_numeric(args[0], heap);
        if (!n.is_valid()) return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError, "cos: argument is not a number"}});
        return make_flonum(std::cos(n.as_double()));
    });

    env.register_builtin("tan", 1, false,
        [&heap, vm, validate_ref_for_tape, make_ad_runtime_error, get_active_tape_for_op](Args args)
            -> std::expected<LispVal, RuntimeError> {
            if (vm && types::tape_ref::is_tape_ref(args[0])) {
                auto tape = get_active_tape_for_op("tan");
                if (!tape) return std::unexpected(tape.error());
                auto idx = validate_ref_for_tape(*tape, args[0], "tan", "arg");
                if (!idx) return std::unexpected(idx.error());
                const double val = std::tan((*tape)->entries[*idx].primal);
                const uint32_t out = (*tape)->push({types::TapeOp::Tan, *idx, 0, val, 0.0});
                if (out > types::tape_ref::MAX_NODE_INDEX) {
                    return std::unexpected(RuntimeError{VMError{
                        RuntimeErrorCode::InternalError,
                        "tan: tape node index exceeds TapeRef capacity"}});
                }
                return types::tape_ref::make((*tape)->tape_id, (*tape)->generation, out);
            }
            auto n = classify_numeric(args[0], heap);
            if (!n.is_valid()) {
                return std::unexpected(RuntimeError{VMError{
                    RuntimeErrorCode::TypeError, "tan: argument is not a number"}});
            }
            return make_flonum(std::tan(n.as_double()));
        });

    env.register_builtin("asin", 1, false,
        [&heap, vm, validate_ref_for_tape, get_active_tape_for_op, make_unary_domain_error](Args args)
            -> std::expected<LispVal, RuntimeError> {
            if (vm && types::tape_ref::is_tape_ref(args[0])) {
                auto tape = get_active_tape_for_op("asin");
                if (!tape) return std::unexpected(tape.error());
                auto idx = validate_ref_for_tape(*tape, args[0], "asin", "arg");
                if (!idx) return std::unexpected(idx.error());
                const double x = (*tape)->entries[*idx].primal;
                if (x < -1.0 || x > 1.0) {
                    return std::unexpected(make_unary_domain_error("asin", x, "requires -1 <= x <= 1"));
                }
                const double val = std::asin(x);
                const uint32_t out = (*tape)->push({types::TapeOp::Asin, *idx, 0, val, 0.0});
                if (out > types::tape_ref::MAX_NODE_INDEX) {
                    return std::unexpected(RuntimeError{VMError{
                        RuntimeErrorCode::InternalError,
                        "asin: tape node index exceeds TapeRef capacity"}});
                }
                return types::tape_ref::make((*tape)->tape_id, (*tape)->generation, out);
            }
            auto n = classify_numeric(args[0], heap);
            if (!n.is_valid()) {
                return std::unexpected(RuntimeError{VMError{
                    RuntimeErrorCode::TypeError, "asin: argument is not a number"}});
            }
            return make_flonum(std::asin(n.as_double()));
        });

    env.register_builtin("acos", 1, false,
        [&heap, vm, validate_ref_for_tape, get_active_tape_for_op, make_unary_domain_error](Args args)
            -> std::expected<LispVal, RuntimeError> {
            if (vm && types::tape_ref::is_tape_ref(args[0])) {
                auto tape = get_active_tape_for_op("acos");
                if (!tape) return std::unexpected(tape.error());
                auto idx = validate_ref_for_tape(*tape, args[0], "acos", "arg");
                if (!idx) return std::unexpected(idx.error());
                const double x = (*tape)->entries[*idx].primal;
                if (x < -1.0 || x > 1.0) {
                    return std::unexpected(make_unary_domain_error("acos", x, "requires -1 <= x <= 1"));
                }
                const double val = std::acos(x);
                const uint32_t out = (*tape)->push({types::TapeOp::Acos, *idx, 0, val, 0.0});
                if (out > types::tape_ref::MAX_NODE_INDEX) {
                    return std::unexpected(RuntimeError{VMError{
                        RuntimeErrorCode::InternalError,
                        "acos: tape node index exceeds TapeRef capacity"}});
                }
                return types::tape_ref::make((*tape)->tape_id, (*tape)->generation, out);
            }
            auto n = classify_numeric(args[0], heap);
            if (!n.is_valid()) {
                return std::unexpected(RuntimeError{VMError{
                    RuntimeErrorCode::TypeError, "acos: argument is not a number"}});
            }
            return make_flonum(std::acos(n.as_double()));
        });

    env.register_builtin("atan", 1, true,
        [&heap, vm, validate_ref_for_tape, get_active_tape_for_op](Args args)
            -> std::expected<LispVal, RuntimeError> {
            if (args.size() == 1 && vm && types::tape_ref::is_tape_ref(args[0])) {
                auto tape = get_active_tape_for_op("atan");
                if (!tape) return std::unexpected(tape.error());
                auto idx = validate_ref_for_tape(*tape, args[0], "atan", "arg");
                if (!idx) return std::unexpected(idx.error());
                const double val = std::atan((*tape)->entries[*idx].primal);
                const uint32_t out = (*tape)->push({types::TapeOp::Atan, *idx, 0, val, 0.0});
                if (out > types::tape_ref::MAX_NODE_INDEX) {
                    return std::unexpected(RuntimeError{VMError{
                        RuntimeErrorCode::InternalError,
                        "atan: tape node index exceeds TapeRef capacity"}});
                }
                return types::tape_ref::make((*tape)->tape_id, (*tape)->generation, out);
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

    env.register_builtin("exp", 1, false, [&heap, vm, validate_ref_for_tape, make_ad_runtime_error](Args args) -> std::expected<LispVal, RuntimeError> {
        /// Tape-aware: record exp on active tape
        if (vm && ops::is_boxed(args[0]) && ops::tag(args[0]) == Tag::TapeRef) {
            const LispVal active = vm->active_tape();
            auto* tape = (ops::is_boxed(active) && ops::tag(active) == Tag::HeapObject)
                ? heap.try_get_as<ObjectKind::Tape, types::Tape>(ops::payload(active))
                : nullptr;
            if (!tape) {
                return std::unexpected(make_ad_runtime_error(
                    ad::kTagNoActiveTape,
                    "exp: no active tape",
                    {ad::field("op", std::string("exp"))}));
            }
            auto idx = validate_ref_for_tape(tape, args[0], "exp", "arg");
            if (!idx) return std::unexpected(idx.error());
            double val = std::exp(tape->entries[*idx].primal);
            uint32_t new_idx = tape->push({types::TapeOp::Exp, *idx, 0, val, 0.0});
            if (new_idx > types::tape_ref::MAX_NODE_INDEX) {
                return std::unexpected(RuntimeError{VMError{
                    RuntimeErrorCode::InternalError,
                    "exp: tape node index exceeds TapeRef capacity"}});
            }
            return types::tape_ref::make(tape->tape_id, tape->generation, new_idx);
        }
        auto n = classify_numeric(args[0], heap);
        if (!n.is_valid()) return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError, "exp: argument is not a number"}});
        return make_flonum(std::exp(n.as_double()));
    });

    env.register_builtin("log", 1, false,
        [&heap, vm, validate_ref_for_tape, make_ad_runtime_error, make_unary_domain_error](Args args)
            -> std::expected<LispVal, RuntimeError> {
        /// Tape-aware: record log on active tape
        if (vm && ops::is_boxed(args[0]) && ops::tag(args[0]) == Tag::TapeRef) {
            const LispVal active = vm->active_tape();
            auto* tape = (ops::is_boxed(active) && ops::tag(active) == Tag::HeapObject)
                ? heap.try_get_as<ObjectKind::Tape, types::Tape>(ops::payload(active))
                : nullptr;
            if (!tape) {
                return std::unexpected(make_ad_runtime_error(
                    ad::kTagNoActiveTape,
                    "log: no active tape",
                    {ad::field("op", std::string("log"))}));
            }
            auto idx = validate_ref_for_tape(tape, args[0], "log", "arg");
            if (!idx) return std::unexpected(idx.error());
            const double x = tape->entries[*idx].primal;
            if (x <= 0.0) {
                return std::unexpected(make_unary_domain_error("log", x, "requires x > 0"));
            }
            double val = std::log(x);
            uint32_t new_idx = tape->push({types::TapeOp::Log, *idx, 0, val, 0.0});
            if (new_idx > types::tape_ref::MAX_NODE_INDEX) {
                return std::unexpected(RuntimeError{VMError{
                    RuntimeErrorCode::InternalError,
                    "log: tape node index exceeds TapeRef capacity"}});
            }
            return types::tape_ref::make(tape->tape_id, tape->generation, new_idx);
        }
        auto n = classify_numeric(args[0], heap);
        if (!n.is_valid()) return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError, "log: argument is not a number"}});
        return make_flonum(std::log(n.as_double()));
    });

    env.register_builtin("sqrt", 1, false,
        [&heap, vm, validate_ref_for_tape, make_ad_runtime_error, make_unary_domain_error](Args args)
            -> std::expected<LispVal, RuntimeError> {
        /// Tape-aware: record sqrt on active tape
        if (vm && ops::is_boxed(args[0]) && ops::tag(args[0]) == Tag::TapeRef) {
            const LispVal active = vm->active_tape();
            auto* tape = (ops::is_boxed(active) && ops::tag(active) == Tag::HeapObject)
                ? heap.try_get_as<ObjectKind::Tape, types::Tape>(ops::payload(active))
                : nullptr;
            if (!tape) {
                return std::unexpected(make_ad_runtime_error(
                    ad::kTagNoActiveTape,
                    "sqrt: no active tape",
                    {ad::field("op", std::string("sqrt"))}));
            }
            auto idx = validate_ref_for_tape(tape, args[0], "sqrt", "arg");
            if (!idx) return std::unexpected(idx.error());
            const double x = tape->entries[*idx].primal;
            if (x < 0.0) {
                return std::unexpected(make_unary_domain_error("sqrt", x, "requires x >= 0"));
            }
            double val = std::sqrt(x);
            uint32_t new_idx = tape->push({types::TapeOp::Sqrt, *idx, 0, val, 0.0});
            if (new_idx > types::tape_ref::MAX_NODE_INDEX) {
                return std::unexpected(RuntimeError{VMError{
                    RuntimeErrorCode::InternalError,
                    "sqrt: tape node index exceeds TapeRef capacity"}});
            }
            return types::tape_ref::make(tape->tape_id, tape->generation, new_idx);
        }
        auto n = classify_numeric(args[0], heap);
        if (!n.is_valid()) return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError, "sqrt: argument is not a number"}});
        return make_flonum(std::sqrt(n.as_double()));
    });

    env.register_builtin("pow", 2, false,
        [&heap, vm, resolve_tape_numeric, get_active_tape_for_op, policy_is_strict,
         make_domain_error, make_tape_ref_result](Args args) -> std::expected<LispVal, RuntimeError> {
            auto lhs_is_ref = types::tape_ref::is_tape_ref(args[0]);
            auto rhs_is_ref = types::tape_ref::is_tape_ref(args[1]);

            if (vm && (lhs_is_ref || rhs_is_ref)) {
                auto tape = get_active_tape_for_op("pow");
                if (!tape) return std::unexpected(tape.error());

                auto lhs_idx = resolve_tape_numeric(*tape, args[0], "pow", "base");
                if (!lhs_idx) return std::unexpected(lhs_idx.error());
                auto rhs_idx = resolve_tape_numeric(*tape, args[1], "pow", "exponent");
                if (!rhs_idx) return std::unexpected(rhs_idx.error());

                const double base = (*tape)->entries[*lhs_idx].primal;
                const double exponent = (*tape)->entries[*rhs_idx].primal;
                const bool exponent_is_integer = std::isfinite(exponent) && std::floor(exponent) == exponent;

                if (base < 0.0 && !exponent_is_integer) {
                    return std::unexpected(make_domain_error(
                        "pow", base, exponent, "negative base requires an integer exponent"));
                }
                if (base == 0.0 && exponent < 0.0) {
                    return std::unexpected(make_domain_error(
                        "pow", base, exponent, "0 raised to a negative exponent is undefined"));
                }
                if (policy_is_strict()) {
                    if (base == 0.0 && exponent == 0.0) {
                        return std::unexpected(make_domain_error(
                            "pow", base, exponent, "strict mode rejects derivative at pow(0, 0)"));
                    }
                    if (base == 0.0 && exponent > 0.0 && exponent < 1.0) {
                        return std::unexpected(make_domain_error(
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
                return make_tape_ref_result(*tape, out, "pow");
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

    /**
     * List operations: length append reverse list-ref
     */

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

        /// Start from the last argument (which doesn't need to be a list)
        LispVal result = args.back();

        /// Process arguments right-to-left (all but the last must be proper lists)
        for (auto it = args.rbegin() + 1; it != args.rend(); ++it) {
            /// Collect elements of this list
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
            /// Build from right to left
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
        LispVal key = args[0];
        LispVal cur = args[1];
        while (cur != nanbox::Nil) {
            if (!ops::is_boxed(cur) || ops::tag(cur) != Tag::HeapObject)
                return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError, "assq: not a proper alist"}});
            auto* outer = heap.try_get_as<ObjectKind::Cons, types::Cons>(ops::payload(cur));
            if (!outer) return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError, "assq: not a proper alist"}});
            /// Each element must be a pair
            LispVal pair = outer->car;
            if (ops::is_boxed(pair) && ops::tag(pair) == Tag::HeapObject) {
                auto* inner = heap.try_get_as<ObjectKind::Cons, types::Cons>(ops::payload(pair));
                if (inner && inner->car == key) {
                    return pair; ///< Return the whole pair
                }
            }
            cur = outer->cdr;
        }
        return nanbox::False;
    });

    env.register_builtin("assoc", 2, false, [&heap, &intern_table](Args args) -> std::expected<LispVal, RuntimeError> {
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

    /**
     * Higher-order: apply map for-each
     *
     * NOTE: (apply proc arg1 ... list) is now a VM-level special form
     * (OpCode::Apply / TailApply).  The SA intercepts calls to `apply` and
     * emits the dedicated opcode, so this primitive is only reachable when
     * `apply` is used as a first-class value (e.g. passed to another function).
     *
     * When a VM pointer is provided both map and for-each use call_value() to
     * invoke any callable (primitive or closure).  Without a VM pointer they
     * fall back to primitive-only mode.
     */

    env.register_builtin("apply", 2, true, [](Args /*args*/) -> std::expected<LispVal, RuntimeError> {
        return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError,
            "apply: cannot be used as a first-class value yet (use direct apply syntax instead)"}});
    });

    env.register_builtin("map", 2, false, [&heap, vm](Args args) -> std::expected<LispVal, RuntimeError> {
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
                        "map: closures require a VM context (internal error  -  VM not provided)"}});
                std::vector<LispVal> call_args = {cons->car};
                res = prim->func(call_args);
            }
            if (!res) return res;
            results.push_back(*res);
            cur = cons->cdr;
        }
        /// Build result list in order
        LispVal result = nanbox::Nil;
        for (auto it = results.rbegin(); it != results.rend(); ++it) {
            auto cons_val = make_cons(heap, *it, result);
            if (!cons_val) return cons_val;
            result = *cons_val;
        }
        return result;
    });

    env.register_builtin("for-each", 2, false, [&heap, vm](Args args) -> std::expected<LispVal, RuntimeError> {
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
                        "for-each: closures require a VM context (internal error  -  VM not provided)"}});
                std::vector<LispVal> call_args = {cons->car};
                res = prim->func(call_args);
            }
            if (!res) return res;
            cur = cons->cdr;
        }
        return nanbox::Nil;
    });

    /**
     * Deep equality: equal?
     */

    env.register_builtin("equal?", 2, false, [&heap, &intern_table](Args args) -> std::expected<LispVal, RuntimeError> {
        /// Recursive structural equality
        std::function<bool(LispVal, LispVal)> equal_impl = [&](LispVal a, LispVal b) -> bool {
            if (a == b) return true;
            if (!ops::is_boxed(a) || !ops::is_boxed(b)) return false;

            /// Both are strings?
            if (ops::tag(a) == Tag::String && ops::tag(b) == Tag::String) {
                auto sa = StringView::try_from(a, intern_table);
                auto sb = StringView::try_from(b, intern_table);
                if (sa && sb) return sa->view() == sb->view();
                return false;
            }

            if (ops::tag(a) != Tag::HeapObject || ops::tag(b) != Tag::HeapObject) return false;

            /// Numeric equality
            auto na = classify_numeric(a, heap);
            auto nb = classify_numeric(b, heap);
            if (na.is_valid() && nb.is_valid()) {
                if (na.is_flonum() || nb.is_flonum()) return na.as_double() == nb.as_double();
                return na.int_val == nb.int_val;
            }

            /// Cons (pair) equality
            auto* ca = heap.try_get_as<ObjectKind::Cons, types::Cons>(ops::payload(a));
            auto* cb = heap.try_get_as<ObjectKind::Cons, types::Cons>(ops::payload(b));
            if (ca && cb) {
                return equal_impl(ca->car, cb->car) && equal_impl(ca->cdr, cb->cdr);
            }

            /// Vector equality
            auto* va = heap.try_get_as<ObjectKind::Vector, types::Vector>(ops::payload(a));
            auto* vb = heap.try_get_as<ObjectKind::Vector, types::Vector>(ops::payload(b));
            if (va && vb) {
                if (va->elements.size() != vb->elements.size()) return false;
                for (size_t i = 0; i < va->elements.size(); ++i) {
                    if (!equal_impl(va->elements[i], vb->elements[i])) return false;
                }
                return true;
            }

            /// Regex equality: pattern + compile flags.
            auto* ra = heap.try_get_as<ObjectKind::Regex, types::Regex>(ops::payload(a));
            auto* rb = heap.try_get_as<ObjectKind::Regex, types::Regex>(ops::payload(b));
            if (ra && rb) {
                return ra->pattern == rb->pattern && ra->flags == rb->flags;
            }

            return false;
        };
        return equal_impl(args[0], args[1]) ? nanbox::True : nanbox::False;
    });

    /**
     * String operations: string-length string-append string-ref number->string string->number
     */

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
        return nanbox::False; ///< Scheme convention: return #f on failure
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
        /// Return the byte at position as a character (treats string as byte-indexed)
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

    /**
     * CSV primitives: %csv-*
     */
    register_csv_builtins(env, heap, intern_table);

    /**
     * Regex primitives: %regex-*
     */
    register_regex_builtins(env, heap, intern_table, vm);

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

    /**
     * Vector operations: vector vector-length vector-ref vector-set!
     */

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

    /**
     * Error signaling: error
     */

    env.register_builtin("error", 1, true, [&heap, &intern_table](Args args) -> std::expected<LispVal, RuntimeError> {
        /**
         * (error message irritant ...)
         * First arg should be a string message
         */
        std::string msg;
        auto sv = StringView::try_from(args[0], intern_table);
        if (sv) {
            msg = std::string(sv->view());
        } else {
            msg = format_value(args[0], FormatMode::Write, heap, intern_table);
        }
        /// Append irritants
        for (size_t i = 1; i < args.size(); ++i) {
            msg += " ";
            msg += format_value(args[i], FormatMode::Write, heap, intern_table);
        }
        return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::UserError, msg}});
    });

    /**
     * Platform detection: platform
     * Returns a symbol identifying the executing host OS at runtime.
     * The #if selects which string is compiled into each platform's binary;
     * the primitive is always invoked at VM execution time, so bytecode built
     * on one platform and run on another correctly reports the executing host.
     */

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

    /**
     * Logic variable type predicate: logic-var?
     */

    env.register_builtin("logic-var?", 1, false, [&heap](Args args) -> std::expected<LispVal, RuntimeError> {
        const LispVal v = args[0];
        if (!ops::is_boxed(v) || ops::tag(v) != Tag::HeapObject) return False;
        auto id = ops::payload(v);
        return heap.try_get_as<ObjectKind::LogicVar, types::LogicVar>(id) ? True : False;
    });

    env.register_builtin("register-finalizer!", 2, false, [&heap](Args args) -> std::expected<LispVal, RuntimeError> {
        const LispVal obj = args[0];
        const LispVal proc = args[1];

        if (!ops::is_boxed(obj) || ops::tag(obj) != Tag::HeapObject) {
            return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError,
                "register-finalizer!: first arg must be a heap object"}});
        }

        if (!ops::is_boxed(proc) || ops::tag(proc) != Tag::HeapObject) {
            return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError,
                "register-finalizer!: second arg must be a procedure"}});
        }
        const auto proc_id = static_cast<memory::heap::ObjectId>(ops::payload(proc));
        if (!heap.try_get_as<ObjectKind::Closure, types::Closure>(proc_id)
            && !heap.try_get_as<ObjectKind::Primitive, types::Primitive>(proc_id)) {
            return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError,
                "register-finalizer!: second arg must be a procedure"}});
        }

        const auto obj_id = static_cast<memory::heap::ObjectId>(ops::payload(obj));
        auto registered = heap.register_finalizer(obj_id, proc);
        if (!registered.has_value()) {
            if (registered.error() == memory::heap::HeapError::ObjectIdNotFound) {
                return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError,
                    "register-finalizer!: first arg must be a live heap object"}});
            }
            if (registered.error() == memory::heap::HeapError::UnexpectedObjectKind) {
                return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError,
                    "register-finalizer!: first arg must be a non-cons heap object"}});
            }
            return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError,
                "register-finalizer!: failed to register finalizer"}});
        }

        return True;
    });

    env.register_builtin("unregister-finalizer!", 1, false, [&heap](Args args) -> std::expected<LispVal, RuntimeError> {
        const LispVal obj = args[0];
        if (!ops::is_boxed(obj) || ops::tag(obj) != Tag::HeapObject) {
            return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError,
                "unregister-finalizer!: first arg must be a heap object"}});
        }

        const auto obj_id = static_cast<memory::heap::ObjectId>(ops::payload(obj));
        return heap.remove_finalizer(obj_id) ? True : False;
    });

    env.register_builtin("make-guardian", 0, false, [&heap](Args) -> std::expected<LispVal, RuntimeError> {
        return make_guardian(heap);
    });

    env.register_builtin("guardian-track!", 2, false, [&heap](Args args) -> std::expected<LispVal, RuntimeError> {
        const LispVal guardian = args[0];
        const LispVal obj = args[1];

        if (!ops::is_boxed(guardian) || ops::tag(guardian) != Tag::HeapObject) {
            return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError,
                "guardian-track!: first arg must be a guardian"}});
        }

        const auto guardian_id = static_cast<memory::heap::ObjectId>(ops::payload(guardian));
        if (!heap.try_get_as<ObjectKind::Guardian, types::Guardian>(guardian_id)) {
            return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError,
                "guardian-track!: first arg must be a guardian"}});
        }

        if (!ops::is_boxed(obj) || ops::tag(obj) != Tag::HeapObject) {
            return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError,
                "guardian-track!: second arg must be a heap object"}});
        }

        const auto obj_id = static_cast<memory::heap::ObjectId>(ops::payload(obj));
        auto tracked = heap.guardian_track(guardian_id, obj_id);
        if (!tracked.has_value()) {
            if (tracked.error() == memory::heap::HeapError::ObjectIdNotFound) {
                return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError,
                    "guardian-track!: second arg must be a live heap object"}});
            }
            if (tracked.error() == memory::heap::HeapError::UnexpectedObjectKind) {
                return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError,
                    "guardian-track!: second arg must be a non-cons heap object"}});
            }
            return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError,
                "guardian-track!: failed to track object"}});
        }

        return True;
    });

    env.register_builtin("guardian-collect", 1, false, [&heap](Args args) -> std::expected<LispVal, RuntimeError> {
        const LispVal guardian = args[0];
        if (!ops::is_boxed(guardian) || ops::tag(guardian) != Tag::HeapObject) {
            return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError,
                "guardian-collect: arg must be a guardian"}});
        }

        const auto guardian_id = static_cast<memory::heap::ObjectId>(ops::payload(guardian));
        if (!heap.try_get_as<ObjectKind::Guardian, types::Guardian>(guardian_id)) {
            return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError,
                "guardian-collect: arg must be a guardian"}});
        }

        auto next = heap.dequeue_guardian_ready(guardian_id);
        return next.has_value() ? *next : False;
    });

    /**
     * Attributed variables
     *                               at least one attribute
     * (register-attr-hook! 'module proc)
     *                               (proc var bound-value attr-value)
     *                               when `var` with attribute 'module is
     *                               bound by unify.  Returns #f on failure
     *                               (which unifies fails).  Hook registry
     *                               is VM-lifetime and NOT trailed.
     */

    /// Helper: extract the InternId from a symbol LispVal, or nullopt.
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
            /// Trail the prior state so backtracking undoes the write.
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
            /// Trail so backtracking re-installs the attribute.
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
            /// An attributed variable must be unbound and have at least one attr.
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

    /**
     * logic-var/named : create a fresh unbound LogicVar with a debug name
     *
     * `(var-name v)` introspection, tracing, and future error messages.
     */
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

    /**
     * var-name : return the debug name of a LogicVar, or #f if none / not a var
     */
    env.register_builtin("var-name", 1, false,
        [&heap, &intern_table](Args args) -> std::expected<LispVal, RuntimeError> {
            const LispVal v = args[0];
            if (!ops::is_boxed(v) || ops::tag(v) != Tag::HeapObject) return False;
            auto* lv = heap.try_get_as<ObjectKind::LogicVar, types::LogicVar>(ops::payload(v));
            if (!lv || lv->name.empty()) return False;
            return make_string(heap, intern_table, lv->name);
        });

    /**
     * Occurs-check policy
     *
     * (set-occurs-check! 'always)  ; run occurs-check, fail on cycle (default)
     * (set-occurs-check! 'never)   ; skip occurs-check (ISO-Prolog default; faster)
     * (set-occurs-check! 'error)   ; run occurs-check, raise error on cycle
     */
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

    /**
     * Ground check: ground?
     * Returns #t iff the term contains no unbound logic variables.
     * Recurses into Cons pairs and Vectors; treats all other heap objects
     */

    env.register_builtin("ground?", 1, false, [&heap](Args args) -> std::expected<LispVal, RuntimeError> {
        std::function<bool(LispVal)> is_ground = [&](LispVal v) -> bool {
            LispVal curr = v;
            for (;;) {
                if (!ops::is_boxed(curr) || ops::tag(curr) != Tag::HeapObject)
                    return true;
                auto id = ops::payload(curr);
                if (auto* lv = heap.try_get_as<ObjectKind::LogicVar, types::LogicVar>(id)) {
                    if (!lv->binding.has_value()) return false;  ///< unbound
                    curr = *lv->binding;                          ///< follow chain
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
                    return true;  ///< string, closure, port, etc.
                }
            }
        };
        return is_ground(args[0]) ? True : False;
    });

    /**
     * Compound terms: term / functor / arity / arg / compound?
     *
     * A `CompoundTerm` is a structured logic term with a symbol functor and
     * Unifies structurally with other compound terms of the same functor+arity.
     * See docs/logic.md and docs/logic-next-steps.md for the rationale.
     */

    env.register_builtin("compound?", 1, false,
        [&heap](Args args) -> std::expected<LispVal, RuntimeError> {
            const LispVal v = args[0];
            if (!ops::is_boxed(v) || ops::tag(v) != Tag::HeapObject) return False;
            return heap.try_get_as<ObjectKind::CompoundTerm, types::CompoundTerm>(ops::payload(v))
                ? True : False;
        });

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

    env.register_builtin("functor", 1, false,
        [&heap](Args args) -> std::expected<LispVal, RuntimeError> {
            const LispVal v = args[0];
            if (!ops::is_boxed(v) || ops::tag(v) != Tag::HeapObject) return False;
            auto* ct = heap.try_get_as<ObjectKind::CompoundTerm, types::CompoundTerm>(ops::payload(v));
            if (!ct) return False;
            return ct->functor;
        });

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

    /**
     *
     * These builtins were removed along with the Dual heap type.
     * They are retained as error stubs so that the global builtin slot indices
     * remain stable (existing compiled bytecode references slots by index).
     */

    env.register_builtin("dual?", 1, false, [](Args) -> std::expected<LispVal, RuntimeError> {
        return False;  ///< Nothing is a Dual any more
    });

    env.register_builtin("dual-primal", 1, false, [](Args args) -> std::expected<LispVal, RuntimeError> {
        return args[0];  ///< pass-through
    });

    env.register_builtin("dual-backprop", 1, false, [&heap](Args) -> std::expected<LispVal, RuntimeError> {
        /// Return a no-op backpropagator
        return make_primitive(heap,
            [](Args) -> std::expected<LispVal, RuntimeError> { return Nil; },
            1, false);
    });

    env.register_builtin("make-dual", 2, false, [](Args) -> std::expected<LispVal, RuntimeError> {
        return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError,
            "make-dual: Dual AD has been removed  -  use tape-based AD instead"}});
    });

    /**
     * CLP domain primitives: %clp-domain-z!  %clp-domain-fd!  %clp-get-domain
     * plus test primitives (%clp-linearize, %clp-fm-*).
     *
     * These are internal builtins consumed by std.clp.  They are prefixed with
     * % to signal that user code should call the std.clp wrapper instead.
     *
     * Domain check at unification time is handled inside VM::unify() using
     * the constraint_store_ field; these builtins only manage the store.
     */

    /**
     * (%clp-domain-z! var lo hi)
     * Constrain `var` (unbound logic variable) to the integer interval [lo, hi].
     * Adds the domain to the constraint store (trailed for backtracking).
     */
    env.register_builtin("%clp-domain-z!", 3, false,
        [&heap, &intern_table, vm](Args args) -> std::expected<LispVal, RuntimeError> {
            if (!vm) return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError,
                "%clp-domain-z!: requires a running VM"}});
            /// Resolve the variable through any binding chain
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

    /**
     * (%clp-domain-fd! var values-list)
     * Constrain `var` to the finite set of integers given as an Eta proper list.
     */
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
            std::vector<int64_t> raw;
            LispVal lst = args[1];
            while (ops::is_boxed(lst) && ops::tag(lst) == Tag::HeapObject) {
                auto* c = heap.try_get_as<ObjectKind::Cons, types::Cons>(ops::payload(lst));
                if (!c) break;
                auto n = classify_numeric(c->car, heap);
                if (!n.is_valid() || n.is_flonum())
                    return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError,
                        "%clp-domain-fd!: domain values must be integers"}});
                raw.push_back(n.int_val);
                lst = c->cdr;
            }
            clp::FDDomain dom = clp::FDDomain::from_unsorted(std::move(raw));
            if (dom.empty())
                return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::UserError,
                    "%clp-domain-fd!: domain list is empty"}});
            vm->trail_set_domain(id, std::move(dom));
            return True;
        });

    /**
     * (%clp-domain-r! var lo hi lo-open? hi-open?)
     * Attach a real-valued interval domain to `var`.  Bounds
     * are doubles (fixnum or flonum accepted, both promoted via
     * classify_numeric).  The open/closed flags are #t / #f booleans.
     * Empty intervals (lo > hi, or lo == hi with any open flag) are
     * rejected at post time as a UserError.
     */
    env.register_builtin("%clp-domain-r!", 5, false,
        [&heap, vm](Args args) -> std::expected<LispVal, RuntimeError> {
            if (!vm) return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError,
                "%clp-domain-r!: requires a running VM"}});
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
                    "%clp-domain-r!: first argument must be a logic variable"}});
            auto id = ops::payload(var);
            if (!heap.try_get_as<ObjectKind::LogicVar, types::LogicVar>(id))
                return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError,
                    "%clp-domain-r!: first argument must be an unbound logic variable"}});
            auto nlo = classify_numeric(args[1], heap);
            auto nhi = classify_numeric(args[2], heap);
            if (!nlo.is_valid() || !nhi.is_valid())
                return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError,
                    "%clp-domain-r!: lo and hi must be numbers"}});
            /// Booleans: accept #t / #f exactly.  Anything else is a type error.
            auto bool_arg = [](LispVal v) -> std::optional<bool> {
                if (v == True)  return true;
                if (v == False) return false;
                return std::nullopt;
            };
            auto blo = bool_arg(args[3]);
            auto bhi = bool_arg(args[4]);
            if (!blo || !bhi)
                return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError,
                    "%clp-domain-r!: open-flag arguments must be #t or #f"}});
            clp::RDomain dom{ nlo.as_double(), nhi.as_double(), *blo, *bhi };
            if (dom.empty())
                return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::UserError,
                    "%clp-domain-r!: empty domain"}});

            // Mirror the interval into RealStore::simplex_bounds so the LP/QP
            // backends see the box constraint. Without this, callers must post
            // an additional clp:r>= / clp:r<= pair for the optimizer to
            // respect the declared domain — a footgun the original two-store
            // design accidentally exposed (see docs/clp.md "Bound stores").
            //
            // Strict (open) flags map to Bound::strict=true; LP/QP shave the
            // value by kRealSimplexEps when reading a strict bound (see
            // build_qp_model and optimize_real_objective in this file).
            //
            // If a tighter bound already exists for this var (from an earlier
            // clp:r<= / clp:r>= or another clp:real), keep the tighter side.
            std::optional<clp::Bound> new_lo;
            if (std::isfinite(dom.lo)) {
                new_lo = clp::Bound{ .value = dom.lo, .strict = dom.lo_open };
            }
            std::optional<clp::Bound> new_hi;
            if (std::isfinite(dom.hi)) {
                new_hi = clp::Bound{ .value = dom.hi, .strict = dom.hi_open };
            }
            if (const auto* prev = vm->real_store().simplex_bounds(id)) {
                auto tighter_lower = [](const clp::Bound& a, const clp::Bound& b) {
                    if (a.value > b.value) return a;
                    if (a.value < b.value) return b;
                    return clp::Bound{ .value = a.value, .strict = a.strict || b.strict };
                };
                auto tighter_upper = [](const clp::Bound& a, const clp::Bound& b) {
                    if (a.value < b.value) return a;
                    if (a.value > b.value) return b;
                    return clp::Bound{ .value = a.value, .strict = a.strict || b.strict };
                };
                if (prev->lo.has_value()) {
                    new_lo = new_lo.has_value()
                        ? std::optional<clp::Bound>{ tighter_lower(*new_lo, *prev->lo) }
                        : prev->lo;
                }
                if (prev->hi.has_value()) {
                    new_hi = new_hi.has_value()
                        ? std::optional<clp::Bound>{ tighter_upper(*new_hi, *prev->hi) }
                        : prev->hi;
                }
            }

            vm->trail_set_domain(id, std::move(dom));
            vm->trail_assert_simplex_bound(id, std::move(new_lo), std::move(new_hi));
            return True;
        });

    /**
     * (%clp-get-domain var)
     * Returns the domain of `var` as an Eta value:
     */
    env.register_builtin("%clp-get-domain", 1, false,
        [&heap, &intern_table, vm](Args args) -> std::expected<LispVal, RuntimeError> {
            if (!vm) return False;
            /// Deref the variable
            LispVal var = args[0];
            for (;;) {
                if (!ops::is_boxed(var) || ops::tag(var) != Tag::HeapObject) return False;
                auto id2 = ops::payload(var);
                auto* lv2 = heap.try_get_as<ObjectKind::LogicVar, types::LogicVar>(id2);
                if (!lv2) return False;           ///< not a logic variable
                if (!lv2->binding.has_value()) break; ///< found unbound variable
                var = *lv2->binding;
            }
            auto id = ops::payload(var);
            const clp::Domain* dom = vm->constraint_store().get_domain(id);
            if (!dom) return False;

            using namespace memory::factory;
            if (const auto* z = std::get_if<clp::ZDomain>(dom)) {
                /// Build (z lo hi)
                auto sym = make_symbol(intern_table, "z");
                auto lo  = make_fixnum(heap, z->lo);
                auto hi  = make_fixnum(heap, z->hi);
                if (!sym || !lo || !hi) return False;
                auto hi_c  = make_cons(heap, *hi,  Nil);
                auto lo_c  = make_cons(heap, *lo,  hi_c ? *hi_c  : Nil);
                auto result= make_cons(heap, *sym, lo_c ? *lo_c  : Nil);
                if (!hi_c || !lo_c || !result) return False;
                return *result;
            } else if (const auto* r = std::get_if<clp::RDomain>(dom)) {
                /**
                 * `hi` are flonums; the open-flag pair is appended so the
                 * shape is unambiguous from the (z lo hi) / (fd vs) cases.
                 */
                auto sym   = make_symbol(intern_table, "r");
                auto lo_e  = make_flonum(r->lo);
                auto hi_e  = make_flonum(r->hi);
                if (!sym || !lo_e || !hi_e) return False;
                LispVal lo_open = r->lo_open ? True : False;
                LispVal hi_open = r->hi_open ? True : False;
                auto c4 = make_cons(heap, hi_open, Nil);
                auto c3 = make_cons(heap, lo_open, c4 ? *c4 : Nil);
                auto c2 = make_cons(heap, *hi_e,   c3 ? *c3 : Nil);
                auto c1 = make_cons(heap, *lo_e,   c2 ? *c2 : Nil);
                auto rs = make_cons(heap, *sym,    c1 ? *c1 : Nil);
                if (!c4 || !c3 || !c2 || !c1 || !rs) return False;
                return *rs;
            } else {
                /// FD: build (fd v1 v2 ...)
                const auto& fd = std::get<clp::FDDomain>(*dom);
                auto sym = make_symbol(intern_table, "fd");
                if (!sym) return False;
                LispVal lst = Nil;
                const auto vs = fd.to_vector();   ///< ascending
                for (int i = static_cast<int>(vs.size()) - 1; i >= 0; --i) {
                    auto v = make_fixnum(heap, vs[static_cast<std::size_t>(i)]);
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

    /**
     * (%clp-linearize term)
     * Test-only primitive.  Returns a dotted pair:
     *
     *   (pairs . constant)
     *
     * where `pairs` is a proper list of `(coef . var-id)` pairs in
     * canonical var-id order.
     */
    env.register_builtin("%clp-linearize", 1, false,
        [&heap, &intern_table](Args args) -> std::expected<LispVal, RuntimeError> {
            auto linear = clp::linearize(args[0], heap, intern_table);
            if (!linear) {
                std::ostringstream oss;
                oss << linear.error().tag << ": " << linear.error().message;
                if (!linear.error().offending_vars.empty()) {
                    oss << " [vars:";
                    for (std::size_t i = 0; i < linear.error().offending_vars.size(); ++i) {
                        if (i > 0) oss << ",";
                        oss << linear.error().offending_vars[i];
                    }
                    oss << "]";
                }
                return std::unexpected(RuntimeError{
                    VMError{RuntimeErrorCode::UserError, oss.str()}});
            }

            auto roots = heap.make_external_root_frame();
            LispVal pairs = Nil;
            roots.push(pairs);

            for (auto it = linear->terms.rbegin(); it != linear->terms.rend(); ++it) {
                auto coef_val = make_flonum(it->coef);
                if (!coef_val) return std::unexpected(coef_val.error());
                auto var_val = make_fixnum(heap, static_cast<int64_t>(it->var_id));
                if (!var_val) return std::unexpected(var_val.error());
                roots.push(*coef_val);
                roots.push(*var_val);

                auto pair_val = make_cons(heap, *coef_val, *var_val);
                if (!pair_val) return std::unexpected(pair_val.error());
                roots.push(*pair_val);

                auto cell = make_cons(heap, *pair_val, pairs);
                if (!cell) return std::unexpected(cell.error());
                pairs = *cell;
                roots.push(pairs);
            }

            auto constant = make_flonum(linear->constant);
            if (!constant) return std::unexpected(constant.error());
            roots.push(*constant);

            return make_cons(heap, pairs, *constant);
        });

    /**
     * Test-only Fourier-Motzkin primitives:
     *
     *   (%clp-fm-feasible? constraints [row-cap])
     *   (%clp-fm-bounds var constraints [row-cap])
     *
     * `constraints` is a proper list of relation terms:
     *   (<= lhs rhs), (>= lhs rhs), (= lhs rhs)
     */
    {
        struct ParsedRelation {
            std::string op;
            LispVal lhs{Nil};
            LispVal rhs{Nil};
        };

        auto fm_user_error = [](std::string msg) -> RuntimeError {
            return RuntimeError{VMError{RuntimeErrorCode::UserError, std::move(msg)}};
        };

        auto symbol_text = [&intern_table](LispVal v) -> std::optional<std::string> {
            if (!ops::is_boxed(v) || ops::tag(v) != Tag::Symbol) return std::nullopt;
            auto s = intern_table.get_string(ops::payload(v));
            if (!s) return std::nullopt;
            return std::string(*s);
        };

        auto parse_relation = [&heap, symbol_text, fm_user_error](LispVal term)
            -> std::expected<ParsedRelation, RuntimeError> {
            if (!ops::is_boxed(term) || ops::tag(term) != Tag::HeapObject) {
                return std::unexpected(fm_user_error(
                    "clp.fm.parse.relation: each constraint must be a relation term"));
            }

            const auto id = ops::payload(term);
            if (auto* ct = heap.try_get_as<ObjectKind::CompoundTerm, types::CompoundTerm>(id)) {
                if (ct->args.size() != 2) {
                    return std::unexpected(fm_user_error(
                        "clp.fm.parse.relation: relation term must have exactly 2 arguments"));
                }
                auto op = symbol_text(ct->functor);
                if (!op) {
                    return std::unexpected(fm_user_error(
                        "clp.fm.parse.relation: relation operator must be a symbol"));
                }
                return ParsedRelation{
                    .op = std::move(*op),
                    .lhs = ct->args[0],
                    .rhs = ct->args[1],
                };
            }

            auto* rel_cell = heap.try_get_as<ObjectKind::Cons, types::Cons>(id);
            if (!rel_cell) {
                return std::unexpected(fm_user_error(
                    "clp.fm.parse.relation: each constraint must be a relation term"));
            }
            auto op = symbol_text(rel_cell->car);
            if (!op) {
                return std::unexpected(fm_user_error(
                    "clp.fm.parse.relation: relation operator must be a symbol"));
            }

            std::vector<LispVal> rel_args;
            LispVal cursor = rel_cell->cdr;
            while (ops::is_boxed(cursor) && ops::tag(cursor) == Tag::HeapObject) {
                auto* c = heap.try_get_as<ObjectKind::Cons, types::Cons>(ops::payload(cursor));
                if (!c) break;
                rel_args.push_back(c->car);
                cursor = c->cdr;
            }
            if (cursor != Nil || rel_args.size() != 2) {
                return std::unexpected(fm_user_error(
                    "clp.fm.parse.relation: relation term must have exactly 2 arguments"));
            }

            return ParsedRelation{
                .op = std::move(*op),
                .lhs = rel_args[0],
                .rhs = rel_args[1],
            };
        };

        auto format_linearize_error = [](const clp::LinearizeErrorInfo& err) -> std::string {
            std::string suffix = err.tag;
            const std::string prefix = "clp.linearize.";
            if (suffix.rfind(prefix, 0) == 0) {
                suffix = suffix.substr(prefix.size());
            }
            std::ostringstream oss;
            oss << "clp.fm.linearize." << suffix << ": " << err.message;
            if (!err.offending_vars.empty()) {
                oss << " [vars:";
                for (std::size_t i = 0; i < err.offending_vars.size(); ++i) {
                    if (i > 0) oss << ",";
                    oss << err.offending_vars[i];
                }
                oss << "]";
            }
            return oss.str();
        };

        auto linear_diff = [&heap, &intern_table, fm_user_error, format_linearize_error]
            (LispVal lhs, LispVal rhs) -> std::expected<clp::LinearExpr, RuntimeError> {
            auto l = clp::linearize(lhs, heap, intern_table);
            if (!l) {
                return std::unexpected(fm_user_error(format_linearize_error(l.error())));
            }
            auto r = clp::linearize(rhs, heap, intern_table);
            if (!r) {
                return std::unexpected(fm_user_error(format_linearize_error(r.error())));
            }

            clp::LinearExpr out;
            out.constant = l->constant - r->constant;
            out.terms = l->terms;
            out.terms.reserve(l->terms.size() + r->terms.size());
            for (const auto& t : r->terms) {
                out.terms.push_back(clp::LinearTerm{
                    .var_id = t.var_id,
                    .coef = -t.coef,
                });
            }
            out.canonicalize();
            return out;
        };

        auto parse_constraints = [&heap, parse_relation, linear_diff, fm_user_error](LispVal raw_constraints)
            -> std::expected<clp::FMSystem, RuntimeError> {
            clp::FMSystem sys;
            LispVal cursor = raw_constraints;
            while (ops::is_boxed(cursor) && ops::tag(cursor) == Tag::HeapObject) {
                auto* c = heap.try_get_as<ObjectKind::Cons, types::Cons>(ops::payload(cursor));
                if (!c) break;

                auto rel = parse_relation(c->car);
                if (!rel) return std::unexpected(rel.error());

                auto diff = linear_diff(rel->lhs, rel->rhs);
                if (!diff) return std::unexpected(diff.error());

                if (rel->op == "<=") {
                    sys.leq.push_back(*diff);
                } else if (rel->op == ">=") {
                    clp::LinearExpr flipped = *diff;
                    flipped.constant = -flipped.constant;
                    for (auto& t : flipped.terms) t.coef = -t.coef;
                    sys.leq.push_back(std::move(flipped));
                } else if (rel->op == "=") {
                    sys.eq.push_back(*diff);
                } else {
                    return std::unexpected(fm_user_error(
                        "clp.fm.parse.relation-op: relation operator must be one of <=, >=, ="));
                }

                cursor = c->cdr;
            }
            if (cursor != Nil) {
                return std::unexpected(fm_user_error(
                    "clp.fm.parse.constraints: constraints must be a proper list"));
            }
            return sys;
        };

        auto parse_row_cap = [&heap, fm_user_error](LispVal arg)
            -> std::expected<std::size_t, RuntimeError> {
            auto n = classify_numeric(arg, heap);
            if (!n.is_valid() || n.is_flonum()) {
                return std::unexpected(fm_user_error(
                    "clp.fm.parse.row-cap: row-cap must be an integer"));
            }
            if (n.int_val <= 0) {
                return std::unexpected(fm_user_error(
                    "clp.fm.parse.row-cap: row-cap must be > 0"));
            }
            if (static_cast<unsigned long long>(n.int_val) >
                static_cast<unsigned long long>(std::numeric_limits<std::size_t>::max())) {
                return std::unexpected(fm_user_error(
                    "clp.fm.parse.row-cap: row-cap is too large"));
            }
            return static_cast<std::size_t>(n.int_val);
        };

        auto cap_symbol = [&intern_table]() -> std::expected<LispVal, RuntimeError> {
            return make_symbol(intern_table, "clp.fm.cap-exceeded");
        };

        auto deref_unbound_lvar = [&heap, fm_user_error](LispVal v)
            -> std::expected<ObjectId, RuntimeError> {
            LispVal cur = v;
            for (;;) {
                if (!ops::is_boxed(cur) || ops::tag(cur) != Tag::HeapObject) {
                    return std::unexpected(fm_user_error(
                        "clp.fm.parse.var: first argument must be an unbound logic variable"));
                }
                auto id = ops::payload(cur);
                auto* lv = heap.try_get_as<ObjectKind::LogicVar, types::LogicVar>(id);
                if (!lv) {
                    return std::unexpected(fm_user_error(
                        "clp.fm.parse.var: first argument must be an unbound logic variable"));
                }
                if (!lv->binding.has_value()) return id;
                cur = *lv->binding;
            }
        };

        env.register_builtin("%clp-fm-feasible?", 1, true,
            [parse_constraints, parse_row_cap, cap_symbol, fm_user_error](Args args)
                -> std::expected<LispVal, RuntimeError> {
                if (args.size() > 2) {
                    return std::unexpected(fm_user_error(
                        "clp.fm.parse.arity: %clp-fm-feasible? expects 1 or 2 arguments"));
                }
                auto sys = parse_constraints(args[0]);
                if (!sys) return std::unexpected(sys.error());

                std::size_t row_cap = 4096;
                if (args.size() == 2) {
                    auto cap = parse_row_cap(args[1]);
                    if (!cap) return std::unexpected(cap.error());
                    row_cap = *cap;
                }

                const auto result = clp::fm_feasible(*sys, clp::FMConfig{
                    .row_cap = row_cap,
                    .eps = 1e-12,
                });
                switch (result.status) {
                    case clp::FMStatus::Feasible:
                        return True;
                    case clp::FMStatus::Infeasible:
                        return False;
                    case clp::FMStatus::CapExceeded:
                        return cap_symbol();
                }
                return False;
            });

        env.register_builtin("%clp-fm-bounds", 2, true,
            [&heap, deref_unbound_lvar, parse_constraints, parse_row_cap, cap_symbol, fm_user_error](Args args)
                -> std::expected<LispVal, RuntimeError> {
                if (args.size() > 3) {
                    return std::unexpected(fm_user_error(
                        "clp.fm.parse.arity: %clp-fm-bounds expects 2 or 3 arguments"));
                }

                auto var_id = deref_unbound_lvar(args[0]);
                if (!var_id) return std::unexpected(var_id.error());

                auto sys = parse_constraints(args[1]);
                if (!sys) return std::unexpected(sys.error());

                std::size_t row_cap = 4096;
                if (args.size() == 3) {
                    auto cap = parse_row_cap(args[2]);
                    if (!cap) return std::unexpected(cap.error());
                    row_cap = *cap;
                }

                const auto result = clp::fm_bounds_for(*sys, *var_id, clp::FMConfig{
                    .row_cap = row_cap,
                    .eps = 1e-12,
                });
                switch (result.status) {
                    case clp::FMStatus::Feasible: {
                        if (!result.bounds.has_value()) {
                            return std::unexpected(fm_user_error(
                                "clp.fm.internal: feasible result missing bounds"));
                        }
                        auto lo = make_flonum(result.bounds->lo);
                        if (!lo) return std::unexpected(lo.error());
                        auto hi = make_flonum(result.bounds->hi);
                        if (!hi) return std::unexpected(hi.error());
                        return make_cons(heap, *lo, *hi);
                    }
                    case clp::FMStatus::Infeasible:
                        return False;
                    case clp::FMStatus::CapExceeded:
                        return cap_symbol();
                }
                return False;
            });
    }

    /**
     * CLP(R) posting primitives:
     *
     *   (%clp-r-post-leq! lhs rhs)
     *   (%clp-r-post-eq!  lhs rhs)
     *   (%clp-r-propagate!)
     *   (%clp-r-minimize objective)
     *   (%clp-r-maximize objective)
     *
     * Posting appends one relation row to the per-VM RealStore, checks
     * simplex feasibility, then tightens R bounds for every participating
     * variable. On failure, all effects since the local trail snapshot are
     * rolled back atomically (including the RealStore append).
     *
     * Optimization returns:
     *   - `#f` on infeasible objective,
     *   - symbol `clp.r.unbounded` on unbounded objective,
     *   - `(opt . witness)` on optimum where `witness` is
     *     `((var . value) ...)`.
     */
    {
        auto r_user_error = [](std::string msg) -> RuntimeError {
            return RuntimeError{VMError{RuntimeErrorCode::UserError, std::move(msg)}};
        };

        auto format_linearize_error = [](const clp::LinearizeErrorInfo& err) -> std::string {
            std::string suffix = err.tag;
            const std::string prefix = "clp.linearize.";
            if (suffix.rfind(prefix, 0) == 0) {
                suffix = suffix.substr(prefix.size());
            }
            std::ostringstream oss;
            oss << "clp.r.linearize." << suffix << ": " << err.message;
            if (!err.offending_vars.empty()) {
                oss << " [vars:";
                for (std::size_t i = 0; i < err.offending_vars.size(); ++i) {
                    if (i > 0) oss << ",";
                    oss << err.offending_vars[i];
                }
                oss << "]";
            }
            return oss.str();
        };

        auto format_quadratic_linearize_error =
            [](const clp::QuadraticLinearizeErrorInfo& err) -> std::string {
            std::string suffix = err.tag;
            const std::string prefix = "clp.qp.linearize.";
            if (suffix.rfind(prefix, 0) == 0) {
                suffix = suffix.substr(prefix.size());
            }
            std::ostringstream oss;
            oss << "clp.r.qp.linearize." << suffix << ": " << err.message;
            if (!err.offending_vars.empty()) {
                oss << " [vars:";
                for (std::size_t i = 0; i < err.offending_vars.size(); ++i) {
                    if (i > 0) oss << ",";
                    oss << err.offending_vars[i];
                }
                oss << "]";
            }
            return oss.str();
        };

        auto format_quadratic_model_error =
            [](const clp::QuadraticModelErrorInfo& err) -> std::string {
            std::string suffix = err.tag;
            const std::string prefix = "clp.qp.";
            if (suffix.rfind(prefix, 0) == 0) {
                suffix = suffix.substr(prefix.size());
            }
            std::ostringstream oss;
            oss << "clp.r.qp." << suffix << ": " << err.message;
            if (!err.offending_vars.empty()) {
                oss << " [vars:";
                for (std::size_t i = 0; i < err.offending_vars.size(); ++i) {
                    if (i > 0) oss << ",";
                    oss << err.offending_vars[i];
                }
                oss << "]";
            }
            return oss.str();
        };

        auto format_qp_solve_error =
            [](const clp::QPSolveErrorInfo& err) -> std::string {
            std::string suffix = err.tag;
            const std::string prefix = "clp.qp.";
            if (suffix.rfind(prefix, 0) == 0) {
                suffix = suffix.substr(prefix.size());
            }
            std::ostringstream oss;
            oss << "clp.r.qp." << suffix << ": " << err.message;
            if (!err.offending_vars.empty()) {
                oss << " [vars:";
                for (std::size_t i = 0; i < err.offending_vars.size(); ++i) {
                    if (i > 0) oss << ",";
                    oss << err.offending_vars[i];
                }
                oss << "]";
            }
            return oss.str();
        };

        auto linear_diff = [&heap, &intern_table, r_user_error, format_linearize_error]
            (LispVal lhs, LispVal rhs) -> std::expected<clp::LinearExpr, RuntimeError> {
            auto l = clp::linearize(lhs, heap, intern_table);
            if (!l) return std::unexpected(r_user_error(format_linearize_error(l.error())));
            auto r = clp::linearize(rhs, heap, intern_table);
            if (!r) return std::unexpected(r_user_error(format_linearize_error(r.error())));

            clp::LinearExpr out;
            out.constant = l->constant - r->constant;
            out.terms = l->terms;
            out.terms.reserve(l->terms.size() + r->terms.size());
            for (const auto& t : r->terms) {
                out.terms.push_back(clp::LinearTerm{
                    .var_id = t.var_id,
                    .coef = -t.coef,
                });
            }
            out.canonicalize();
            return out;
        };

        constexpr double kRealSimplexEps = 1e-9;
#ifdef ETA_CLP_FM_ORACLE
        constexpr clp::FMConfig kRealOracleCfg{
            .row_cap = 4096,
            .eps = 1e-12,
        };
#endif

        auto same_rdomain = [](const clp::RDomain& a, const clp::RDomain& b) -> bool {
            return a.lo == b.lo && a.hi == b.hi &&
                   a.lo_open == b.lo_open && a.hi_open == b.hi_open;
        };

        auto is_unbounded = [](const clp::RDomain& b) -> bool {
            return std::isinf(b.lo) && b.lo < 0.0 &&
                   std::isinf(b.hi) && b.hi > 0.0;
        };

        auto mixed_domain_error = [r_user_error](ObjectId id) -> RuntimeError {
            std::ostringstream oss;
            oss << "clp.r.fd-mixing-not-supported: variable " << id
                << " has a non-real CLP domain";
            return r_user_error(oss.str());
        };

        auto non_numeric_binding_error = [r_user_error](ObjectId id) -> RuntimeError {
            std::ostringstream oss;
            oss << "clp.r.non-numeric-binding: variable " << id
                << " is bound to a non-numeric value";
            return r_user_error(oss.str());
        };

        auto deref_real_var =
            [&heap, r_user_error, non_numeric_binding_error](ObjectId id)
            -> std::expected<std::variant<ObjectId, double>, RuntimeError> {
            constexpr std::size_t kMaxDerefDepth = 1024;
            LispVal cur = ops::box(Tag::HeapObject, static_cast<int64_t>(id));
            for (std::size_t depth = 0; depth < kMaxDerefDepth; ++depth) {
                if (ops::is_boxed(cur) && ops::tag(cur) == Tag::HeapObject) {
                    const auto cid = ops::payload(cur);
                    auto* lv = heap.try_get_as<ObjectKind::LogicVar, types::LogicVar>(cid);
                    if (lv) {
                        if (!lv->binding.has_value()) return cid;
                        cur = *lv->binding;
                        continue;
                    }
                }
                auto n = classify_numeric(cur, heap);
                if (!n.is_valid()) {
                    return std::unexpected(non_numeric_binding_error(id));
                }
                return n.as_double();
            }
            return std::unexpected(r_user_error(
                "clp.r.deref-depth-exceeded: logic variable dereference depth exceeded"));
        };

        auto materialize_system =
            [vm, deref_real_var]()
            -> std::expected<std::pair<clp::FMSystem, std::vector<ObjectId>>, RuntimeError> {
            if (!vm) {
                return std::unexpected(RuntimeError{VMError{
                    RuntimeErrorCode::TypeError,
                    "clp.r.internal: requires a running VM"}});
            }

            clp::FMSystem sys;
            std::vector<ObjectId> vars;
            const auto& entries = vm->real_store().entries();
            sys.leq.reserve(entries.size());
            sys.eq.reserve(entries.size());

            for (const auto& entry : entries) {
                clp::LinearExpr row;
                row.constant = entry.expr.constant;
                row.terms.reserve(entry.expr.terms.size());

                for (const auto& t : entry.expr.terms) {
                    auto resolved = deref_real_var(t.var_id);
                    if (!resolved) return std::unexpected(resolved.error());
                    if (std::holds_alternative<double>(*resolved)) {
                        row.constant += t.coef * std::get<double>(*resolved);
                    } else {
                        const auto vid = std::get<ObjectId>(*resolved);
                        row.terms.push_back(clp::LinearTerm{
                            .var_id = vid,
                            .coef = t.coef,
                        });
                        vars.push_back(vid);
                    }
                }

                row.canonicalize();
                if (entry.relation == clp::RealRelation::Leq) {
                    sys.leq.push_back(std::move(row));
                } else {
                    sys.eq.push_back(std::move(row));
                }
            }

            std::sort(vars.begin(), vars.end());
            vars.erase(std::unique(vars.begin(), vars.end()), vars.end());
            return std::make_pair(std::move(sys), std::move(vars));
        };

        auto materialize_quadratic_expr =
            [deref_real_var](clp::QuadraticExpr expr)
            -> std::expected<clp::QuadraticExpr, RuntimeError> {
            clp::QuadraticExpr out;
            out.constant = expr.constant;
            out.linear_terms.reserve(expr.linear_terms.size() + expr.quadratic_terms.size());
            out.quadratic_terms.reserve(expr.quadratic_terms.size());

            for (const auto& t : expr.linear_terms) {
                auto resolved = deref_real_var(t.var_id);
                if (!resolved) return std::unexpected(resolved.error());
                if (std::holds_alternative<double>(*resolved)) {
                    out.constant += t.coef * std::get<double>(*resolved);
                } else {
                    out.linear_terms.push_back(clp::LinearTerm{
                        .var_id = std::get<ObjectId>(*resolved),
                        .coef = t.coef,
                    });
                }
            }

            for (const auto& t : expr.quadratic_terms) {
                auto lhs = deref_real_var(t.var_i);
                if (!lhs) return std::unexpected(lhs.error());
                auto rhs = deref_real_var(t.var_j);
                if (!rhs) return std::unexpected(rhs.error());

                const bool lhs_num = std::holds_alternative<double>(*lhs);
                const bool rhs_num = std::holds_alternative<double>(*rhs);
                if (lhs_num && rhs_num) {
                    out.constant += t.coef * std::get<double>(*lhs) * std::get<double>(*rhs);
                } else if (lhs_num || rhs_num) {
                    const double k = lhs_num ? std::get<double>(*lhs) : std::get<double>(*rhs);
                    const ObjectId var_id = lhs_num ? std::get<ObjectId>(*rhs) : std::get<ObjectId>(*lhs);
                    out.linear_terms.push_back(clp::LinearTerm{
                        .var_id = var_id,
                        .coef = t.coef * k,
                    });
                } else {
                    out.quadratic_terms.push_back(clp::QuadraticTerm{
                        .var_i = std::get<ObjectId>(*lhs),
                        .var_j = std::get<ObjectId>(*rhs),
                        .coef = t.coef,
                    });
                }
            }

            out.canonicalize();
            return out;
        };

        auto ensure_real_domains =
            [vm, mixed_domain_error](const std::vector<ObjectId>& vars)
            -> std::expected<void, RuntimeError> {
            if (!vm) {
                return std::unexpected(RuntimeError{VMError{
                    RuntimeErrorCode::TypeError,
                    "clp.r.internal: requires a running VM"}});
            }
            for (auto id : vars) {
                const auto* dom = vm->constraint_store().get_domain(id);
                if (dom && !std::holds_alternative<clp::RDomain>(*dom)) {
                    return std::unexpected(mixed_domain_error(id));
                }
            }
            return {};
        };

        auto tighten_real_bounds =
            [vm, same_rdomain, is_unbounded, mixed_domain_error, r_user_error,
             materialize_system, ensure_real_domains]()
            -> std::expected<bool, RuntimeError> {
            if (!vm) {
                return std::unexpected(RuntimeError{VMError{
                    RuntimeErrorCode::TypeError,
                    "clp.r.internal: requires a running VM"}});
            }

            auto materialized = materialize_system();
            if (!materialized) return std::unexpected(materialized.error());
            const auto& sys  = materialized->first;
            const auto& vars = materialized->second;
            if (auto domains_ok = ensure_real_domains(vars); !domains_ok) {
                return std::unexpected(domains_ok.error());
            }

            clp::Simplex simplex;
            for (const auto& row : sys.leq) simplex.add_leq(row);
            for (const auto& row : sys.eq)  simplex.add_eq(row);

            for (auto id : vars) {
                if (const auto* sb = vm->real_store().simplex_bounds(id)) {
                    if (sb->lo.has_value()) simplex.assert_lower(id, *sb->lo);
                    if (sb->hi.has_value()) simplex.assert_upper(id, *sb->hi);
                }
            }

            const auto feasible = simplex.check(kRealSimplexEps);
            switch (feasible) {
                case clp::SimplexStatus::Feasible:
                case clp::SimplexStatus::Unbounded:
                    break;
                case clp::SimplexStatus::Infeasible:
                    return false;
                case clp::SimplexStatus::NumericFailure:
                    return std::unexpected(r_user_error(
                        "clp.r.simplex.numeric-failure: simplex numeric failure"));
            }

#ifdef ETA_CLP_FM_ORACLE
            clp::FMSystem oracle_sys = sys;
            for (auto id : vars) {
                if (const auto* sb = vm->real_store().simplex_bounds(id)) {
                    if (sb->lo.has_value() && std::isfinite(sb->lo->value)) {
                        clp::LinearExpr lo_row;
                        lo_row.terms.push_back(clp::LinearTerm{
                            .var_id = id,
                            .coef = -1.0,
                        });
                        lo_row.constant = sb->lo->value +
                            (sb->lo->strict ? kRealOracleCfg.eps : 0.0);
                        lo_row.canonicalize();
                        oracle_sys.leq.push_back(std::move(lo_row));
                    }
                    if (sb->hi.has_value() && std::isfinite(sb->hi->value)) {
                        clp::LinearExpr hi_row;
                        hi_row.terms.push_back(clp::LinearTerm{
                            .var_id = id,
                            .coef = 1.0,
                        });
                        hi_row.constant = -sb->hi->value +
                            (sb->hi->strict ? kRealOracleCfg.eps : 0.0);
                        hi_row.canonicalize();
                        oracle_sys.leq.push_back(std::move(hi_row));
                    }
                }
            }

            const auto oracle_feasible = clp::fm_feasible(oracle_sys, kRealOracleCfg);
            const bool simplex_is_feasible =
                (feasible == clp::SimplexStatus::Feasible || feasible == clp::SimplexStatus::Unbounded);
            const bool fm_is_feasible = (oracle_feasible.status == clp::FMStatus::Feasible);
            if (simplex_is_feasible != fm_is_feasible) {
                return std::unexpected(r_user_error(
                    "clp.r.oracle-mismatch: simplex/fm feasibility divergence"));
            }
#endif

            for (auto id : vars) {
                const auto bounds_res = simplex.bounds_for(id, kRealSimplexEps);
                switch (bounds_res.status) {
                    case clp::SimplexStatus::Feasible:
                    case clp::SimplexStatus::Unbounded:
                        break;
                    case clp::SimplexStatus::Infeasible:
                        return false;
                    case clp::SimplexStatus::NumericFailure:
                        return std::unexpected(r_user_error(
                            "clp.r.simplex.numeric-failure: simplex numeric failure"));
                }
                if (!bounds_res.bounds.has_value()) {
                    return std::unexpected(r_user_error(
                        "clp.r.internal: feasible projection missing bounds"));
                }

                const auto projected = *bounds_res.bounds;

                std::optional<clp::Bound> asserted_lo;
                if (std::isfinite(projected.lo)) {
                    asserted_lo = clp::Bound{
                        .value = projected.lo,
                        .strict = projected.lo_open,
                    };
                }
                std::optional<clp::Bound> asserted_hi;
                if (std::isfinite(projected.hi)) {
                    asserted_hi = clp::Bound{
                        .value = projected.hi,
                        .strict = projected.hi_open,
                    };
                }
                vm->trail_assert_simplex_bound(id, asserted_lo, asserted_hi);

#ifdef ETA_CLP_FM_ORACLE
                const auto fm_bounds = clp::fm_bounds_for(oracle_sys, id, kRealOracleCfg);
                if (fm_bounds.status == clp::FMStatus::Infeasible) {
                    return false;
                }
                if (fm_bounds.status == clp::FMStatus::CapExceeded) {
                    return std::unexpected(r_user_error(
                        "clp.r.oracle-mismatch: fm oracle cap exceeded"));
                }
                if (!fm_bounds.bounds.has_value()) {
                    return std::unexpected(r_user_error(
                        "clp.r.oracle-mismatch: fm oracle missing bounds"));
                }
                auto approx = [](double a, double b) -> bool {
                    if (std::isinf(a) || std::isinf(b)) return a == b;
                    return std::abs(a - b) <= 1e-7;
                };
                if (!approx(projected.lo, fm_bounds.bounds->lo) ||
                    !approx(projected.hi, fm_bounds.bounds->hi)) {
                    return std::unexpected(r_user_error(
                        "clp.r.oracle-mismatch: simplex/fm bound divergence"));
                }
#endif

                const auto* cur_dom = vm->constraint_store().get_domain(id);
                if (!cur_dom) {
                    if (!projected.empty() && !is_unbounded(projected)) {
                        vm->trail_set_domain(id, projected);
                    }
                    continue;
                }

                if (!std::holds_alternative<clp::RDomain>(*cur_dom)) {
                    return std::unexpected(mixed_domain_error(id));
                }

                const auto current = std::get<clp::RDomain>(*cur_dom);
                const auto narrowed = current.intersect(projected);
                if (narrowed.empty()) return false;
                if (!same_rdomain(current, narrowed)) {
                    vm->trail_set_domain(id, narrowed);
                }
            }

            return true;
        };

        auto post_relation =
            [vm, tighten_real_bounds](clp::RealRelation rel, clp::LinearExpr expr)
            -> std::expected<LispVal, RuntimeError> {
            if (!vm) {
                return std::unexpected(RuntimeError{VMError{
                    RuntimeErrorCode::TypeError,
                    "clp.r.post: requires a running VM"}});
            }
            const auto mark = vm->trail_stack().size();
            vm->trail_mark_real_store();
            if (rel == clp::RealRelation::Leq) {
                vm->real_store().append_leq(std::move(expr));
            } else {
                vm->real_store().append_eq(std::move(expr));
            }

            auto ok = tighten_real_bounds();
            if (!ok) {
                vm->rollback_trail_to(mark);
                return std::unexpected(ok.error());
            }
            if (!*ok) {
                vm->rollback_trail_to(mark);
                return False;
            }
            return True;
        };

        env.register_builtin("%clp-r-post-leq!", 2, false,
            [linear_diff, post_relation](Args args) -> std::expected<LispVal, RuntimeError> {
                auto diff = linear_diff(args[0], args[1]);
                if (!diff) return std::unexpected(diff.error());
                return post_relation(clp::RealRelation::Leq, std::move(*diff));
            });

        env.register_builtin("%clp-r-post-eq!", 2, false,
            [linear_diff, post_relation](Args args) -> std::expected<LispVal, RuntimeError> {
                auto diff = linear_diff(args[0], args[1]);
                if (!diff) return std::unexpected(diff.error());
                return post_relation(clp::RealRelation::Eq, std::move(*diff));
            });

        env.register_builtin("%clp-r-propagate!", 0, false,
            [vm, tighten_real_bounds](Args) -> std::expected<LispVal, RuntimeError> {
                if (!vm) {
                    return std::unexpected(RuntimeError{VMError{
                        RuntimeErrorCode::TypeError,
                        "clp.r.propagate: requires a running VM"}});
                }
                const auto mark = vm->trail_stack().size();
                auto ok = tighten_real_bounds();
                if (!ok) {
                    vm->rollback_trail_to(mark);
                    return std::unexpected(ok.error());
                }
                if (!*ok) {
                    vm->rollback_trail_to(mark);
                    return False;
                }
                return True;
            });

        auto pack_optimization_result =
            [&heap](double optimum,
                    const std::vector<std::pair<ObjectId, double>>& witness_entries)
            -> std::expected<LispVal, RuntimeError> {
            auto roots = heap.make_external_root_frame();
            LispVal witness = Nil;
            roots.push(witness);

            for (auto it = witness_entries.rbegin(); it != witness_entries.rend(); ++it) {
                const LispVal var = ops::box(Tag::HeapObject, static_cast<int64_t>(it->first));
                auto value = make_flonum(it->second);
                if (!value) return std::unexpected(value.error());
                roots.push(*value);

                auto pair = make_cons(heap, var, *value);
                if (!pair) return std::unexpected(pair.error());
                roots.push(*pair);

                auto cell = make_cons(heap, *pair, witness);
                if (!cell) return std::unexpected(cell.error());
                witness = *cell;
                roots.push(witness);
            }

            auto opt_value = make_flonum(optimum);
            if (!opt_value) return std::unexpected(opt_value.error());
            roots.push(*opt_value);
            return make_cons(heap, *opt_value, witness);
        };

        auto build_qp_model =
            [vm, r_user_error](const clp::QuadraticObjectiveMatrix& objective,
                               const clp::FMSystem& sys,
                               const std::vector<ObjectId>& vars)
            -> std::expected<clp::QPModel, RuntimeError> {
            if (!vm) {
                return std::unexpected(RuntimeError{VMError{
                    RuntimeErrorCode::TypeError,
                    "clp.r.internal: requires a running VM"}});
            }

            clp::QPModel model;
            model.vars = vars;
            const auto n = model.vars.size();
            if (n > 0 && n > (std::numeric_limits<std::size_t>::max() / n)) {
                return std::unexpected(r_user_error(
                    "clp.r.qp.numeric-failure: QP variable dimension overflow"));
            }
            model.q.assign(n * n, 0.0);
            model.c.assign(n, 0.0);
            model.k = objective.k;

            std::unordered_map<ObjectId, std::size_t> index_of;
            index_of.reserve(n);
            for (std::size_t i = 0; i < n; ++i) {
                index_of.emplace(model.vars[i], i);
            }

            for (std::size_t i = 0; i < objective.vars.size(); ++i) {
                const auto it = index_of.find(objective.vars[i]);
                if (it == index_of.end()) {
                    return std::unexpected(r_user_error(
                        "clp.r.qp.numeric-failure: objective variable mapping failed"));
                }
                const auto gi = it->second;
                const double ci = objective.c[i];
                if (!std::isfinite(ci)) {
                    return std::unexpected(r_user_error(
                        "clp.r.qp.numeric-failure: objective linear coefficient is not finite"));
                }
                model.c[gi] += ci;
                if (!std::isfinite(model.c[gi])) {
                    return std::unexpected(r_user_error(
                        "clp.r.qp.numeric-failure: objective linear accumulation is not finite"));
                }

                for (std::size_t j = 0; j < objective.vars.size(); ++j) {
                    const auto jt = index_of.find(objective.vars[j]);
                    if (jt == index_of.end()) {
                        return std::unexpected(r_user_error(
                            "clp.r.qp.numeric-failure: objective variable mapping failed"));
                    }
                    const auto gj = jt->second;
                    const double qij = objective.q_at(i, j);
                    if (!std::isfinite(qij)) {
                        return std::unexpected(r_user_error(
                            "clp.r.qp.numeric-failure: objective Hessian entry is not finite"));
                    }
                    model.q[gi * n + gj] += qij;
                    if (!std::isfinite(model.q[gi * n + gj])) {
                        return std::unexpected(r_user_error(
                            "clp.r.qp.numeric-failure: objective Hessian accumulation is not finite"));
                    }
                }
            }

            auto append_row =
                [&](const clp::LinearExpr& row,
                    std::vector<double>& target_a,
                    std::vector<double>& target_b)
                -> std::expected<void, RuntimeError> {
                std::vector<double> coeffs(n, 0.0);
                for (const auto& t : row.terms) {
                    if (!std::isfinite(t.coef)) {
                        return std::unexpected(r_user_error(
                            "clp.r.qp.numeric-failure: constraint coefficient is not finite"));
                    }
                    const auto it = index_of.find(t.var_id);
                    if (it == index_of.end()) {
                        return std::unexpected(r_user_error(
                            "clp.r.qp.numeric-failure: constraint variable mapping failed"));
                    }
                    coeffs[it->second] += t.coef;
                    if (!std::isfinite(coeffs[it->second])) {
                        return std::unexpected(r_user_error(
                            "clp.r.qp.numeric-failure: constraint row accumulation is not finite"));
                    }
                }
                const double rhs = -row.constant;
                if (!std::isfinite(rhs)) {
                    return std::unexpected(r_user_error(
                        "clp.r.qp.numeric-failure: constraint constant is not finite"));
                }
                target_a.insert(target_a.end(), coeffs.begin(), coeffs.end());
                target_b.push_back(rhs);
                return {};
            };

            for (const auto& row : sys.leq) {
                auto ok = append_row(row, model.a_leq, model.b_leq);
                if (!ok) return std::unexpected(ok.error());
            }
            for (const auto& row : sys.eq) {
                auto ok = append_row(row, model.a_eq, model.b_eq);
                if (!ok) return std::unexpected(ok.error());
            }

            for (auto id : vars) {
                const auto* sb = vm->real_store().simplex_bounds(id);
                if (!sb) continue;
                const auto it = index_of.find(id);
                if (it == index_of.end()) {
                    return std::unexpected(r_user_error(
                        "clp.r.qp.numeric-failure: bound variable mapping failed"));
                }
                const auto idx = it->second;

                if (sb->lo.has_value()) {
                    const double lo = sb->lo->value + (sb->lo->strict ? kRealSimplexEps : 0.0);
                    if (!std::isfinite(lo)) {
                        if (lo > 0.0) {
                            return std::unexpected(r_user_error(
                                "clp.r.qp.numeric-failure: lower bound is not finite"));
                        }
                    } else {
                        std::vector<double> coeffs(n, 0.0);
                        coeffs[idx] = -1.0;
                        model.a_leq.insert(model.a_leq.end(), coeffs.begin(), coeffs.end());
                        model.b_leq.push_back(-lo);
                    }
                }
                if (sb->hi.has_value()) {
                    const double hi = sb->hi->value - (sb->hi->strict ? kRealSimplexEps : 0.0);
                    if (!std::isfinite(hi)) {
                        if (hi < 0.0) {
                            return std::unexpected(r_user_error(
                                "clp.r.qp.numeric-failure: upper bound is not finite"));
                        }
                    } else {
                        std::vector<double> coeffs(n, 0.0);
                        coeffs[idx] = 1.0;
                        model.a_leq.insert(model.a_leq.end(), coeffs.begin(), coeffs.end());
                        model.b_leq.push_back(hi);
                    }
                }
            }

            return model;
        };

        auto build_qp_initial_guess =
            [vm, r_user_error](const clp::FMSystem& sys,
                               const std::vector<ObjectId>& vars)
            -> std::expected<std::optional<std::vector<double>>, RuntimeError> {
            if (!vm) {
                return std::unexpected(RuntimeError{VMError{
                    RuntimeErrorCode::TypeError,
                    "clp.r.internal: requires a running VM"}});
            }

            clp::Simplex simplex;
            for (const auto& row : sys.leq) simplex.add_leq(row);
            for (const auto& row : sys.eq) simplex.add_eq(row);
            for (auto id : vars) {
                if (const auto* sb = vm->real_store().simplex_bounds(id)) {
                    if (sb->lo.has_value()) simplex.assert_lower(id, *sb->lo);
                    if (sb->hi.has_value()) simplex.assert_upper(id, *sb->hi);
                }
            }

            const auto feasible = simplex.check(kRealSimplexEps);
            switch (feasible) {
                case clp::SimplexStatus::Feasible:
                case clp::SimplexStatus::Unbounded:
                    break;
                case clp::SimplexStatus::Infeasible:
                    return std::optional<std::vector<double>>{};
                case clp::SimplexStatus::NumericFailure:
                    return std::unexpected(r_user_error(
                        "clp.r.simplex.numeric-failure: simplex numeric failure"));
            }

            clp::LinearExpr zero_objective;
            const auto seed = simplex.optimize(
                std::move(zero_objective),
                clp::SimplexDirection::Minimize,
                kRealSimplexEps);
            switch (seed.status) {
                case clp::SimplexOptResult::Status::Optimal:
                    break;
                case clp::SimplexOptResult::Status::Infeasible:
                    return std::optional<std::vector<double>>{};
                case clp::SimplexOptResult::Status::Unbounded:
                    return std::unexpected(r_user_error(
                        "clp.r.simplex.numeric-failure: failed to extract a feasible seed"));
                case clp::SimplexOptResult::Status::NumericFailure:
                    return std::unexpected(r_user_error(
                        "clp.r.simplex.numeric-failure: simplex numeric failure"));
            }

            std::unordered_map<ObjectId, std::size_t> index_of;
            index_of.reserve(vars.size());
            for (std::size_t i = 0; i < vars.size(); ++i) {
                index_of.emplace(vars[i], i);
            }

            std::vector<double> x(vars.size(), 0.0);
            for (const auto& [id, value] : seed.witness) {
                const auto it = index_of.find(id);
                if (it == index_of.end()) continue;
                x[it->second] = value;
            }
            return x;
        };

        auto optimize_real_objective =
            [&heap, &intern_table, vm, r_user_error, format_quadratic_linearize_error,
             format_quadratic_model_error, materialize_system, materialize_quadratic_expr,
             ensure_real_domains, pack_optimization_result]
            (LispVal objective, clp::SimplexDirection direction)
            -> std::expected<LispVal, RuntimeError> {
            if (!vm) {
                return std::unexpected(RuntimeError{VMError{
                    RuntimeErrorCode::TypeError,
                    "clp.r.optimize: requires a running VM"}});
            }

            auto raw_objective = clp::linearize_quadratic_objective(objective, heap, intern_table);
            if (!raw_objective) {
                return std::unexpected(r_user_error(
                    format_quadratic_linearize_error(raw_objective.error())));
            }
            auto objective_expr = materialize_quadratic_expr(std::move(*raw_objective));
            if (!objective_expr) return std::unexpected(objective_expr.error());

            auto objective_matrix =
                clp::materialize_quadratic_objective_matrix(*objective_expr);
            if (!objective_matrix) {
                return std::unexpected(r_user_error(
                    format_quadratic_model_error(objective_matrix.error())));
            }

            auto materialized = materialize_system();
            if (!materialized) return std::unexpected(materialized.error());
            const auto& sys = materialized->first;
            auto vars = materialized->second;
            vars.reserve(vars.size() + objective_matrix->vars.size());
            vars.insert(vars.end(),
                        objective_matrix->vars.begin(),
                        objective_matrix->vars.end());
            std::sort(vars.begin(), vars.end());
            vars.erase(std::unique(vars.begin(), vars.end()), vars.end());
            if (auto domains_ok = ensure_real_domains(vars); !domains_ok) {
                return std::unexpected(domains_ok.error());
            }

            const double hessian_sign =
                (direction == clp::SimplexDirection::Minimize) ? 1.0 : -1.0;
            auto convexity = clp::check_quadratic_convexity(
                *objective_matrix, hessian_sign);
            if (!convexity) {
                return std::unexpected(r_user_error(
                    format_quadratic_model_error(convexity.error())));
            }

            if (!objective_expr->quadratic_terms.empty()) {
                return std::unexpected(r_user_error(
                    "clp.r.qp.objective-nonlinear-unsupported: quadratic objective requires QP backend"));
            }

            clp::LinearExpr objective_linear;
            objective_linear.constant = objective_matrix->k;
            objective_linear.terms.reserve(objective_matrix->vars.size());
            for (std::size_t i = 0; i < objective_matrix->vars.size(); ++i) {
                const auto coef = objective_matrix->c[i];
                if (coef == 0.0) continue;
                objective_linear.terms.push_back(clp::LinearTerm{
                    .var_id = objective_matrix->vars[i],
                    .coef = coef,
                });
            }
            objective_linear.canonicalize();

            clp::Simplex simplex;
            for (const auto& row : sys.leq) simplex.add_leq(row);
            for (const auto& row : sys.eq) simplex.add_eq(row);
            for (auto id : vars) {
                if (const auto* sb = vm->real_store().simplex_bounds(id)) {
                    if (sb->lo.has_value()) simplex.assert_lower(id, *sb->lo);
                    if (sb->hi.has_value()) simplex.assert_upper(id, *sb->hi);
                }
            }

            const auto result = simplex.optimize(std::move(objective_linear), direction, kRealSimplexEps);
            switch (result.status) {
                case clp::SimplexOptResult::Status::Optimal:
                    break;
                case clp::SimplexOptResult::Status::Infeasible:
                    return False;
                case clp::SimplexOptResult::Status::Unbounded:
                    return make_symbol(intern_table, "clp.r.unbounded");
                case clp::SimplexOptResult::Status::NumericFailure:
                    return std::unexpected(r_user_error(
                        "clp.r.simplex.numeric-failure: simplex numeric failure"));
            }
            return pack_optimization_result(result.value, result.witness);
        };

        auto optimize_real_qp_objective =
            [&heap, &intern_table, vm, r_user_error, format_quadratic_linearize_error,
             format_quadratic_model_error, format_qp_solve_error, materialize_system,
             materialize_quadratic_expr, ensure_real_domains, build_qp_model,
             build_qp_initial_guess, pack_optimization_result, optimize_real_objective]
            (LispVal objective, clp::SimplexDirection direction)
            -> std::expected<LispVal, RuntimeError> {
            if (!vm) {
                return std::unexpected(RuntimeError{VMError{
                    RuntimeErrorCode::TypeError,
                    "clp.r.optimize: requires a running VM"}});
            }

            auto raw_objective = clp::linearize_quadratic_objective(objective, heap, intern_table);
            if (!raw_objective) {
                return std::unexpected(r_user_error(
                    format_quadratic_linearize_error(raw_objective.error())));
            }
            auto objective_expr = materialize_quadratic_expr(std::move(*raw_objective));
            if (!objective_expr) return std::unexpected(objective_expr.error());

            auto objective_matrix =
                clp::materialize_quadratic_objective_matrix(*objective_expr);
            if (!objective_matrix) {
                return std::unexpected(r_user_error(
                    format_quadratic_model_error(objective_matrix.error())));
            }

            auto materialized = materialize_system();
            if (!materialized) return std::unexpected(materialized.error());
            const auto& sys = materialized->first;
            auto vars = materialized->second;
            vars.reserve(vars.size() + objective_matrix->vars.size());
            vars.insert(vars.end(),
                        objective_matrix->vars.begin(),
                        objective_matrix->vars.end());
            std::sort(vars.begin(), vars.end());
            vars.erase(std::unique(vars.begin(), vars.end()), vars.end());
            if (auto domains_ok = ensure_real_domains(vars); !domains_ok) {
                return std::unexpected(domains_ok.error());
            }

            const double hessian_sign =
                (direction == clp::SimplexDirection::Minimize) ? 1.0 : -1.0;
            auto convexity = clp::check_quadratic_convexity(
                *objective_matrix, hessian_sign);
            if (!convexity) {
                return std::unexpected(r_user_error(
                    format_quadratic_model_error(convexity.error())));
            }

            if (objective_expr->quadratic_terms.empty()) {
                return optimize_real_objective(objective, direction);
            }

            auto initial = build_qp_initial_guess(sys, vars);
            if (!initial) return std::unexpected(initial.error());
            if (!initial->has_value()) return False;

            auto qp_model = build_qp_model(*objective_matrix, sys, vars);
            if (!qp_model) return std::unexpected(qp_model.error());

            auto solve = clp::solve_quadratic_program(
                *qp_model, direction, std::move(initial->value()));
            if (!solve) {
                return std::unexpected(r_user_error(
                    format_qp_solve_error(solve.error())));
            }

            switch (solve->status) {
                case clp::QPSolveResult::Status::Optimal:
                    return pack_optimization_result(solve->value, solve->witness);
                case clp::QPSolveResult::Status::Infeasible:
                    return False;
                case clp::QPSolveResult::Status::Unbounded:
                    return make_symbol(intern_table, "clp.r.unbounded");
            }

            return std::unexpected(r_user_error(
                "clp.r.qp.numeric-failure: unknown QP solver status"));
        };

        env.register_builtin("%clp-r-minimize", 1, false,
            [optimize_real_objective](Args args) -> std::expected<LispVal, RuntimeError> {
                return optimize_real_objective(args[0], clp::SimplexDirection::Minimize);
            });

        env.register_builtin("%clp-r-maximize", 1, false,
            [optimize_real_objective](Args args) -> std::expected<LispVal, RuntimeError> {
                return optimize_real_objective(args[0], clp::SimplexDirection::Maximize);
            });

        env.register_builtin("%clp-r-qp-minimize", 1, false,
            [optimize_real_qp_objective](Args args) -> std::expected<LispVal, RuntimeError> {
                return optimize_real_qp_objective(args[0], clp::SimplexDirection::Minimize);
            });

        env.register_builtin("%clp-r-qp-maximize", 1, false,
            [optimize_real_qp_objective](Args args) -> std::expected<LispVal, RuntimeError> {
                return optimize_real_qp_objective(args[0], clp::SimplexDirection::Maximize);
            });
    }

    /**
     * CLP(FD) native bounds-consistency propagators
     *
     *
     * Each returns #t on success (including "nothing to do"), #f on detected
     * inconsistency (empty domain).  Narrowing goes through the trailed
     * ConstraintStore::set_domain so backtracking correctly restores state.
     *
     * These are the bounds kernel only; re-firing on variable binding is
     * installed in Eta-level `std.clp` via a `clp.prop` attribute hook.
     */
    {
        /// Helper: deref a LispVal through any binding chain.
        auto deref = [&heap](LispVal v) -> LispVal {
            for (;;) {
                if (!ops::is_boxed(v) || ops::tag(v) != Tag::HeapObject) return v;
                auto id = ops::payload(v);
                auto* lv = heap.try_get_as<ObjectKind::LogicVar, types::LogicVar>(id);
                if (!lv || !lv->binding.has_value()) return v;
                v = *lv->binding;
            }
        };

        /**
         * Bounds snapshot of a CLP(FD) argument: ground integer, unbound var
         * with Z or FD domain, or unbound var with no domain (unbounded).
         */
        struct Bounds {
            int64_t  lo     = 0;
            int64_t  hi     = 0;
            bool     finite = false;        ///< has finite [lo,hi]
            bool     is_var = false;        ///< unbound logic var
            ObjectId id     = 0;            ///< heap id when is_var
            bool     is_fd  = false;        ///< FD domain (else Z or none)
        };

        /**
         * Extract a Bounds for arg. Returns std::nullopt on type error
         * with lo > hi so caller detects infeasibility uniformly.
         */
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
                                if (fd->empty()) {
                                    b.lo = 1; b.hi = 0; b.finite = true;  ///< empty sentinel
                                } else {
                                    b.lo = fd->min();
                                    b.hi = fd->max();
                                    b.finite = true;
                                }
                            }
                            /**
                             * R-domained vars are intentionally
                             * FD bounds-consistency is integer-only, so an
                             * R-domained operand simply contributes no
                             * narrowing (b.finite stays false).
                             */
                            else if (std::holds_alternative<clp::RDomain>(*dom)) {
                                /* fall-through: keep finite=false */
                            }
                        }
                    }
                    return b;
                }
            }
            /// Ground: must be an integer
            auto n = classify_numeric(d, heap);
            if (!n.is_valid() || n.is_flonum()) return std::nullopt;
            Bounds b;
            b.lo = n.int_val; b.hi = n.int_val; b.finite = true;
            return b;
        };

        /**
         * Narrow a var's domain to [new_lo, new_hi]. Returns false on empty.
         * Only writes through trail_set_domain (trailed) when something actually
         * changes.  For FD domains, values outside [new_lo, new_hi] are filtered out.
         */
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
                if (lo == z->lo && hi == z->hi) return true;  ///< no change
                vm->trail_set_domain(id, clp::ZDomain{ lo, hi });
                return true;
            }
            /**
             * R-domained vars are not narrowed by FD bounds
             * (extract_bounds reports !finite, so this branch is normally
             * unreachable; defensive no-op preserves R domain unchanged).
             */
            if (std::holds_alternative<clp::RDomain>(*dom)) return true;
            /// FD
            const auto& fd = std::get<clp::FDDomain>(*dom);
            const int64_t old_size = fd.size();
            clp::FDDomain nfd = fd.intersect_z(new_lo, new_hi);
            if (nfd.empty()) return false;
            if (nfd.size() == old_size) return true;  ///< no change
            vm->trail_set_domain(id, std::move(nfd));
            return true;
        };

        /**
         * Bounds consistency (interval form):
         *   z.lo >= x.lo + y.lo   z.hi <= x.hi + y.hi
         *   x.lo >= z.lo - y.hi   x.hi <= z.hi - y.lo
         *   y.lo >= z.lo - x.hi   y.hi <= z.hi - x.lo
         */
        env.register_builtin("%clp-fd-plus!", 3, false,
            [extract_bounds, narrow_var](Args args) -> std::expected<LispVal, RuntimeError> {
                auto bx = extract_bounds(args[0]);
                auto by = extract_bounds(args[1]);
                auto bz = extract_bounds(args[2]);
                if (!bx || !by || !bz)
                    return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError,
                        "%clp-fd-plus!: arguments must be integers or logic variables"}});
                /**
                 * succeed without narrowing (MVP: propagate only when all three
                 * are finite).
                 */
                if (!bx->finite || !by->finite || !bz->finite) return True;
                /// Empty-domain short-circuit.
                if (bx->lo > bx->hi || by->lo > by->hi || bz->lo > bz->hi) return False;
                int64_t nz_lo = bx->lo + by->lo,  nz_hi = bx->hi + by->hi;
                int64_t nx_lo = bz->lo - by->hi,  nx_hi = bz->hi - by->lo;
                int64_t ny_lo = bz->lo - bx->hi,  ny_hi = bz->hi - bx->lo;
                /// Narrow each var (ignores ground operands).
                if (bz->is_var && !narrow_var(bz->id, nz_lo, nz_hi)) return False;
                if (bx->is_var && !narrow_var(bx->id, nx_lo, nx_hi)) return False;
                if (by->is_var && !narrow_var(by->id, ny_lo, ny_hi)) return False;
                /// For ground operands, verify consistency (e.g. z=5 must satisfy
                if (!bz->is_var && (bz->lo < nz_lo || bz->hi > nz_hi)) return False;
                if (!bx->is_var && (bx->lo < nx_lo || bx->hi > nx_hi)) return False;
                if (!by->is_var && (by->lo < ny_lo || by->hi > ny_hi)) return False;
                return True;
            });

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

        /// Bounds:
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
                /// Forward: narrow y.
                if (by->is_var && !narrow_var(by->id, ny_lo, ny_hi)) return False;
                if (!by->is_var && (by->lo < ny_lo || by->hi > ny_hi)) return False;
                /// Backward: narrow x using (possibly updated) y bounds.
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

        /**
         * Bounds consistency via interval multiplication.  The product of
         * two intervals is [min of corners, max of corners].  Division for
         * back-propagation is implemented with explicit floor/ceil to keep
         * results integral; divisors straddling zero leave the quotient
         * variable unconstrained (weak propagation, consistent with SWI).
         */
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
                /// Forward: z = x * y
                int64_t nz_lo, nz_hi;
                interval_mul(bx->lo, bx->hi, by->lo, by->hi, nz_lo, nz_hi);
                if (bz->is_var && !narrow_var(bz->id, nz_lo, nz_hi)) return False;
                if (!bz->is_var && (bz->lo < nz_lo || bz->hi > nz_hi)) return False;
                /// Backward x = z / y (only when y does not straddle 0)
                auto narrow_quot = [&](const Bounds& src, const Bounds& div) -> std::optional<std::pair<int64_t,int64_t>> {
                    if (div.lo <= 0 && div.hi >= 0) return std::nullopt;
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

        /// `xs` is an Eta list of logic vars and/or ground integers.  Bounds:
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
                    if (!b->finite) return True;
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
                /// Forward: narrow s
                if (bs_b->is_var && !narrow_var(bs_b->id, tot_lo, tot_hi)) return False;
                if (!bs_b->is_var && (bs_b->lo < tot_lo || bs_b->hi > tot_hi)) return False;
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

        /**
         * `cs` is a list of ground integer coefficients of the same length
         * the same subtractive back-propagation as fd_sum.
         */
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
                /// Forward narrow s.
                if (bs_s->is_var && !narrow_var(bs_s->id, tot_lo, tot_hi)) return False;
                if (!bs_s->is_var && (bs_s->lo < tot_lo || bs_s->hi > tot_hi)) return False;
                int64_t s_lo = std::max(bs_s->lo, tot_lo);
                int64_t s_hi = std::min(bs_s->hi, tot_hi);
                for (std::size_t j = 0; j < bs.size(); ++j) {
                    if (!bs[j].is_var || cs[j] == 0) continue;
                    int64_t other_lo = tot_lo - term[j].first;
                    int64_t other_hi = tot_hi - term[j].second;
                    int64_t t_lo = s_lo - other_hi;
                    int64_t t_hi = s_hi - other_lo;
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

        /**
         * One-based index to match Prolog `element/3` convention.  When `i`
         * is ground, degenerates to a single equality via narrowing.  When
         * `i` is a logic var with an FD domain, we intersect `v`'s bounds
         * with the union of the candidate xs[k]'s bounds, and prune `i`'s
         * domain to values `k` whose xs[k] is consistent with `v`.
         */
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
                if (bi->is_var && !narrow_var(bi->id, 1, n)) return False;
                if (!bi->is_var && (bi->lo < 1 || bi->hi > n)) return False;
                int64_t i_lo = bi->is_var ? std::max<int64_t>(1, bi->lo) : bi->lo;
                int64_t i_hi = bi->is_var ? std::min<int64_t>(n, bi->hi) : bi->hi;
                /**
                 * its bounds union is the possible value for v.  Also collect
                 * the set of k's that are consistent with v's current bounds.
                 */
                int64_t v_union_lo = INT64_MAX, v_union_hi = INT64_MIN;
                std::vector<int64_t> compatible_ks;
                bool any_infinite_candidate = false;
                for (int64_t k = i_lo; k <= i_hi; ++k) {
                    /// If i is FD-domained, skip values not in its FD set.
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
                    if (bk->lo > bk->hi) continue;  ///< this candidate is infeasible
                    /// Is this candidate consistent with v?
                    int64_t isect_lo = std::max(bv->lo, bk->lo);
                    int64_t isect_hi = std::min(bv->hi, bk->hi);
                    if (bv->finite && isect_lo > isect_hi) continue;
                    compatible_ks.push_back(k);
                    v_union_lo = std::min(v_union_lo, bk->lo);
                    v_union_hi = std::max(v_union_hi, bk->hi);
                }
                if (compatible_ks.empty()) return False;
                /// Narrow v to the union of candidate bounds (skip when an
                if (!any_infinite_candidate) {
                    if (bv->is_var && !narrow_var(bv->id, v_union_lo, v_union_hi)) return False;
                    if (!bv->is_var && (bv->lo < v_union_lo || bv->hi > v_union_hi)) return False;
                }
                /**
                 * Narrow i to the compatible set.  Use an FD domain if the
                 * set is sparse relative to [i_lo..i_hi]; otherwise Z bounds.
                 */
                if (bi->is_var && vm) {
                    const int64_t first_k = compatible_ks.front();
                    const int64_t last_k  = compatible_ks.back();
                    bool dense = (static_cast<int64_t>(compatible_ks.size()) == (last_k - first_k + 1));
                    if (dense) {
                        if (!narrow_var(bi->id, first_k, last_k)) return False;
                    } else {
                        /**
                         * `compatible_ks` is built in ascending k-order and is
                         * unique by construction; build the bit-set directly.
                         */
                        clp::FDDomain nd =
                            clp::FDDomain::from_sorted_unique(compatible_ks);
                        vm->trail_set_domain(bi->id, std::move(nd));
                    }
                }
                return True;
            });

        /**
         * Domain-consistent pruning: removes every value v from D(x) that
         * cannot participate in a valuation satisfying all_different(vars).
         * Strictly stronger than the pairwise attribute-hook version.
         */
        env.register_builtin("%clp-fd-all-different!", 1, false,
            [&heap, vm, deref, walk_list](Args args) -> std::expected<LispVal, RuntimeError> {
                if (!vm) return True;
                std::vector<LispVal> elems;
                if (!walk_list(args[0], elems))
                    return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError,
                        "%clp-fd-all-different!: argument must be a proper list"}});
                if (elems.size() <= 1) return True;

                /// Build the algorithm's var table by dereffing each element.
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
                                /**
                                 * Materialise Z interval as FD values.
                                 * Cap at a reasonable ceiling to avoid runaway
                                 * allocation for unbounded-looking domains.
                                 */
                                constexpr int64_t MAX_SPAN = 1'000'000;
                                if (z->hi - z->lo + 1 > MAX_SPAN) {
                                    av.is_free = true;
                                } else {
                                    av.domain.reserve(static_cast<std::size_t>(z->hi - z->lo + 1));
                                    for (int64_t v = z->lo; v <= z->hi; ++v) av.domain.push_back(v);
                                }
                            } else if (auto* fd = std::get_if<clp::FDDomain>(dom)) {
                                av.domain = fd->to_vector();   ///< sorted ascending
                                if (av.domain.empty()) return False;
                            } else {
                                /**
                                 * RDomain (or any future kind)
                                 * all-different.  Treat as free (no
                                 * contribution to value-graph matching).
                                 */
                                av.is_free = true;
                            }
                            avars.push_back(std::move(av));
                            continue;
                        }
                    }
                    auto n = classify_numeric(d, heap);
                    if (!n.is_valid() || n.is_flonum())
                        return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError,
                            "%clp-fd-all-different!: values must be integers or logic variables"}});
                    av.ground_val = n.int_val;
                    av.is_ground = true;
                    avars.push_back(std::move(av));
                }

                /**
                 * Narrow callback: the algorithm invokes this for each var
                 * whose domain shrank.  We install the new domain via the
                 * trailed constraint store (preserves ZDomain narrowing if
                 */
                auto narrow = [vm](uint64_t id, const std::vector<int64_t>& new_dom) -> bool {
                    if (new_dom.empty()) return false;
                    /// If contiguous, collapse to a Z domain; else FD.
                    bool contiguous = true;
                    for (std::size_t i = 1; i < new_dom.size(); ++i) {
                        if (new_dom[i] != new_dom[i - 1] + 1) { contiguous = false; break; }
                    }
                    if (contiguous) {
                        vm->trail_set_domain(id,
                            clp::ZDomain{ new_dom.front(), new_dom.back() });
                    } else {
                        clp::FDDomain fd = clp::FDDomain::from_sorted_unique(new_dom);
                        vm->trail_set_domain(id, std::move(fd));
                    }
                    return true;
                };

                return clp::run_regin_alldiff(avars, narrow) ? True : False;
            });
    }

    /**
     * CLP(B) native Boolean propagators
     *
     *   %clp-bool-card! (xs k-lo k-hi)
     *
     * an unbound logic var constrained to a domain that intersects {0,1}.
     * A 2-bit `mask` encodes the current allowed values: bit 0 = may be 0,
     * bit 1 = may be 1; mask 3 = {0,1}, mask 0 = infeasible.
     *
     * Propagation uses exhaustive-support pruning: for each constraint we
     * enumerate its truth table, keep only rows consistent with the current
     * masks, and narrow each variable to the union of its rows.  This is
     * exact (domain-consistent) on 2-value domains and is the cheapest
     * thing that works.
     *
     * Each propagator returns #t on success (including "nothing to do"),
     * #f on detected inconsistency.  Narrowing is trailed through the
     * unified VM trail (`VM::trail_set_domain`).  Re-firing on later
     * bindings is installed by the Eta-level `std.clpb` wrappers via
     * `%clp-prop-attach!`, sharing the same `'clp.prop` queue attribute
     */
    {
        /// Deref a LispVal through any binding chain.
        auto deref = [&heap](LispVal v) -> LispVal {
            for (;;) {
                if (!ops::is_boxed(v) || ops::tag(v) != Tag::HeapObject) return v;
                auto id = ops::payload(v);
                auto* lv = heap.try_get_as<ObjectKind::LogicVar, types::LogicVar>(id);
                if (!lv || !lv->binding.has_value()) return v;
                v = *lv->binding;
            }
        };

        /**
         * Walk an Eta proper list into a std::vector<LispVal>.  Returns
         * false if the list is improper (dotted tail / non-cons element).
         */
        auto walk_list = [&heap](LispVal lst, std::vector<LispVal>& out) -> bool {
            while (ops::is_boxed(lst) && ops::tag(lst) == Tag::HeapObject) {
                auto* c = heap.try_get_as<ObjectKind::Cons, types::Cons>(ops::payload(lst));
                if (!c) return false;
                out.push_back(c->car);
                lst = c->cdr;
            }
            return lst == Nil;
        };

        /**
         * Boolean view of a CLP(B) argument.
         *   mask bit 0 = may be 0; mask bit 1 = may be 1.
         *   mask == 0 means infeasible; mask == 3 means {0,1}.
         */
        struct BoolView {
            uint8_t  mask   = 3;
            bool     is_var = false;
            ObjectId id     = 0;
        };

        /**
         * Extract a BoolView for `v`.  Returns std::nullopt on a type
         * error (non-integer ground value, or integer outside {0,1}).
         */
        auto bool_view = [&heap, vm, deref](LispVal v) -> std::optional<BoolView> {
            LispVal d = deref(v);
            if (ops::is_boxed(d) && ops::tag(d) == Tag::HeapObject) {
                auto id = ops::payload(d);
                auto* lv = heap.try_get_as<ObjectKind::LogicVar, types::LogicVar>(id);
                if (lv && !lv->binding.has_value()) {
                    BoolView bv;
                    bv.is_var = true;
                    bv.id     = id;
                    bv.mask   = 3;
                    if (vm) {
                        const auto* dom = vm->constraint_store().get_domain(id);
                        if (dom) {
                            /**
                             * An R-domained var is not a valid
                             * integer set {0,1}.  Reject explicitly.
                             */
                            if (std::holds_alternative<clp::RDomain>(*dom))
                                return std::nullopt;
                            bv.mask = 0;
                            if (clp::domain_contains_int(*dom, 0)) bv.mask |= 1;
                            if (clp::domain_contains_int(*dom, 1)) bv.mask |= 2;
                        }
                    }
                    return bv;
                }
            }
            auto n = classify_numeric(d, heap);
            if (!n.is_valid() || n.is_flonum()) return std::nullopt;
            BoolView bv;
            if (n.int_val == 0) { bv.mask = 1; return bv; }
            if (n.int_val == 1) { bv.mask = 2; return bv; }
            return std::nullopt;  ///< integer but not 0/1
        };

        /**
         * Narrow `bv` to the target mask.  Returns false if the intersection
         * is empty, or if an already-ground bv is being forced to a value
         * it is not (the caller should have caught that via bv.mask probing).
         * Writes only happen when the mask actually shrinks; all writes are
         * trailed.
         */
        auto narrow_bool = [vm](const BoolView& bv, uint8_t new_mask) -> bool {
            const uint8_t m = static_cast<uint8_t>(bv.mask & new_mask);
            if (m == 0) return false;
            if (m == bv.mask) return true;       ///< no change
            if (!bv.is_var) return true;
            if (!vm)         return true;
            /// also being no change, already handled above.
            const int64_t lo = (m == 1) ? 0 : 1;
            const int64_t hi = (m == 1) ? 0 : 1;
            /**
             * Route through trailed write.  Preserve FD domain kind when the
             * current domain is FD; else use a ZDomain.  Either way the
             * singleton is exactly [lo,hi].
             */
            const auto& store = vm->constraint_store();
            const auto* dom = store.get_domain(bv.id);
            if (dom && std::holds_alternative<clp::FDDomain>(*dom)) {
                vm->trail_set_domain(bv.id, clp::FDDomain::singleton(lo));
            } else {
                vm->trail_set_domain(bv.id, clp::ZDomain{ lo, hi });
            }
            return true;
        };

        /**
         * Generic 3-variable support propagator.
         * `table[r]` encodes row r as bits: bit 0 = v0, bit 1 = v1, bit 2 = v2.
         * For each row r in [0..7], the row is "alive" iff
         *    masks[i] has bit `((r >> i) & 1)` set, for i = 0,1,2.
         * New mask for variable i is the OR over all alive rows of
         *    1 << ((r >> i) & 1).
         *
         * NB: `narrow_bool` is captured BY VALUE here (not by reference).
         * This lambda is itself captured by value into the registered
         * builtin closures, which outlive `register_core_primitives()`.
         * Holding a `&narrow_bool` reference would dangle and crash on
         */
        auto propagate_ternary = [narrow_bool](const uint8_t rows[], std::size_t n_rows,
                                               BoolView& b0, BoolView& b1, BoolView& b2) -> bool {
            uint8_t nm0 = 0, nm1 = 0, nm2 = 0;
            for (std::size_t r = 0; r < n_rows; ++r) {
                const uint8_t row = rows[r];
                const uint8_t v0 = row & 1u;
                const uint8_t v1 = (row >> 1) & 1u;
                const uint8_t v2 = (row >> 2) & 1u;
                if ((b0.mask & (1u << v0)) == 0) continue;
                if ((b1.mask & (1u << v1)) == 0) continue;
                if ((b2.mask & (1u << v2)) == 0) continue;
                nm0 |= static_cast<uint8_t>(1u << v0);
                nm1 |= static_cast<uint8_t>(1u << v1);
                nm2 |= static_cast<uint8_t>(1u << v2);
            }
            if (nm0 == 0 || nm1 == 0 || nm2 == 0) return false;
            return narrow_bool(b0, nm0)
                && narrow_bool(b1, nm1)
                && narrow_bool(b2, nm2);
        };

        /**
         * Generic 2-variable support propagator (same shape, 4 rows max).
         * Same value-capture rule for `narrow_bool` as above.
         */
        auto propagate_binary = [narrow_bool](const uint8_t rows[], std::size_t n_rows,
                                              BoolView& b0, BoolView& b1) -> bool {
            uint8_t nm0 = 0, nm1 = 0;
            for (std::size_t r = 0; r < n_rows; ++r) {
                const uint8_t row = rows[r];
                const uint8_t v0 = row & 1u;
                const uint8_t v1 = (row >> 1) & 1u;
                if ((b0.mask & (1u << v0)) == 0) continue;
                if ((b1.mask & (1u << v1)) == 0) continue;
                nm0 |= static_cast<uint8_t>(1u << v0);
                nm1 |= static_cast<uint8_t>(1u << v1);
            }
            if (nm0 == 0 || nm1 == 0) return false;
            return narrow_bool(b0, nm0) && narrow_bool(b1, nm1);
        };

        /**
         * ---- Truth tables.  Row encoding: bit i = variable i's value. ----
         *
         * For (z x y) we use the convention: bit 0 = z, bit 1 = x, bit 2 = y.
         * So row `(z << 0) | (x << 1) | (y << 2)` means (z, x, y).
         */

        ///   z = x AND y  :   {(0,0,0), (0,0,1), (0,1,0), (1,1,1)}
        static constexpr uint8_t TT_AND[] = { 0b000, 0b100, 0b010, 0b111 };
        ///   z = x OR y   :   {(0,0,0), (1,0,1), (1,1,0), (1,1,1)}
        static constexpr uint8_t TT_OR[]  = { 0b000, 0b101, 0b011, 0b111 };
        ///   z = x XOR y  :   {(0,0,0), (1,0,1), (1,1,0), (0,1,1)}
        static constexpr uint8_t TT_XOR[] = { 0b000, 0b101, 0b011, 0b110 };
        ///     (z,x,y): (1,0,0), (1,0,1), (0,1,0), (1,1,1)
        static constexpr uint8_t TT_IMP[] = { 0b001, 0b101, 0b010, 0b111 };
        ///   z = x EQ y   :   (1,0,0), (0,0,1), (0,1,0), (1,1,1)
        static constexpr uint8_t TT_EQ[]  = { 0b001, 0b100, 0b010, 0b111 };

        /**
         *   For (z x): bit 0 = z, bit 1 = x.
         *   z = NOT x    :   (1,0), (0,1)
         */
        static constexpr uint8_t TT_NOT[] = { 0b01, 0b10 };

        auto register_ternary = [&env, bool_view, propagate_ternary]
            (const char* name, const uint8_t* table, std::size_t n_rows) {
            env.register_builtin(name, 3, false,
                [bool_view, propagate_ternary, table, n_rows, name](Args args)
                    -> std::expected<LispVal, RuntimeError> {
                    auto bz = bool_view(args[0]);
                    auto bx = bool_view(args[1]);
                    auto by = bool_view(args[2]);
                    if (!bz || !bx || !by)
                        return std::unexpected(RuntimeError{VMError{
                            RuntimeErrorCode::TypeError,
                            std::string(name) + ": arguments must be booleans (0/1 or logic vars)"}});
                    return propagate_ternary(table, n_rows, *bz, *bx, *by) ? True : False;
                });
        };

        register_ternary("%clp-bool-and!", TT_AND, 4);
        register_ternary("%clp-bool-or!",  TT_OR,  4);
        register_ternary("%clp-bool-xor!", TT_XOR, 4);
        register_ternary("%clp-bool-imp!", TT_IMP, 4);
        register_ternary("%clp-bool-eq!",  TT_EQ,  4);

        env.register_builtin("%clp-bool-not!", 2, false,
            [bool_view, propagate_binary](Args args)
                -> std::expected<LispVal, RuntimeError> {
                auto bz = bool_view(args[0]);
                auto bx = bool_view(args[1]);
                if (!bz || !bx)
                    return std::unexpected(RuntimeError{VMError{
                        RuntimeErrorCode::TypeError,
                        "%clp-bool-not!: arguments must be booleans (0/1 or logic vars)"}});
                return propagate_binary(TT_NOT, 2, *bz, *bx) ? True : False;
            });

        /**
         *
         * Classic cardinality propagation:
         *   let forced_1 = |{ x : mask(x) = {1} }|
         *   fail   if forced_1   > k_hi  or  possible_1 < k_lo
         *   force 0 on every open var if forced_1   == k_hi
         *   force 1 on every open var if possible_1 == k_lo
         */
        env.register_builtin("%clp-bool-card!", 3, false,
            [&heap, bool_view, narrow_bool, walk_list](Args args)
                -> std::expected<LispVal, RuntimeError> {
                auto nl = classify_numeric(args[1], heap);
                auto nh = classify_numeric(args[2], heap);
                if (!nl.is_valid() || nl.is_flonum() || !nh.is_valid() || nh.is_flonum())
                    return std::unexpected(RuntimeError{VMError{
                        RuntimeErrorCode::TypeError,
                        "%clp-bool-card!: k-lo and k-hi must be integers"}});
                const int64_t k_lo = nl.int_val, k_hi = nh.int_val;
                std::vector<LispVal> xs;
                if (!walk_list(args[0], xs))
                    return std::unexpected(RuntimeError{VMError{
                        RuntimeErrorCode::TypeError,
                        "%clp-bool-card!: first argument must be a proper list"}});
                std::vector<BoolView> bvs;
                bvs.reserve(xs.size());
                for (auto e : xs) {
                    auto bv = bool_view(e);
                    if (!bv)
                        return std::unexpected(RuntimeError{VMError{
                            RuntimeErrorCode::TypeError,
                            "%clp-bool-card!: list elements must be booleans"}});
                    bvs.push_back(*bv);
                }
                int64_t forced_1 = 0, possible_1 = 0;
                for (const auto& bv : bvs) {
                    if (bv.mask == 2) ++forced_1;       ///< must-be-1
                    if (bv.mask & 2u) ++possible_1;     ///< could be 1
                }
                if (forced_1   > k_hi) return False;
                if (possible_1 < k_lo) return False;
                if (forced_1 == k_hi) {
                    /// Force every open var (mask == 3) to 0.
                    for (auto& bv : bvs) {
                        if (bv.mask == 3u && !narrow_bool(bv, 1u)) return False;
                    }
                }
                if (possible_1 == k_lo) {
                    /// Force every open var (mask == 3) to 1.
                    for (auto& bv : bvs) {
                        if (bv.mask == 3u && !narrow_bool(bv, 2u)) return False;
                    }
                }
                return True;
            });
    }

    /**
     * AD Tape primitives: tape-new tape-start! tape-stop! tape-var
     *                     tape-backward! tape-adjoint tape-primal
     *                     tape-ref? tape-ref-index tape-size
     *
     * These expose the tape-based (Wengert list) reverse-mode AD to Eta code.
     * Arithmetic +,-,*,/ and transcendentals sin,cos,exp,log,sqrt are
     * automatically recorded when a TapeRef operand is detected.
     */

    auto get_tape_arg = [&heap](LispVal v, const char* who, const char* role) -> std::expected<types::Tape*, RuntimeError> {
        if (!ops::is_boxed(v) || ops::tag(v) != Tag::HeapObject) {
            return std::unexpected(RuntimeError{VMError{
                RuntimeErrorCode::TypeError, std::string(who) + ": " + role + " must be a tape"}});
        }
        auto* tape = heap.try_get_as<ObjectKind::Tape, types::Tape>(ops::payload(v));
        if (!tape) {
            return std::unexpected(RuntimeError{VMError{
                RuntimeErrorCode::TypeError, std::string(who) + ": " + role + " must be a tape"}});
        }
        return tape;
    };

    auto ensure_tape_identity = [allocate_tape_id](types::Tape& tape) {
        if (tape.tape_id == 0) tape.tape_id = allocate_tape_id();
        tape.generation = types::tape_ref::normalize_generation(tape.generation);
    };

    env.register_builtin("tape-new", 0, false, [&heap, ensure_tape_identity](Args /*args*/) -> std::expected<LispVal, RuntimeError> {
        auto tv = make_tape(heap);
        if (!tv) return std::unexpected(tv.error());
        auto* tape = heap.try_get_as<ObjectKind::Tape, types::Tape>(ops::payload(*tv));
        if (!tape) {
            return std::unexpected(RuntimeError{VMError{
                RuntimeErrorCode::InternalError, "tape-new: failed to allocate tape object"}});
        }
        ensure_tape_identity(*tape);
        return *tv;
    });

    env.register_builtin("tape-start!", 1, false, [get_tape_arg, ensure_tape_identity, vm](Args args) -> std::expected<LispVal, RuntimeError> {
        if (!vm) {
            return std::unexpected(RuntimeError{VMError{
                RuntimeErrorCode::TypeError, "tape-start!: requires a running VM"}});
        }
        auto tape = get_tape_arg(args[0], "tape-start!", "argument");
        if (!tape) return std::unexpected(tape.error());
        ensure_tape_identity(**tape);
        vm->push_active_tape(args[0]);
        return True;
    });

    env.register_builtin("tape-stop!", 0, false, [vm](Args /*args*/) -> std::expected<LispVal, RuntimeError> {
        if (!vm) {
            return std::unexpected(RuntimeError{VMError{
                RuntimeErrorCode::TypeError, "tape-stop!: requires a running VM"}});
        }
        vm->pop_active_tape();
        return True;
    });

    env.register_builtin("tape-clear!", 1, false, [get_tape_arg](Args args) -> std::expected<LispVal, RuntimeError> {
        auto tape = get_tape_arg(args[0], "tape-clear!", "argument");
        if (!tape) return std::unexpected(tape.error());
        (*tape)->clear_and_bump_generation();
        return True;
    });

    env.register_builtin("tape-var", 2, false,
        [&heap, get_tape_arg, ensure_tape_identity](Args args) -> std::expected<LispVal, RuntimeError> {
            auto tape = get_tape_arg(args[0], "tape-var", "first argument");
            if (!tape) return std::unexpected(tape.error());
            ensure_tape_identity(**tape);
            auto n = classify_numeric(args[1], heap);
            if (!n.is_valid()) {
                return std::unexpected(RuntimeError{VMError{
                    RuntimeErrorCode::TypeError, "tape-var: second argument must be a number"}});
            }
            uint32_t idx = (*tape)->push_var(n.as_double());
            if (idx > types::tape_ref::MAX_NODE_INDEX) {
                return std::unexpected(RuntimeError{VMError{
                    RuntimeErrorCode::InternalError,
                    "tape-var: tape node index exceeds TapeRef capacity"}});
            }
            return types::tape_ref::make((*tape)->tape_id, (*tape)->generation, idx);
        });

    env.register_builtin("tape-backward!", 2, false,
        [get_tape_arg, validate_ref_for_tape](Args args) -> std::expected<LispVal, RuntimeError> {
            auto tape = get_tape_arg(args[0], "tape-backward!", "first argument");
            if (!tape) return std::unexpected(tape.error());
            if (!types::tape_ref::is_tape_ref(args[1])) {
                return std::unexpected(RuntimeError{VMError{
                    RuntimeErrorCode::TypeError, "tape-backward!: second argument must be a tape-ref"}});
            }
            auto output_idx = validate_ref_for_tape(*tape, args[1], "tape-backward!", "output-ref");
            if (!output_idx) return std::unexpected(output_idx.error());
            (*tape)->backward(*output_idx);
            return True;
        });

    env.register_builtin("tape-adjoint", 2, false,
        [get_tape_arg, validate_ref_for_tape](Args args) -> std::expected<LispVal, RuntimeError> {
            auto tape = get_tape_arg(args[0], "tape-adjoint", "first argument");
            if (!tape) return std::unexpected(tape.error());
            if (!types::tape_ref::is_tape_ref(args[1])) {
                return std::unexpected(RuntimeError{VMError{
                    RuntimeErrorCode::TypeError, "tape-adjoint: second argument must be a tape-ref"}});
            }
            auto idx = validate_ref_for_tape(*tape, args[1], "tape-adjoint", "ref");
            if (!idx) return std::unexpected(idx.error());
            return make_flonum((*tape)->entries[*idx].adjoint);
        });

    env.register_builtin("tape-primal", 2, false,
        [get_tape_arg, validate_ref_for_tape](Args args) -> std::expected<LispVal, RuntimeError> {
            auto tape = get_tape_arg(args[0], "tape-primal", "first argument");
            if (!tape) return std::unexpected(tape.error());
            if (!types::tape_ref::is_tape_ref(args[1])) {
                return std::unexpected(RuntimeError{VMError{
                    RuntimeErrorCode::TypeError, "tape-primal: second argument must be a tape-ref"}});
            }
            auto idx = validate_ref_for_tape(*tape, args[1], "tape-primal", "ref");
            if (!idx) return std::unexpected(idx.error());
            return make_flonum((*tape)->entries[*idx].primal);
        });

    env.register_builtin("tape-ref?", 1, false, [](Args args) -> std::expected<LispVal, RuntimeError> {
        return types::tape_ref::is_tape_ref(args[0]) ? True : False;
    });

    env.register_builtin("tape-ref-index", 1, false, [](Args args) -> std::expected<LispVal, RuntimeError> {
        if (!types::tape_ref::is_tape_ref(args[0])) {
            return std::unexpected(RuntimeError{VMError{
                RuntimeErrorCode::TypeError, "tape-ref-index: argument must be a tape-ref"}});
        }
        const auto parts = types::tape_ref::decode(args[0]);
        return ops::encode(static_cast<int64_t>(parts.node_index));
    });

    env.register_builtin("tape-size", 1, false, [get_tape_arg](Args args) -> std::expected<LispVal, RuntimeError> {
        auto tape = get_tape_arg(args[0], "tape-size", "argument");
        if (!tape) return std::unexpected(tape.error());
        return ops::encode(static_cast<int64_t>((*tape)->entries.size()));
    });

    env.register_builtin("tape-ref-value-of", 2, false,
        [get_tape_arg, validate_ref_for_tape](Args args) -> std::expected<LispVal, RuntimeError> {
            auto tape = get_tape_arg(args[0], "tape-ref-value-of", "first argument");
            if (!tape) return std::unexpected(tape.error());
            if (!types::tape_ref::is_tape_ref(args[1])) {
                return std::unexpected(RuntimeError{VMError{
                    RuntimeErrorCode::TypeError, "tape-ref-value-of: second argument must be a tape-ref"}});
            }
            auto idx = validate_ref_for_tape(*tape, args[1], "tape-ref-value-of", "ref");
            if (!idx) return std::unexpected(idx.error());
            return make_flonum((*tape)->entries[*idx].primal);
        });

    /**
     * tape-ref-value: extract the primal value of a TapeRef from the
     * current active tape. Non-TapeRef inputs remain pass-through.
     */
    env.register_builtin("tape-ref-value", 1, false,
        [&heap, vm, validate_ref_for_tape, make_ad_runtime_error](Args args) -> std::expected<LispVal, RuntimeError> {
            if (!types::tape_ref::is_tape_ref(args[0])) return args[0];
            if (!vm) {
                return std::unexpected(RuntimeError{VMError{
                    RuntimeErrorCode::TypeError, "tape-ref-value: requires a running VM"}});
            }
            auto active = vm->active_tape();
            auto* tape = (ops::is_boxed(active) && ops::tag(active) == Tag::HeapObject)
                ? heap.try_get_as<ObjectKind::Tape, types::Tape>(ops::payload(active))
                : nullptr;
            if (!tape) {
                return std::unexpected(make_ad_runtime_error(
                    ad::kTagNoActiveTape,
                    "tape-ref-value: no active tape",
                    {ad::field("op", std::string("tape-ref-value"))}));
            }
            auto idx = validate_ref_for_tape(tape, args[0], "tape-ref-value", "ref");
            if (!idx) return std::unexpected(idx.error());
            return make_flonum(tape->entries[*idx].primal);
        });

    /// Fact-table builtins

    /// Helper: extract FactTable* from a LispVal or return a type error.
    auto get_fact_table = [&heap](LispVal v, const char* who) -> std::expected<types::FactTable*, RuntimeError> {
        if (!ops::is_boxed(v) || ops::tag(v) != Tag::HeapObject)
            return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError, std::string(who) + ": argument must be a fact-table"}});
        auto* ft = heap.try_get_as<ObjectKind::FactTable, types::FactTable>(ops::payload(v));
        if (!ft)
            return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError, std::string(who) + ": argument must be a fact-table"}});
        return ft;
    };

    /**
     * @brief Decode a proper Eta list into a flat vector.
     *
     * Used by `%fact-table-insert!` and clause insertion builtins.
     */
    auto list_to_vector = [&heap](LispVal list, const char* who) -> std::expected<std::vector<LispVal>, RuntimeError> {
        std::vector<LispVal> out;
        LispVal cur = list;
        while (cur != Nil) {
            if (!ops::is_boxed(cur) || ops::tag(cur) != Tag::HeapObject)
                return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError, std::string(who) + ": expected a proper list"}});
            auto* cons = heap.try_get_as<ObjectKind::Cons, types::Cons>(ops::payload(cur));
            if (!cons)
                return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError, std::string(who) + ": expected a proper list"}});
            out.push_back(cons->car);
            cur = cons->cdr;
        }
        return out;
    };

    /**
     * @brief Encode a row-id vector as an Eta list of fixnums.
     */
    auto row_ids_to_list = [&heap](const std::vector<std::size_t>& rows,
                                   const char* who) -> std::expected<LispVal, RuntimeError> {
        LispVal result = Nil;
        for (auto it = rows.rbegin(); it != rows.rend(); ++it) {
            auto enc = ops::encode(static_cast<int64_t>(*it));
            if (!enc)
                return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError,
                    std::string(who) + ": row index too large"}});
            auto cell = make_cons(heap, *enc, result);
            if (!cell) return std::unexpected(cell.error());
            result = *cell;
        }
        return result;
    };

    /**
     * @brief Count live rows per distinct key in one column.
     *
     * Returns the groups in first-seen row order so output is deterministic.
     */
    auto group_count_rows = [](const types::FactTable& ft, std::size_t col)
        -> std::vector<std::pair<LispVal, std::size_t>> {
        std::vector<std::pair<LispVal, std::size_t>> out;
        if (col >= ft.columns.size()) return out;

        std::unordered_map<LispVal, std::size_t> slot_by_key;
        slot_by_key.reserve(ft.live_count);

        std::vector<LispVal> keys;
        std::vector<std::size_t> counts;
        keys.reserve(ft.live_count);
        counts.reserve(ft.live_count);

        const auto& group_col = ft.columns[col];
        for (std::size_t r = 0; r < ft.row_count; ++r) {
            if (ft.live_mask[r] == 0) continue;
            LispVal key = group_col[r];
            auto [it, inserted] = slot_by_key.emplace(key, counts.size());
            if (inserted) {
                keys.push_back(key);
                counts.push_back(1);
            } else {
                ++counts[it->second];
            }
        }

        out.reserve(keys.size());
        for (std::size_t i = 0; i < keys.size(); ++i) {
            out.emplace_back(keys[i], counts[i]);
        }
        return out;
    };

    /// fact-table predicates
    auto fact_table_predicate = [&heap](Args args) -> std::expected<LispVal, RuntimeError> {
        if (ops::is_boxed(args[0]) && ops::tag(args[0]) == Tag::HeapObject) {
            if (heap.try_get_as<ObjectKind::FactTable, types::FactTable>(ops::payload(args[0])))
                return True;
        }
        return False;
    };
    env.register_builtin("%fact-table?", 1, false, fact_table_predicate);
    env.register_builtin("fact-table?", 1, false, fact_table_predicate);

    ///   col-name-list is an Eta list of symbols or strings.
    env.register_builtin("%make-fact-table", 1, false, [&heap, &intern_table](Args args) -> std::expected<LispVal, RuntimeError> {
        /// Walk the Eta list and collect column names
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

    env.register_builtin("%fact-table-insert!", 2, false, [get_fact_table, list_to_vector](Args args) -> std::expected<LispVal, RuntimeError> {
        auto ft_res = get_fact_table(args[0], "%fact-table-insert!");
        if (!ft_res) return std::unexpected(ft_res.error());
        auto row_res = list_to_vector(args[1], "%fact-table-insert!: second arg");
        if (!row_res) return std::unexpected(row_res.error());
        if (!(*ft_res)->add_row(*row_res))
            return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError, "%fact-table-insert!: row arity mismatch"}});
        return True;
    });

    /**
     * Clause insert path.
     * (table row-list rule-or-false ground?)
     */
    env.register_builtin("%fact-table-insert-clause!", 4, false,
        [get_fact_table, list_to_vector](Args args) -> std::expected<LispVal, RuntimeError> {
            auto ft_res = get_fact_table(args[0], "%fact-table-insert-clause!");
            if (!ft_res) return std::unexpected(ft_res.error());
            auto row_res = list_to_vector(args[1], "%fact-table-insert-clause!: second arg");
            if (!row_res) return std::unexpected(row_res.error());
            if (args[3] != True && args[3] != False) {
                return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError,
                    "%fact-table-insert-clause!: fourth arg must be #t or #f (ground?)"}});
            }
            const bool is_ground = (args[3] == True);
            if (!(*ft_res)->add_row(*row_res, args[2], is_ground))
                return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError,
                    "%fact-table-insert-clause!: row arity mismatch"}});
            return True;
        });

    env.register_builtin("%fact-table-delete-row!", 2, false, [get_fact_table](Args args) -> std::expected<LispVal, RuntimeError> {
        auto ft_res = get_fact_table(args[0], "%fact-table-delete-row!");
        if (!ft_res) return std::unexpected(ft_res.error());
        auto row_opt = ops::decode<int64_t>(args[1]);
        if (!row_opt)
            return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError, "%fact-table-delete-row!: row index must be a fixnum"}});
        if (*row_opt < 0) return False;
        return (*ft_res)->delete_row(static_cast<std::size_t>(*row_opt)) ? True : False;
    });

    env.register_builtin("%fact-table-row-live?", 2, false, [get_fact_table](Args args) -> std::expected<LispVal, RuntimeError> {
        auto ft_res = get_fact_table(args[0], "%fact-table-row-live?");
        if (!ft_res) return std::unexpected(ft_res.error());
        auto row_opt = ops::decode<int64_t>(args[1]);
        if (!row_opt)
            return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError, "%fact-table-row-live?: row index must be a fixnum"}});
        if (*row_opt < 0) return False;
        return (*ft_res)->is_live_row(static_cast<std::size_t>(*row_opt)) ? True : False;
    });

    env.register_builtin("%fact-table-row-ground?", 2, false, [get_fact_table](Args args) -> std::expected<LispVal, RuntimeError> {
        auto ft_res = get_fact_table(args[0], "%fact-table-row-ground?");
        if (!ft_res) return std::unexpected(ft_res.error());
        auto row_opt = ops::decode<int64_t>(args[1]);
        if (!row_opt)
            return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError, "%fact-table-row-ground?: row index must be a fixnum"}});
        if (*row_opt < 0) return False;
        return (*ft_res)->is_ground_row(static_cast<std::size_t>(*row_opt)) ? True : False;
    });

    env.register_builtin("%fact-table-row-rule", 2, false, [get_fact_table](Args args) -> std::expected<LispVal, RuntimeError> {
        auto ft_res = get_fact_table(args[0], "%fact-table-row-rule");
        if (!ft_res) return std::unexpected(ft_res.error());
        auto row_opt = ops::decode<int64_t>(args[1]);
        if (!row_opt)
            return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError, "%fact-table-row-rule: row index must be a fixnum"}});
        if (*row_opt < 0) return False;
        return (*ft_res)->get_rule(static_cast<std::size_t>(*row_opt));
    });

    env.register_builtin("%fact-table-set-predicate!", 3, false,
        [get_fact_table](Args args) -> std::expected<LispVal, RuntimeError> {
            auto ft_res = get_fact_table(args[0], "%fact-table-set-predicate!");
            if (!ft_res) return std::unexpected(ft_res.error());
            if (!ops::is_boxed(args[1]) || ops::tag(args[1]) != Tag::Symbol)
                return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError,
                    "%fact-table-set-predicate!: second arg must be a symbol functor"}});
            auto arity_opt = ops::decode<int64_t>(args[2]);
            if (!arity_opt || *arity_opt < 0 || *arity_opt > 255)
                return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError,
                    "%fact-table-set-predicate!: third arg must be a fixnum in [0,255]"}});
            (*ft_res)->set_predicate_header(
                static_cast<std::uint64_t>(ops::payload(args[1])),
                static_cast<std::uint8_t>(*arity_opt));
            return True;
        });

    env.register_builtin("%fact-table-predicate", 1, false, [&heap, get_fact_table](Args args) -> std::expected<LispVal, RuntimeError> {
        auto ft_res = get_fact_table(args[0], "%fact-table-predicate");
        if (!ft_res) return std::unexpected(ft_res.error());
        const auto& ft = **ft_res;
        if (!ft.predicate_functor.has_value()) return False;
        const LispVal sym = ops::box(Tag::Symbol, *ft.predicate_functor);
        auto ar = ops::encode(static_cast<int64_t>(ft.predicate_arity));
        if (!ar) return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError, "%fact-table-predicate: arity encoding failed"}});
        auto tail = make_cons(heap, *ar, Nil);
        if (!tail) return std::unexpected(tail.error());
        auto head = make_cons(heap, sym, *tail);
        if (!head) return std::unexpected(head.error());
        return *head;
    });

    env.register_builtin("%fact-table-build-index!", 2, false, [get_fact_table](Args args) -> std::expected<LispVal, RuntimeError> {
        auto ft_res = get_fact_table(args[0], "%fact-table-build-index!");
        if (!ft_res) return std::unexpected(ft_res.error());
        auto col_opt = ops::decode<int64_t>(args[1]);
        if (!col_opt)
            return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError, "%fact-table-build-index!: column index must be a fixnum"}});
        (*ft_res)->build_index(static_cast<std::size_t>(*col_opt));
        return True;
    });

    env.register_builtin("%fact-table-query", 3, false, [get_fact_table, row_ids_to_list](Args args) -> std::expected<LispVal, RuntimeError> {
        auto ft_res = get_fact_table(args[0], "%fact-table-query");
        if (!ft_res) return std::unexpected(ft_res.error());
        auto col_opt = ops::decode<int64_t>(args[1]);
        if (!col_opt)
            return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError, "%fact-table-query: column index must be a fixnum"}});
        auto rows = (*ft_res)->query(static_cast<std::size_t>(*col_opt), args[2]);
        return row_ids_to_list(rows, "%fact-table-query");
    });

    /**
     * (%fact-table-group-count table group-col-idx)
     *
     * Returns an alist of dotted pairs: ((key . count) ...).
     */
    env.register_builtin("%fact-table-group-count", 2, false,
        [&heap, get_fact_table, group_count_rows](Args args) -> std::expected<LispVal, RuntimeError> {
            auto ft_res = get_fact_table(args[0], "%fact-table-group-count");
            if (!ft_res) return std::unexpected(ft_res.error());
            auto col_opt = ops::decode<int64_t>(args[1]);
            if (!col_opt)
                return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError,
                    "%fact-table-group-count: column index must be a fixnum"}});

            auto grouped = group_count_rows(**ft_res, static_cast<std::size_t>(*col_opt));

            auto roots = heap.make_external_root_frame();
            LispVal result = Nil;
            roots.push(result);

            for (auto it = grouped.rbegin(); it != grouped.rend(); ++it) {
                if (it->second > static_cast<std::size_t>(std::numeric_limits<int64_t>::max())) {
                    return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError,
                        "%fact-table-group-count: group count too large"}});
                }

                auto count_val = make_fixnum(heap, static_cast<int64_t>(it->second));
                if (!count_val) return std::unexpected(count_val.error());
                roots.push(*count_val);

                auto pair_val = make_cons(heap, it->first, *count_val);
                if (!pair_val) return std::unexpected(pair_val.error());
                roots.push(*pair_val);

                auto cell = make_cons(heap, *pair_val, result);
                if (!cell) return std::unexpected(cell.error());
                result = *cell;
                roots.push(result);
            }

            return result;
        });

    /**
     * (%fact-table-group-sum table group-col-idx value-col-idx)
     *
     * Returns an alist of dotted pairs: ((key . sum) ...). The sum preserves
     * fixnum accumulation until overflow or flonum input forces promotion.
     */
    env.register_builtin("%fact-table-group-sum", 3, false,
        [&heap, get_fact_table](Args args) -> std::expected<LispVal, RuntimeError> {
            auto ft_res = get_fact_table(args[0], "%fact-table-group-sum");
            if (!ft_res) return std::unexpected(ft_res.error());
            auto group_col_opt = ops::decode<int64_t>(args[1]);
            if (!group_col_opt)
                return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError,
                    "%fact-table-group-sum: group column index must be a fixnum"}});
            auto value_col_opt = ops::decode<int64_t>(args[2]);
            if (!value_col_opt)
                return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError,
                    "%fact-table-group-sum: value column index must be a fixnum"}});

            const auto group_col = static_cast<std::size_t>(*group_col_opt);
            const auto value_col = static_cast<std::size_t>(*value_col_opt);
            auto& ft = **ft_res;
            if (group_col >= ft.columns.size() || value_col >= ft.columns.size()) return Nil;

            struct SumState {
                bool use_float{false};
                int64_t isum{0};
                double fsum{0.0};
            };

            std::unordered_map<LispVal, std::size_t> slot_by_key;
            slot_by_key.reserve(ft.live_count);

            std::vector<LispVal> keys;
            std::vector<SumState> sums;
            keys.reserve(ft.live_count);
            sums.reserve(ft.live_count);

            const auto& group_values = ft.columns[group_col];
            const auto& sum_values = ft.columns[value_col];
            for (std::size_t r = 0; r < ft.row_count; ++r) {
                if (ft.live_mask[r] == 0) continue;

                LispVal key = group_values[r];
                auto n = classify_numeric(sum_values[r], heap);
                if (!n.is_valid()) {
                    return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError,
                        "%fact-table-group-sum: value column contains non-numeric data"}});
                }

                auto [it, inserted] = slot_by_key.emplace(key, sums.size());
                if (inserted) {
                    keys.push_back(key);
                    SumState st;
                    st.use_float = n.is_flonum();
                    if (st.use_float) {
                        st.fsum = n.as_double();
                    } else {
                        st.isum = n.int_val;
                    }
                    sums.push_back(st);
                    continue;
                }

                auto& st = sums[it->second];
                if (n.is_flonum() || st.use_float) {
                    if (!st.use_float) {
                        st.fsum = static_cast<double>(st.isum);
                        st.use_float = true;
                    }
                    st.fsum += n.as_double();
                } else {
                    int64_t next = 0;
                    if (detail::add_overflow(st.isum, n.int_val, &next)) {
                        st.use_float = true;
                        st.fsum = static_cast<double>(st.isum) + static_cast<double>(n.int_val);
                    } else {
                        st.isum = next;
                    }
                }
            }

            auto roots = heap.make_external_root_frame();
            LispVal result = Nil;
            roots.push(result);

            for (std::size_t i = keys.size(); i > 0; --i) {
                const std::size_t idx = i - 1;

                std::expected<LispVal, RuntimeError> sum_val =
                    sums[idx].use_float
                        ? make_flonum(sums[idx].fsum)
                        : make_fixnum(heap, sums[idx].isum);
                if (!sum_val) return std::unexpected(sum_val.error());
                roots.push(*sum_val);

                auto pair_val = make_cons(heap, keys[idx], *sum_val);
                if (!pair_val) return std::unexpected(pair_val.error());
                roots.push(*pair_val);

                auto cell = make_cons(heap, *pair_val, result);
                if (!cell) return std::unexpected(cell.error());
                result = *cell;
                roots.push(result);
            }

            return result;
        });

    env.register_builtin("%fact-table-live-row-ids", 1, false, [get_fact_table, row_ids_to_list](Args args) -> std::expected<LispVal, RuntimeError> {
        auto ft_res = get_fact_table(args[0], "%fact-table-live-row-ids");
        if (!ft_res) return std::unexpected(ft_res.error());
        return row_ids_to_list((*ft_res)->live_rows(), "%fact-table-live-row-ids");
    });

    env.register_builtin("%fact-table-ref", 3, false, [get_fact_table](Args args) -> std::expected<LispVal, RuntimeError> {
        auto ft_res = get_fact_table(args[0], "%fact-table-ref");
        if (!ft_res) return std::unexpected(ft_res.error());
        auto row_opt = ops::decode<int64_t>(args[1]);
        auto col_opt = ops::decode<int64_t>(args[2]);
        if (!row_opt || !col_opt)
            return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError, "%fact-table-ref: indices must be fixnums"}});
        if (*row_opt < 0 || *col_opt < 0) return Nil;
        return (*ft_res)->get_cell(static_cast<std::size_t>(*row_opt), static_cast<std::size_t>(*col_opt));
    });

    env.register_builtin("%fact-table-row-count", 1, false, [get_fact_table](Args args) -> std::expected<LispVal, RuntimeError> {
        auto ft_res = get_fact_table(args[0], "%fact-table-row-count");
        if (!ft_res) return std::unexpected(ft_res.error());
        return ops::encode(static_cast<int64_t>((*ft_res)->active_row_count()));
    });

    /**
     * (%fact-table-column-names table)
     *
     * Returns the declared column names as a list of symbols.
     */
    env.register_builtin("%fact-table-column-names", 1, false, [&heap, &intern_table, get_fact_table](Args args) -> std::expected<LispVal, RuntimeError> {
        auto ft_res = get_fact_table(args[0], "%fact-table-column-names");
        if (!ft_res) return std::unexpected(ft_res.error());

        auto roots = heap.make_external_root_frame();
        LispVal result = Nil;
        roots.push(result);

        const auto& names = (*ft_res)->col_names;
        for (auto it = names.rbegin(); it != names.rend(); ++it) {
            auto sid = intern_table.intern(*it);
            if (!sid) {
                return std::unexpected(RuntimeError{VMError{
                    RuntimeErrorCode::TypeError,
                    "%fact-table-column-names: intern failed"}});
            }
            const LispVal sym = ops::box(Tag::Symbol, *sid);

            auto cell = make_cons(heap, sym, result);
            if (!cell) return std::unexpected(cell.error());
            result = *cell;
            roots.push(result);
        }
        return result;
    });

    /**
     * (term-hash term depth)
     *
     * Computes a depth-limited structural hash used by relation
     * indexing/tabling helpers. Cycles are handled by depth truncation.
     */
    auto mix_hash = [](std::uint64_t seed, std::uint64_t value) -> std::uint64_t {
        constexpr std::uint64_t kMul = 0x9E3779B97F4A7C15ULL;
        seed ^= value + kMul + (seed << 6) + (seed >> 2);
        return seed;
    };

    auto term_hash_impl = [&heap, mix_hash](auto&& self, LispVal v, int depth) -> std::uint64_t {
        if (depth <= 0) {
            return mix_hash(0x6A09E667F3BCC909ULL, static_cast<std::uint64_t>(v));
        }

        if (v == Nil)   return 0xA54FF53A5F1D36F1ULL;
        if (v == True)  return 0x510E527FADE682D1ULL;
        if (v == False) return 0x9B05688C2B3E6C1FULL;

        if (!ops::is_boxed(v)) {
            return mix_hash(0x1F83D9ABFB41BD6BULL, static_cast<std::uint64_t>(v));
        }

        const Tag t = ops::tag(v);
        std::uint64_t h = mix_hash(0x5BE0CD19137E2179ULL, static_cast<std::uint64_t>(t));

        if (t == Tag::Fixnum) {
            auto x = ops::decode<int64_t>(v).value_or(0);
            return mix_hash(h, static_cast<std::uint64_t>(x));
        }
        if (t == Tag::Char) {
            auto x = ops::decode<char32_t>(v).value_or(U'\0');
            return mix_hash(h, static_cast<std::uint64_t>(x));
        }
        if (t == Tag::String || t == Tag::Symbol || t == Tag::TapeRef) {
            return mix_hash(h, static_cast<std::uint64_t>(ops::payload(v)));
        }
        if (t != Tag::HeapObject) {
            return mix_hash(h, static_cast<std::uint64_t>(ops::payload(v)));
        }

        const auto id = ops::payload(v);

        auto num = classify_numeric(v, heap);
        if (num.is_fixnum()) return mix_hash(h, static_cast<std::uint64_t>(num.int_val));
        if (num.is_flonum()) return mix_hash(h, static_cast<std::uint64_t>(std::bit_cast<std::uint64_t>(num.float_val)));

        if (auto* cons = heap.try_get_as<ObjectKind::Cons, types::Cons>(id)) {
            h = mix_hash(h, 0xC1059ED8U);
            h = mix_hash(h, self(self, cons->car, depth - 1));
            h = mix_hash(h, self(self, cons->cdr, depth - 1));
            return h;
        }
        if (auto* vec = heap.try_get_as<ObjectKind::Vector, types::Vector>(id)) {
            h = mix_hash(h, 0x1EAFBEEF);
            h = mix_hash(h, static_cast<std::uint64_t>(vec->elements.size()));
            for (auto e : vec->elements) h = mix_hash(h, self(self, e, depth - 1));
            return h;
        }
        if (auto* ct = heap.try_get_as<ObjectKind::CompoundTerm, types::CompoundTerm>(id)) {
            h = mix_hash(h, 0xCCAA5511U);
            h = mix_hash(h, self(self, ct->functor, depth - 1));
            h = mix_hash(h, static_cast<std::uint64_t>(ct->args.size()));
            for (auto a : ct->args) h = mix_hash(h, self(self, a, depth - 1));
            return h;
        }
        if (auto* lv = heap.try_get_as<ObjectKind::LogicVar, types::LogicVar>(id)) {
            h = mix_hash(h, 0xBADC0DEULL);
            if (lv->binding.has_value())
                return mix_hash(h, self(self, *lv->binding, depth - 1));
            return mix_hash(h, static_cast<std::uint64_t>(id));
        }
        if (auto* ft = heap.try_get_as<ObjectKind::FactTable, types::FactTable>(id)) {
            h = mix_hash(h, 0xFA17AB1EULL);
            h = mix_hash(h, static_cast<std::uint64_t>(ft->active_row_count()));
            h = mix_hash(h, static_cast<std::uint64_t>(ft->col_names.size()));
            if (ft->predicate_functor.has_value()) {
                h = mix_hash(h, *ft->predicate_functor);
                h = mix_hash(h, static_cast<std::uint64_t>(ft->predicate_arity));
            }
            return h;
        }
        if (auto* rx = heap.try_get_as<ObjectKind::Regex, types::Regex>(id)) {
            h = mix_hash(h, 0x9E97A8B1ULL);
            h = mix_hash(h, std::hash<std::string>{}(rx->pattern));
            h = mix_hash(h, static_cast<std::uint64_t>(rx->flags));
            return h;
        }
        return mix_hash(h, static_cast<std::uint64_t>(id));
    };

    env.register_builtin("term-hash", 2, false, [&heap, term_hash_impl](Args args) -> std::expected<LispVal, RuntimeError> {
        auto depth_opt = ops::decode<int64_t>(args[1]);
        if (!depth_opt || *depth_opt < 0)
            return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError,
                "term-hash: second arg must be a non-negative fixnum depth"}});
        auto h = term_hash_impl(term_hash_impl, args[0], static_cast<int>(*depth_opt));
        constexpr std::uint64_t kMask = (1ULL << 46) - 1ULL; ///< always fixnum-encodable.
        const auto narrowed = static_cast<int64_t>(h & kMask);
        auto enc = ops::encode(narrowed);
        if (enc) return *enc;
        return make_fixnum(heap, narrowed);
    });

    /**
     * (term-variant-hash term depth)
     *
     * Like `term-hash`, but unbound logic variables are normalized by first
     * occurrence order rather than by raw object id.  This gives stable keys
     * across alpha-renamed call patterns, which is required by tabling.
     */
    auto term_variant_hash_impl =
        [&heap, mix_hash](auto&& self, LispVal v, int depth,
                          std::unordered_map<memory::heap::ObjectId, std::uint64_t>& lvar_slots,
                          std::uint64_t& next_lvar_slot) -> std::uint64_t {
            if (depth <= 0) {
                return mix_hash(0x6A09E667F3BCC909ULL, static_cast<std::uint64_t>(v));
            }

            if (v == Nil)   return 0xA54FF53A5F1D36F1ULL;
            if (v == True)  return 0x510E527FADE682D1ULL;
            if (v == False) return 0x9B05688C2B3E6C1FULL;

            if (!ops::is_boxed(v)) {
                return mix_hash(0x1F83D9ABFB41BD6BULL, static_cast<std::uint64_t>(v));
            }

            const Tag t = ops::tag(v);
            std::uint64_t h = mix_hash(0x5BE0CD19137E2179ULL, static_cast<std::uint64_t>(t));

            if (t == Tag::Fixnum) {
                auto x = ops::decode<int64_t>(v).value_or(0);
                return mix_hash(h, static_cast<std::uint64_t>(x));
            }
            if (t == Tag::Char) {
                auto x = ops::decode<char32_t>(v).value_or(U'\0');
                return mix_hash(h, static_cast<std::uint64_t>(x));
            }
            if (t == Tag::String || t == Tag::Symbol || t == Tag::TapeRef) {
                return mix_hash(h, static_cast<std::uint64_t>(ops::payload(v)));
            }
            if (t != Tag::HeapObject) {
                return mix_hash(h, static_cast<std::uint64_t>(ops::payload(v)));
            }

            const auto id = static_cast<memory::heap::ObjectId>(ops::payload(v));

            auto num = classify_numeric(v, heap);
            if (num.is_fixnum()) return mix_hash(h, static_cast<std::uint64_t>(num.int_val));
            if (num.is_flonum()) return mix_hash(h, static_cast<std::uint64_t>(std::bit_cast<std::uint64_t>(num.float_val)));

            if (auto* cons = heap.try_get_as<ObjectKind::Cons, types::Cons>(id)) {
                h = mix_hash(h, 0xC1059ED8U);
                h = mix_hash(h, self(self, cons->car, depth - 1, lvar_slots, next_lvar_slot));
                h = mix_hash(h, self(self, cons->cdr, depth - 1, lvar_slots, next_lvar_slot));
                return h;
            }
            if (auto* vec = heap.try_get_as<ObjectKind::Vector, types::Vector>(id)) {
                h = mix_hash(h, 0x1EAFBEEF);
                h = mix_hash(h, static_cast<std::uint64_t>(vec->elements.size()));
                for (auto e : vec->elements)
                    h = mix_hash(h, self(self, e, depth - 1, lvar_slots, next_lvar_slot));
                return h;
            }
            if (auto* ct = heap.try_get_as<ObjectKind::CompoundTerm, types::CompoundTerm>(id)) {
                h = mix_hash(h, 0xCCAA5511U);
                h = mix_hash(h, self(self, ct->functor, depth - 1, lvar_slots, next_lvar_slot));
                h = mix_hash(h, static_cast<std::uint64_t>(ct->args.size()));
                for (auto a : ct->args)
                    h = mix_hash(h, self(self, a, depth - 1, lvar_slots, next_lvar_slot));
                return h;
            }
            if (auto* lv = heap.try_get_as<ObjectKind::LogicVar, types::LogicVar>(id)) {
                h = mix_hash(h, 0xBADC0DEULL);
                if (lv->binding.has_value()) {
                    return mix_hash(h, self(self, *lv->binding, depth - 1, lvar_slots, next_lvar_slot));
                }
                auto it = lvar_slots.find(id);
                if (it == lvar_slots.end()) {
                    it = lvar_slots.emplace(id, next_lvar_slot++).first;
                }
                return mix_hash(h, it->second);
            }
            if (auto* ft = heap.try_get_as<ObjectKind::FactTable, types::FactTable>(id)) {
                h = mix_hash(h, 0xFA17AB1EULL);
                h = mix_hash(h, static_cast<std::uint64_t>(ft->active_row_count()));
                h = mix_hash(h, static_cast<std::uint64_t>(ft->col_names.size()));
                if (ft->predicate_functor.has_value()) {
                    h = mix_hash(h, *ft->predicate_functor);
                    h = mix_hash(h, static_cast<std::uint64_t>(ft->predicate_arity));
                }
                return h;
            }
            if (auto* rx = heap.try_get_as<ObjectKind::Regex, types::Regex>(id)) {
                h = mix_hash(h, 0x9E97A8B1ULL);
                h = mix_hash(h, std::hash<std::string>{}(rx->pattern));
                h = mix_hash(h, static_cast<std::uint64_t>(rx->flags));
                return h;
            }
            return mix_hash(h, static_cast<std::uint64_t>(id));
        };

    env.register_builtin("term-variant-hash", 2, false, [&heap, term_variant_hash_impl](Args args) -> std::expected<LispVal, RuntimeError> {
        auto depth_opt = ops::decode<int64_t>(args[1]);
        if (!depth_opt || *depth_opt < 0)
            return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError,
                "term-variant-hash: second arg must be a non-negative fixnum depth"}});
        std::unordered_map<memory::heap::ObjectId, std::uint64_t> lvar_slots;
        lvar_slots.reserve(32);
        std::uint64_t next_lvar_slot = 1;
        auto h = term_variant_hash_impl(
            term_variant_hash_impl, args[0], static_cast<int>(*depth_opt), lvar_slots, next_lvar_slot);
        constexpr std::uint64_t kMask = (1ULL << 46) - 1ULL; ///< always fixnum-encodable.
        const auto narrowed = static_cast<int64_t>(h & kMask);
        auto enc = ops::encode(narrowed);
        if (enc) return *enc;
        return make_fixnum(heap, narrowed);
    });

    /**
     * Statistics builtins (stats_math.h + stats_extract.h)
     *
     * All %stats-* primitives accept any numeric sequence (list, vector,
     * or fact-table column) via the polymorphic stats::to_eigen() helper
     * and return numeric results.  They provide the foundation for
     * std.stats.
     */

    env.register_builtin("%stats-mean", 1, false, [&heap](Args args) -> std::expected<LispVal, RuntimeError> {
        auto xs = stats::to_eigen(heap, args[0], "%stats-mean");
        if (!xs) return std::unexpected(xs.error());
        if (xs->size() == 0) return make_flonum(0.0);
        return make_flonum(stats::mean(*xs));
    });

    env.register_builtin("%stats-variance", 1, false, [&heap](Args args) -> std::expected<LispVal, RuntimeError> {
        auto xs = stats::to_eigen(heap, args[0], "%stats-variance");
        if (!xs) return std::unexpected(xs.error());
        if (xs->size() < 2)
            return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError, "%stats-variance: need at least 2 elements"}});
        return make_flonum(stats::variance(*xs));
    });

    env.register_builtin("%stats-stddev", 1, false, [&heap](Args args) -> std::expected<LispVal, RuntimeError> {
        auto xs = stats::to_eigen(heap, args[0], "%stats-stddev");
        if (!xs) return std::unexpected(xs.error());
        if (xs->size() < 2)
            return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError, "%stats-stddev: need at least 2 elements"}});
        return make_flonum(stats::stddev(*xs));
    });

    env.register_builtin("%stats-sem", 1, false, [&heap](Args args) -> std::expected<LispVal, RuntimeError> {
        auto xs = stats::to_eigen(heap, args[0], "%stats-sem");
        if (!xs) return std::unexpected(xs.error());
        if (xs->size() < 2)
            return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError, "%stats-sem: need at least 2 elements"}});
        return make_flonum(stats::sem(*xs));
    });

    env.register_builtin("%stats-percentile", 2, false, [&heap](Args args) -> std::expected<LispVal, RuntimeError> {
        auto xs = stats::to_eigen(heap, args[0], "%stats-percentile");
        if (!xs) return std::unexpected(xs.error());
        auto pv = classify_numeric(args[1], heap);
        if (!pv.is_valid())
            return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError, "%stats-percentile: p must be a number"}});
        return make_flonum(stats::percentile(std::move(*xs), pv.as_double()));
    });

    env.register_builtin("%stats-covariance", 2, false, [&heap](Args args) -> std::expected<LispVal, RuntimeError> {
        auto xs = stats::to_eigen(heap, args[0], "%stats-covariance");
        if (!xs) return std::unexpected(xs.error());
        auto ys = stats::to_eigen(heap, args[1], "%stats-covariance");
        if (!ys) return std::unexpected(ys.error());
        auto r = stats::covariance(*xs, *ys);
        if (!r) return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError, "%stats-covariance: sequences must be same length (>=2)"}});
        return make_flonum(*r);
    });

    env.register_builtin("%stats-correlation", 2, false, [&heap](Args args) -> std::expected<LispVal, RuntimeError> {
        auto xs = stats::to_eigen(heap, args[0], "%stats-correlation");
        if (!xs) return std::unexpected(xs.error());
        auto ys = stats::to_eigen(heap, args[1], "%stats-correlation");
        if (!ys) return std::unexpected(ys.error());
        auto r = stats::correlation(*xs, *ys);
        if (!r) return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError, "%stats-correlation: sequences must be same length (>=2), non-constant"}});
        return make_flonum(*r);
    });

    env.register_builtin("%stats-t-cdf", 2, false, [&heap](Args args) -> std::expected<LispVal, RuntimeError> {
        auto tv = classify_numeric(args[0], heap);
        auto dv = classify_numeric(args[1], heap);
        if (!tv.is_valid() || !dv.is_valid())
            return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError, "%stats-t-cdf: arguments must be numbers"}});
        return make_flonum(stats::t_cdf(tv.as_double(), dv.as_double()));
    });

    env.register_builtin("%stats-t-quantile", 2, false, [&heap](Args args) -> std::expected<LispVal, RuntimeError> {
        auto pv = classify_numeric(args[0], heap);
        auto dv = classify_numeric(args[1], heap);
        if (!pv.is_valid() || !dv.is_valid())
            return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError, "%stats-t-quantile: arguments must be numbers"}});
        return make_flonum(stats::t_quantile(pv.as_double(), dv.as_double()));
    });

    env.register_builtin("%stats-normal-quantile", 1, false, [&heap](Args args) -> std::expected<LispVal, RuntimeError> {
        auto pv = classify_numeric(args[0], heap);
        if (!pv.is_valid())
            return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError, "%stats-normal-quantile: argument must be a number"}});
        return make_flonum(stats::normal_quantile(pv.as_double()));
    });

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

    env.register_builtin("%stats-t-test-2", 2, false, [&heap](Args args) -> std::expected<LispVal, RuntimeError> {
        auto xs = stats::to_eigen(heap, args[0], "%stats-t-test-2");
        if (!xs) return std::unexpected(xs.error());
        auto ys = stats::to_eigen(heap, args[1], "%stats-t-test-2");
        if (!ys) return std::unexpected(ys.error());
        auto r = stats::t_test_2(*xs, *ys);
        if (!r) return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError, "%stats-t-test-2: each sequence must have >=2 elements"}});

        /// Build result list: (t-stat p-value df mean-diff)
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

    env.register_builtin("%stats-ols", 2, false, [&heap](Args args) -> std::expected<LispVal, RuntimeError> {
        auto xs = stats::to_eigen(heap, args[0], "%stats-ols");
        if (!xs) return std::unexpected(xs.error());
        auto ys = stats::to_eigen(heap, args[1], "%stats-ols");
        if (!ys) return std::unexpected(ys.error());
        auto r = stats::ols(*xs, *ys);
        if (!r) return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError, "%stats-ols: sequences must be same length (>=3)"}});

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

    /**
     * (register-prop-attr! 'key)
     *   Marks attribute key 'key as carrying a list of re-propagator thunks.
     *   When `unify` later binds a logic var carrying this attribute, every
     *   thunk in the attribute's value (a list) is *enqueued* on the VM's
     *   FIFO propagation queue rather than invoked synchronously.  The queue
     *   drains at the outer-`unify` boundary; thunk return values of #f
     *   abort the unify and trigger the standard atomic rollback.
     *
     * (%clp-prop-queue-size)
     *   diagnostics; always 0 outside an active unify.
     */

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

} ///< namespace eta::runtime

