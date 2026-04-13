// types/fact_table.h — Columnar fact table with per-column hash indexes.
//
// Each column stores a std::vector<LispVal>.  Hash indexes map column
// values to matching row IDs for O(1)-amortised lookup.
//
// The FactTable is a GC-managed heap object: every LispVal stored in
// its columns is visited during mark-sweep collection.

#pragma once

#include <cstddef>
#include <string>
#include <unordered_map>
#include <vector>
#include <eta/runtime/nanbox.h>

namespace eta::runtime::types {
    using namespace eta::runtime::nanbox;

    struct FactTable {
        /// Column names (parallel to `columns`).
        std::vector<std::string> col_names;

        /// Columnar storage: columns[col_idx][row_idx] = LispVal.
        std::vector<std::vector<LispVal>> columns;

        /// Per-column hash index: indexes[col_idx] maps value → list of row indices.
        /// Only populated after build_index() is called for that column.
        std::vector<std::unordered_multimap<LispVal, std::size_t>> indexes;

        /// Number of rows currently stored.
        std::size_t row_count{0};

        // Mutators

        /// Add a row.  `values` must have exactly col_names.size() elements.
        /// Returns true on success, false on arity mismatch.
        bool add_row(const std::vector<LispVal>& values) {
            if (values.size() != columns.size()) return false;
            for (std::size_t i = 0; i < columns.size(); ++i) {
                columns[i].push_back(values[i]);
                // If this column has a live index, update it incrementally
                if (!indexes[i].empty()) {
                    indexes[i].emplace(values[i], row_count);
                }
            }
            ++row_count;
            return true;
        }

        /// Build (or rebuild) the hash index for column `col`.
        void build_index(std::size_t col) {
            if (col >= columns.size()) return;
            auto& idx = indexes[col];
            idx.clear();
            idx.reserve(row_count);
            for (std::size_t r = 0; r < row_count; ++r) {
                idx.emplace(columns[col][r], r);
            }
        }

        // Queries

        /// Look up all row indices where column `col` has value `key`.
        /// Uses the index if available; otherwise falls back to linear scan.
        std::vector<std::size_t> query(std::size_t col, LispVal key) const {
            std::vector<std::size_t> result;
            if (col >= columns.size()) return result;
            if (!indexes[col].empty()) {
                // Indexed path
                auto [first, last] = indexes[col].equal_range(key);
                for (auto it = first; it != last; ++it)
                    result.push_back(it->second);
            } else {
                // Linear scan fallback
                const auto& c = columns[col];
                for (std::size_t r = 0; r < row_count; ++r) {
                    if (c[r] == key) result.push_back(r);
                }
            }
            return result;
        }

        /// Get a cell value.  Returns Nil on out-of-bounds.
        LispVal get_cell(std::size_t row, std::size_t col) const {
            if (col >= columns.size() || row >= row_count) return Nil;
            return columns[col][row];
        }
    };

} // namespace eta::runtime::types

