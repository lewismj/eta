#pragma once

/**
 * @file stats_extract.h
 * @brief Polymorphic numeric-sequence extraction for Eta stats primitives.
 *
 * Provides a single `to_eigen()` helper that converts any of the following
 * Eta runtime types into an Eigen::VectorXd:
 *   - Cons-list of numbers   (the common case from std.stats)
 *   - Vector of numbers      (Scheme-style #(...) vectors)
 *   - FactTable column       (when given a fact-table + column index pair)
 *
 * This eliminates the duplicated list_to_doubles / column_to_eigen helpers
 * that previously existed in core_primitives.h and stats_primitives.h.
 */

#include <expected>
#include <string>
#include <vector>

#include <Eigen/Dense>

#include <eta/runtime/error.h>
#include <eta/runtime/memory/heap.h>
#include <eta/runtime/nanbox.h>
#include <eta/runtime/numeric_value.h>
#include <eta/runtime/types/fact_table.h>

namespace eta::runtime::stats {

using namespace eta::runtime::nanbox;
using namespace eta::runtime::memory::heap;
using namespace eta::runtime::error;

/**
 * Extract an Eigen::VectorXd from a LispVal that is one of:
 *   (a) a Cons-list of numbers,
 *   (b) a Vector of numbers, or
 *   (c) a FactTable (caller must supply the column index separately).
 *
 * Returns a type-error with `who` context on failure.
 */
inline std::expected<Eigen::VectorXd, RuntimeError>
to_eigen(Heap& heap, LispVal val, const char* who) {
    if (val == Nil) {
        return Eigen::VectorXd{};
    }

    if (ops::is_boxed(val) && ops::tag(val) == Tag::HeapObject) {
        auto id = ops::payload(val);

        if (auto* vec = heap.try_get_as<ObjectKind::Vector, types::Vector>(id)) {
            Eigen::VectorXd result(static_cast<Eigen::Index>(vec->elements.size()));
            for (std::size_t i = 0; i < vec->elements.size(); ++i) {
                auto nv = classify_numeric(vec->elements[i], heap);
                if (!nv.is_valid())
                    return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError,
                        std::string(who) + ": vector element at index "
                        + std::to_string(i) + " is not a number"}});
                result(static_cast<Eigen::Index>(i)) = nv.as_double();
            }
            return result;
        }

        if (auto* cons = heap.try_get_as<ObjectKind::Cons, types::Cons>(id)) {
            std::vector<double> buf;
            auto* cur_cons = cons;
            LispVal cur_tail = val;
            while (true) {
                auto nv = classify_numeric(cur_cons->car, heap);
                if (!nv.is_valid())
                    return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError,
                        std::string(who) + ": list element is not a number"}});
                buf.push_back(nv.as_double());
                cur_tail = cur_cons->cdr;
                if (cur_tail == Nil) break;
                if (!ops::is_boxed(cur_tail) || ops::tag(cur_tail) != Tag::HeapObject)
                    return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError,
                        std::string(who) + ": expected a proper list of numbers"}});
                cur_cons = heap.try_get_as<ObjectKind::Cons, types::Cons>(ops::payload(cur_tail));
                if (!cur_cons)
                    return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError,
                        std::string(who) + ": expected a proper list of numbers"}});
            }
            return Eigen::Map<Eigen::VectorXd>(buf.data(), static_cast<Eigen::Index>(buf.size()));
        }
    }

    return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError,
        std::string(who) + ": expected a list, vector, or fact-table column of numbers"}});
}

/// Extract an Eigen::VectorXd from a specific column of a FactTable.
inline std::expected<Eigen::VectorXd, RuntimeError>
column_to_eigen(types::FactTable& ft, std::size_t col, Heap& heap, const char* who) {
    if (col >= ft.columns.size())
        return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError,
            std::string(who) + ": column index out of range"}});
    Eigen::VectorXd v(static_cast<Eigen::Index>(ft.row_count));
    for (std::size_t r = 0; r < ft.row_count; ++r) {
        auto nv = classify_numeric(ft.columns[col][r], heap);
        if (!nv.is_valid())
            return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError,
                std::string(who) + ": column contains non-numeric value at row "
                + std::to_string(r)}});
        v(static_cast<Eigen::Index>(r)) = nv.as_double();
    }
    return v;
}

} ///< namespace eta::runtime::stats

