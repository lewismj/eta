#pragma once

#include <bit>
#include <cstdint>
#include <expected>
#include <limits>

#include <eta/runtime/factory.h>
#include <eta/runtime/nanbox.h>
#include <eta/runtime/numeric_value.h>
#include <eta/runtime/overflow.h>

namespace eta::runtime::numeric_ops {

/**
 * @brief Single source of truth for fixnum/flonum arithmetic and comparison
 *
 * Shared by the VM arithmetic opcodes and the builtin primitives so the two
 * cannot drift apart. TapeRef (AD) operands are not handled here; callers
 * must check for them first and route them to the tape.
 *
 * Semantics:
 *   - fixnum op fixnum stays fixnum; on int64 overflow the result is promoted to flonum
 *   - fixnum / fixnum is exact when it divides evenly, otherwise flonum
 *   - any flonum operand makes the result flonum
 *   - division by exact or inexact zero is an error
 *   - comparisons follow IEEE 754 when either side is a flonum (NaN compares false, 0.0 == -0.0)
 */

enum class ArithOp : std::uint8_t { Add, Sub, Mul, Div };
enum class CmpOp : std::uint8_t { NumEq, Lt, Gt, Le, Ge };

/// Largest valid CmpOp encoding (NumCmp instruction arg is validated against this).
inline constexpr std::uint32_t kMaxCmpOp = static_cast<std::uint32_t>(CmpOp::Ge);

/// Scheme name of a comparison, used for error messages and disassembly.
[[nodiscard]] constexpr const char* to_string(const CmpOp op) noexcept {
    switch (op) {
        case CmpOp::NumEq: return "=";
        case CmpOp::Lt:    return "<";
        case CmpOp::Gt:    return ">";
        case CmpOp::Le:    return "<=";
        case CmpOp::Ge:    return ">=";
    }
    return "?";
}

[[nodiscard]] constexpr const char* to_string(const ArithOp op) noexcept {
    switch (op) {
        case ArithOp::Add: return "+";
        case ArithOp::Sub: return "-";
        case ArithOp::Mul: return "*";
        case ArithOp::Div: return "/";
    }
    return "?";
}

enum class NumError : std::uint8_t { NotANumber, DivisionByZero };

using memory::factory::make_fixnum;
using memory::factory::make_flonum;

/// Decode immediate numbers (fixnum payload, raw double, boxed NaN) without touching the heap.
[[nodiscard]] inline bool classify_immediate(const LispVal v, NumericValue& out) noexcept {
    if (!ops::is_boxed(v)) {
        out = {NumType::Flonum, 0, std::bit_cast<double>(v)};
        return true;
    }
    const Tag t = ops::tag(v);
    if (t == Tag::Fixnum) {
        out = {NumType::Fixnum, ops::sign_extend_fixnum(ops::payload(v)), 0.0};
        return true;
    }
    if (t == Tag::Nan) {
        out = {NumType::Flonum, 0, std::numeric_limits<double>::quiet_NaN()};
        return true;
    }
    return false;
}

/// classify_numeric with an inline fast path; only heap objects pay for the heap lookup.
[[nodiscard]] inline NumericValue classify(const LispVal v, Heap& heap) {
    NumericValue n{};
    if (classify_immediate(v, n)) return n;
    if (ops::tag(v) != Tag::HeapObject) return {NumType::Invalid, 0, 0.0};
    return classify_numeric(v, heap);
}

[[nodiscard]] inline double apply_double(const ArithOp op, const double a, const double b) noexcept {
    switch (op) {
        case ArithOp::Add: return a + b;
        case ArithOp::Sub: return a - b;
        case ArithOp::Mul: return a * b;
        case ArithOp::Div: return a / b;
    }
    return 0.0;
}

template <typename T>
[[nodiscard]] inline bool apply_compare(const CmpOp op, const T a, const T b) noexcept {
    switch (op) {
        case CmpOp::NumEq: return a == b;
        case CmpOp::Lt:    return a < b;
        case CmpOp::Gt:    return a > b;
        case CmpOp::Le:    return a <= b;
        case CmpOp::Ge:    return a >= b;
    }
    return false;
}

/// Arithmetic on already-classified (valid) operands.
[[nodiscard]] inline std::expected<NumericValue, NumError>
arith(const ArithOp op, const NumericValue& a, const NumericValue& b) noexcept {
    if (a.is_flonum() || b.is_flonum()) {
        const double y = b.as_double();
        if (op == ArithOp::Div && y == 0.0) return std::unexpected(NumError::DivisionByZero);
        return NumericValue{NumType::Flonum, 0, apply_double(op, a.as_double(), y)};
    }

    const int64_t x = a.int_val;
    const int64_t y = b.int_val;
    int64_t r = 0;
    bool overflowed = false;
    switch (op) {
        case ArithOp::Add: overflowed = detail::add_overflow(x, y, &r); break;
        case ArithOp::Sub: overflowed = detail::sub_overflow(x, y, &r); break;
        case ArithOp::Mul: overflowed = detail::mul_overflow(x, y, &r); break;
        case ArithOp::Div:
            if (y == 0) return std::unexpected(NumError::DivisionByZero);
            /// INT64_MIN / -1 overflows, and INT64_MIN % -1 traps on x86
            if (y == -1) {
                overflowed = detail::sub_overflow(0, x, &r);
                break;
            }
            if (x % y != 0) {
                return NumericValue{NumType::Flonum, 0,
                                    static_cast<double>(x) / static_cast<double>(y)};
            }
            r = x / y;
            break;
    }
    if (overflowed) {
        return NumericValue{NumType::Flonum, 0,
                            apply_double(op, static_cast<double>(x), static_cast<double>(y))};
    }
    return NumericValue{NumType::Fixnum, r, 0.0};
}

/// Comparison on already-classified (valid) operands.
[[nodiscard]] inline bool compare(const CmpOp op, const NumericValue& a, const NumericValue& b) noexcept {
    if (a.is_flonum() || b.is_flonum()) return apply_compare(op, a.as_double(), b.as_double());
    return apply_compare(op, a.int_val, b.int_val);
}

/// Classify and compare two values; NotANumber if either is not a number.
[[nodiscard]] inline std::expected<bool, NumError>
compare(Heap& heap, const CmpOp op, const LispVal a, const LispVal b) {
    const auto na = classify(a, heap);
    const auto nb = classify(b, heap);
    if (!na.is_valid() || !nb.is_valid()) return std::unexpected(NumError::NotANumber);
    return compare(op, na, nb);
}

/// Box a classified result, allocating a heap fixnum only when it does not fit an immediate.
[[nodiscard]] inline std::expected<LispVal, error::RuntimeError> box(Heap& heap, const NumericValue& n) {
    if (n.is_flonum()) return make_flonum(n.float_val);
    return make_fixnum(heap, n.int_val);
}

/**
 * Immediate-only fast path for the VM opcodes: never allocates and never fails.
 * Returns false when the general path is needed (heap fixnums, TapeRefs,
 * non-numbers, results outside the immediate fixnum range, exact division
 * that is not exact, division by zero).
 */
[[nodiscard]] inline bool try_arith_immediate(const ArithOp op, const LispVal a, const LispVal b,
                                              LispVal& out) noexcept {
    NumericValue na{};
    NumericValue nb{};
    if (!classify_immediate(a, na) || !classify_immediate(b, nb)) return false;

    if (na.is_flonum() || nb.is_flonum()) {
        const double y = nb.as_double();
        if (op == ArithOp::Div && y == 0.0) return false;
        out = *ops::encode(apply_double(op, na.as_double(), y));
        return true;
    }

    /// Both operands are 47-bit immediates, so Add/Sub cannot overflow int64.
    const int64_t x = na.int_val;
    const int64_t y = nb.int_val;
    int64_t r = 0;
    switch (op) {
        case ArithOp::Add: r = x + y; break;
        case ArithOp::Sub: r = x - y; break;
        case ArithOp::Mul:
            if (detail::mul_overflow(x, y, &r)) return false;
            break;
        case ArithOp::Div:
            if (y == 0 || x % y != 0) return false;
            r = x / y;
            break;
    }
    if (r < constants::FIXNUM_MIN || r > constants::FIXNUM_MAX) return false;
    out = ops::box(Tag::Fixnum, static_cast<uint64_t>(r));
    return true;
}

} ///< namespace eta::runtime::numeric_ops
