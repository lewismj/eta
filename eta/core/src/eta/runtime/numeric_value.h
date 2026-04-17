#pragma once

#include <cstdint>
#include <limits>

#include <eta/runtime/nanbox.h>
#include <eta/runtime/memory/heap.h>
#include <eta/runtime/memory/value_visit.h>

namespace eta::runtime {

using namespace eta::runtime::nanbox;
using namespace eta::runtime::memory::heap;
using namespace eta::runtime::memory::value_visit;

/**
 * @brief Runtime numeric classification
 *
 * eta uses a simple two-type numeric model: int64_t (fixnum) and double (flonum).
 * This is a deliberate design choice favouring speed and simplicity over a full
 * Scheme numeric tower (bignum, rational, complex).
 *
 * Fixnums may be NaN-box immediates (47-bit) or heap-allocated (full 64-bit).
 * Flonums are raw IEEE 754 doubles stored directly in the NaN-box encoding.
 */
enum class NumType : uint8_t { Fixnum, Flonum, Invalid };

struct NumericValue {
    NumType type;
    int64_t int_val;    ///< Valid when type == Fixnum
    double  float_val;  ///< Valid when type == Flonum

    [[nodiscard]] bool is_valid()  const noexcept { return type != NumType::Invalid; }
    [[nodiscard]] bool is_fixnum() const noexcept { return type == NumType::Fixnum;  }
    [[nodiscard]] bool is_flonum() const noexcept { return type == NumType::Flonum;  }

    /// Promote to double regardless of underlying type
    [[nodiscard]] double as_double() const noexcept {
        return is_flonum() ? float_val : static_cast<double>(int_val);
    }
};

/**
 * @brief Visitor that classifies a LispVal into NumericValue
 *
 * Uses the centralized ValueVisitor from value_visit.h so that
 * tag-dispatch logic is never duplicated.
 */
struct NumericClassifier final : ValueVisitor<NumericValue> {
    Heap& heap;
    explicit NumericClassifier(Heap& h) : heap(h) {}

    NumericValue visit_fixnum(std::int64_t v) override {
        return {NumType::Fixnum, v, 0.0};
    }
    NumericValue visit_flonum(double v) override {
        return {NumType::Flonum, 0, v};
    }
    NumericValue visit_heapref(uint64_t obj_id) override {
        /// Heap-allocated fixnum (64-bit integers that don't fit in 47-bit immediate)
        HeapEntry entry;
        if (heap.try_get(obj_id, entry) && entry.header.kind == ObjectKind::Fixnum) {
            return {NumType::Fixnum, *static_cast<int64_t*>(entry.ptr), 0.0};
        }
        return {NumType::Invalid, 0, 0.0};
    }
    NumericValue visit_nan()           override { return {NumType::Flonum, 0, std::numeric_limits<double>::quiet_NaN()}; }
    NumericValue visit_char(char32_t)  override { return {NumType::Invalid, 0, 0.0}; }
    NumericValue visit_string(uint64_t)override { return {NumType::Invalid, 0, 0.0}; }
    NumericValue visit_symbol(uint64_t)override { return {NumType::Invalid, 0, 0.0}; }
    NumericValue visit_nil()           override { return {NumType::Invalid, 0, 0.0}; }
};

/// Convenience: classify a single LispVal
inline NumericValue classify_numeric(LispVal v, Heap& heap) {
    NumericClassifier c(heap);
    return visit_value(v, c);
}

} ///< namespace eta::runtime

