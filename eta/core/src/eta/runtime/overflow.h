#pragma once

#include <cstdint>
#include <limits>

namespace eta::runtime::detail {

/**
 * @brief Portable overflow-checking integer arithmetic
 *
 * These functions perform an arithmetic operation and return true if the
 * result overflowed, false otherwise. On overflow the value of *result
 *
 * On GCC/Clang the compiler builtins are used for optimal codegen.
 * On MSVC a manual bounds check is used instead.
 */

inline bool add_overflow(int64_t a, int64_t b, int64_t* result) {
#if defined(__GNUC__) || defined(__clang__)
    return __builtin_add_overflow(a, b, result);
#else
    if (b > 0 && a > std::numeric_limits<int64_t>::max() - b) return true;
    if (b < 0 && a < std::numeric_limits<int64_t>::min() - b) return true;
    *result = a + b;
    return false;
#endif
}

inline bool sub_overflow(int64_t a, int64_t b, int64_t* result) {
#if defined(__GNUC__) || defined(__clang__)
    return __builtin_sub_overflow(a, b, result);
#else
    if (b < 0 && a > std::numeric_limits<int64_t>::max() + b) return true;
    if (b > 0 && a < std::numeric_limits<int64_t>::min() + b) return true;
    *result = a - b;
    return false;
#endif
}

inline bool mul_overflow(int64_t a, int64_t b, int64_t* result) {
#if defined(__GNUC__) || defined(__clang__)
    return __builtin_mul_overflow(a, b, result);
#else
    if (a == 0 || b == 0) { *result = 0; return false; }
    if (a > 0 && b > 0 && a > std::numeric_limits<int64_t>::max() / b) return true;
    if (a < 0 && b < 0 && a < std::numeric_limits<int64_t>::max() / b) return true;
    if (a > 0 && b < 0 && b < std::numeric_limits<int64_t>::min() / a) return true;
    if (a < 0 && b > 0 && a < std::numeric_limits<int64_t>::min() / b) return true;
    *result = a * b;
    return false;
#endif
}

} ///< namespace eta::runtime::detail

