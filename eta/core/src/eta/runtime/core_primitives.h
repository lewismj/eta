 #pragma once

#include <cmath>
#include <functional>
#include <sstream>
#include <string>

#include "eta/runtime/builtin_env.h"
#include "eta/runtime/numeric_value.h"
#include "eta/runtime/overflow.h"
#include "eta/runtime/factory.h"
#include "eta/runtime/string_view.h"
#include "eta/runtime/value_formatter.h"

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
 *  Higher-order: apply  map  for-each  (apply is VM-level; map/for-each primitives-only)
 *  Equality:     equal?
 *  Strings:      string-length  string-append  number->string  string->number
 *  Vectors:      vector  vector-length  vector-ref  vector-set!  vector?  make-vector
 *  Error:        error
 *  Platform:     platform
 *
 * Note: I/O primitives (display, write, newline) are in io_primitives.h
 * and require VM access for port support.
 *
 * All primitives capture Heap& and/or InternTable& by reference where needed.
 */
inline void register_core_primitives(BuiltinEnvironment& env, Heap& heap, InternTable& intern_table) {
    using Args = const std::vector<LispVal>&;

    // ========================================================================
    // Arithmetic: + - * /
    // ========================================================================

    env.register_builtin("+", 0, true, [&heap](Args args) -> std::expected<LispVal, RuntimeError> {
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

    env.register_builtin("-", 1, true, [&heap](Args args) -> std::expected<LispVal, RuntimeError> {
        if (args.empty()) return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::InvalidArity, "-: requires at least 1 argument"}});
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

    env.register_builtin("*", 0, true, [&heap](Args args) -> std::expected<LispVal, RuntimeError> {
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

    env.register_builtin("/", 1, true, [&heap](Args args) -> std::expected<LispVal, RuntimeError> {
        if (args.empty()) return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::InvalidArity, "/: requires at least 1 argument"}});
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
        if (!cons) return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError, "car: not a pair"}});
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
        for (auto it = args.rbegin(); it != args.rend(); ++it) {
            auto cons = make_cons(heap, *it, result);
            if (!cons) return cons;
            result = *cons;
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

    env.register_builtin("sin", 1, false, [&heap](Args args) -> std::expected<LispVal, RuntimeError> {
        auto n = classify_numeric(args[0], heap);
        if (!n.is_valid()) return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError, "sin: argument is not a number"}});
        return make_flonum(std::sin(n.as_double()));
    });

    env.register_builtin("cos", 1, false, [&heap](Args args) -> std::expected<LispVal, RuntimeError> {
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

    env.register_builtin("exp", 1, false, [&heap](Args args) -> std::expected<LispVal, RuntimeError> {
        auto n = classify_numeric(args[0], heap);
        if (!n.is_valid()) return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError, "exp: argument is not a number"}});
        return make_flonum(std::exp(n.as_double()));
    });

    env.register_builtin("log", 1, false, [&heap](Args args) -> std::expected<LispVal, RuntimeError> {
        auto n = classify_numeric(args[0], heap);
        if (!n.is_valid()) return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError, "log: argument is not a number"}});
        return make_flonum(std::log(n.as_double()));
    });

    env.register_builtin("sqrt", 1, false, [&heap](Args args) -> std::expected<LispVal, RuntimeError> {
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

    // ========================================================================
    // Higher-order: apply map for-each
    //
    // NOTE: (apply proc arg1 ... list) is now a VM-level special form
    // (OpCode::Apply / TailApply).  The SA intercepts calls to `apply` and
    // emits the dedicated opcode, so this primitive is only reachable when
    // `apply` is used as a first-class value (e.g. passed to another function).
    // That case is not yet supported — it would require a VM trampoline.
    //
    // map and for-each only work with primitive procedures.  They should be
    // replaced with Scheme definitions once the REPL / stdlib loader exists.
    // ========================================================================

    env.register_builtin("apply", 2, true, [](Args /*args*/) -> std::expected<LispVal, RuntimeError> {
        return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError,
            "apply: cannot be used as a first-class value yet (use direct apply syntax instead)"}});
    });

    env.register_builtin("map", 2, false, [&heap](Args args) -> std::expected<LispVal, RuntimeError> {
        // (map proc list) — single-list version, primitives only
        // TODO: Replace with Scheme definition once stdlib loader exists
        LispVal proc = args[0];
        if (!ops::is_boxed(proc) || ops::tag(proc) != Tag::HeapObject)
            return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError, "map: first argument must be a procedure"}});
        auto* prim = heap.try_get_as<ObjectKind::Primitive, types::Primitive>(ops::payload(proc));
        if (!prim) return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError, "map: closures not yet supported (use manual recursion)"}});

        std::vector<LispVal> results;
        LispVal cur = args[1];
        while (cur != nanbox::Nil) {
            if (!ops::is_boxed(cur) || ops::tag(cur) != Tag::HeapObject)
                return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError, "map: not a proper list"}});
            auto* cons = heap.try_get_as<ObjectKind::Cons, types::Cons>(ops::payload(cur));
            if (!cons) return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError, "map: not a proper list"}});
            std::vector<LispVal> call_args = {cons->car};
            auto res = prim->func(call_args);
            if (!res) return res;
            results.push_back(*res);
            cur = cons->cdr;
        }
        // Build result list
        LispVal result = nanbox::Nil;
        for (auto it = results.rbegin(); it != results.rend(); ++it) {
            auto cons = make_cons(heap, *it, result);
            if (!cons) return cons;
            result = *cons;
        }
        return result;
    });

    env.register_builtin("for-each", 2, false, [&heap](Args args) -> std::expected<LispVal, RuntimeError> {
        // (for-each proc list) — single-list version, primitives only
        // TODO: Replace with Scheme definition once stdlib loader exists
        LispVal proc = args[0];
        if (!ops::is_boxed(proc) || ops::tag(proc) != Tag::HeapObject)
            return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError, "for-each: first argument must be a procedure"}});
        auto* prim = heap.try_get_as<ObjectKind::Primitive, types::Primitive>(ops::payload(proc));
        if (!prim) return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError, "for-each: closures not yet supported (use manual recursion)"}});

        LispVal cur = args[1];
        while (cur != nanbox::Nil) {
            if (!ops::is_boxed(cur) || ops::tag(cur) != Tag::HeapObject)
                return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError, "for-each: not a proper list"}});
            auto* cons = heap.try_get_as<ObjectKind::Cons, types::Cons>(ops::payload(cur));
            if (!cons) return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError, "for-each: not a proper list"}});
            std::vector<LispVal> call_args = {cons->car};
            auto res = prim->func(call_args);
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
}

} // namespace eta::runtime

