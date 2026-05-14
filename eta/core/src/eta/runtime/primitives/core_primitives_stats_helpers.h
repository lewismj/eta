#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "eta/runtime/core_primitives_internal.h"
#include "eta/runtime/factory.h"

namespace eta::runtime::detail::core_primitives_stats {

/**
 * @brief Extract FactTable* from a LispVal or return a type error.
 */
inline std::expected<types::FactTable*, RuntimeError> get_fact_table(
    LispVal value,
    Heap& heap,
    const char* who) {
    if (!ops::is_boxed(value) || ops::tag(value) != Tag::HeapObject) {
        return std::unexpected(RuntimeError{VMError{
            RuntimeErrorCode::TypeError,
            std::string(who) + ": argument must be a fact-table"}});
    }

    auto* fact_table =
        heap.try_get_as<ObjectKind::FactTable, types::FactTable>(ops::payload(value));
    if (!fact_table) {
        return std::unexpected(RuntimeError{VMError{
            RuntimeErrorCode::TypeError,
            std::string(who) + ": argument must be a fact-table"}});
    }
    return fact_table;
}

/**
 * @brief Decode a proper Eta list into a flat vector.
 */
inline std::expected<std::vector<LispVal>, RuntimeError> list_to_vector(
    LispVal list,
    Heap& heap,
    const char* who) {
    std::vector<LispVal> out;
    LispVal cur = list;
    while (cur != Nil) {
        if (!ops::is_boxed(cur) || ops::tag(cur) != Tag::HeapObject) {
            return std::unexpected(RuntimeError{VMError{
                RuntimeErrorCode::TypeError,
                std::string(who) + ": expected a proper list"}});
        }
        auto* cons = heap.try_get_as<ObjectKind::Cons, types::Cons>(ops::payload(cur));
        if (!cons) {
            return std::unexpected(RuntimeError{VMError{
                RuntimeErrorCode::TypeError,
                std::string(who) + ": expected a proper list"}});
        }
        out.push_back(cons->car);
        cur = cons->cdr;
    }
    return out;
}

/**
 * @brief Encode a row-id vector as an Eta list of fixnums.
 */
inline std::expected<LispVal, RuntimeError> row_ids_to_list(
    const std::vector<std::size_t>& rows,
    Heap& heap,
    const char* who) {
    LispVal result = Nil;
    for (auto it = rows.rbegin(); it != rows.rend(); ++it) {
        auto enc = ops::encode(static_cast<int64_t>(*it));
        if (!enc) {
            return std::unexpected(RuntimeError{VMError{
                RuntimeErrorCode::TypeError,
                std::string(who) + ": row index too large"}});
        }
        auto cell = make_cons(heap, *enc, result);
        if (!cell) return std::unexpected(cell.error());
        result = *cell;
    }
    return result;
}

/**
 * @brief Count live rows per distinct key in one column.
 *
 * Returns groups in first-seen row order for deterministic output.
 */
inline std::vector<std::pair<LispVal, std::size_t>> group_count_rows(
    const types::FactTable& fact_table,
    std::size_t column) {
    std::vector<std::pair<LispVal, std::size_t>> out;
    if (column >= fact_table.columns.size()) return out;

    std::unordered_map<LispVal, std::size_t> slot_by_key;
    slot_by_key.reserve(fact_table.live_count);

    std::vector<LispVal> keys;
    std::vector<std::size_t> counts;
    keys.reserve(fact_table.live_count);
    counts.reserve(fact_table.live_count);

    const auto& group_col = fact_table.columns[column];
    for (std::size_t row = 0; row < fact_table.row_count; ++row) {
        if (fact_table.live_mask[row] == 0) continue;
        const LispVal key = group_col[row];
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
}

} // namespace eta::runtime::detail::core_primitives_stats
