/**
 *
 * Each column stores a std::vector<LispVal>.  Hash indexes map column
 * values to matching row IDs for O(1)-amortised lookup.
 *
 * The FactTable is a GC-managed heap object: every LispVal stored in
 * its columns is visited during mark-sweep collection.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
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

        /// Only populated after build_index() is called for that column.
        std::vector<std::unordered_multimap<LispVal, std::size_t>> indexes;

        /**
         * @brief Optional compiled rule payload per row.
         *
         * Ground facts store `Nil` here. Rule rows store a closure / callable
         * value used by the relation layer.
         */
        std::vector<LispVal> rule_column;

        /// Ground-row marker (1 = fully ground fact, 0 = non-ground/rule row).
        std::vector<std::uint8_t> ground_mask;

        /// Logical tombstone (1 = live row, 0 = deleted row).
        std::vector<std::uint8_t> live_mask;

        /// Number of physical rows ever inserted (includes tombstoned rows).
        std::size_t row_count{0};

        /// Number of live rows currently visible to queries.
        std::size_t live_count{0};

        /// Optional predicate identity metadata used by std.db / relation calls.
        std::optional<std::uint64_t> predicate_functor;
        std::uint8_t predicate_arity{0};

        /// Mutators

        /**
         * Add a row.  `values` must have exactly col_names.size() elements.
         * Returns true on success, false on arity mismatch.
         */
        bool add_row(const std::vector<LispVal>& values, LispVal rule = Nil, bool is_ground = true) {
            if (values.size() != columns.size()) return false;
            for (std::size_t i = 0; i < columns.size(); ++i) {
                columns[i].push_back(values[i]);
                /// If this column has a live index, update it incrementally
                if (!indexes[i].empty()) {
                    indexes[i].emplace(values[i], row_count);
                }
            }
            rule_column.push_back(rule);
            ground_mask.push_back(is_ground ? 1u : 0u);
            live_mask.push_back(1u);
            ++row_count;
            ++live_count;
            return true;
        }

        /**
         * @brief Mark a row as deleted (logical tombstone).
         *
         * Query paths and cell reads ignore tombstoned rows. Existing row IDs
         * remain stable.
         */
        bool delete_row(std::size_t row) {
            if (row >= row_count) return false;
            if (live_mask[row] == 0) return false;
            live_mask[row] = 0;
            if (live_count > 0) --live_count;

            for (std::size_t col = 0; col < indexes.size(); ++col) {
                auto& idx = indexes[col];
                if (idx.empty()) continue;
                auto [first, last] = idx.equal_range(columns[col][row]);
                for (auto it = first; it != last; ++it) {
                    if (it->second == row) {
                        idx.erase(it);
                        break;
                    }
                }
            }
            return true;
        }

        /// Attach predicate identity metadata to the table.
        void set_predicate_header(std::uint64_t functor, std::uint8_t arity) {
            predicate_functor = functor;
            predicate_arity = arity;
        }

        /// Build (or rebuild) the hash index for column `col`.
        void build_index(std::size_t col) {
            if (col >= columns.size()) return;
            auto& idx = indexes[col];
            idx.clear();
            idx.reserve(live_count);
            for (std::size_t r = 0; r < row_count; ++r) {
                if (live_mask[r] == 0) continue;
                idx.emplace(columns[col][r], r);
            }
        }

        /// Queries

        /**
         * Look up all row indices where column `col` has value `key`.
         * Uses the index if available; otherwise falls back to linear scan.
         */
        std::vector<std::size_t> query(std::size_t col, LispVal key) const {
            std::vector<std::size_t> result;
            if (col >= columns.size()) return result;
            if (!indexes[col].empty()) {
                /// Indexed path
                auto [first, last] = indexes[col].equal_range(key);
                for (auto it = first; it != last; ++it) {
                    if (it->second < row_count && live_mask[it->second] != 0)
                        result.push_back(it->second);
                }
            } else {
                /// Linear scan fallback
                const auto& c = columns[col];
                for (std::size_t r = 0; r < row_count; ++r) {
                    if (live_mask[r] == 0) continue;
                    if (c[r] == key) result.push_back(r);
                }
            }
            return result;
        }

        /// Get all live row IDs in ascending order.
        std::vector<std::size_t> live_rows() const {
            std::vector<std::size_t> out;
            out.reserve(live_count);
            for (std::size_t r = 0; r < row_count; ++r) {
                if (live_mask[r] != 0) out.push_back(r);
            }
            return out;
        }

        /// Number of live rows visible to queries.
        std::size_t active_row_count() const { return live_count; }

        /// True iff row exists and is not tombstoned.
        bool is_live_row(std::size_t row) const {
            return row < row_count && live_mask[row] != 0;
        }

        /// True iff row exists, is live, and marked as ground.
        bool is_ground_row(std::size_t row) const {
            return row < row_count && live_mask[row] != 0 && ground_mask[row] != 0;
        }

        /// Return the rule payload for a live row, or #f when absent.
        LispVal get_rule(std::size_t row) const {
            if (row >= row_count || live_mask[row] == 0) return False;
            const LispVal rule = rule_column[row];
            return (rule == Nil) ? False : rule;
        }

        /// Get a cell value.  Returns Nil on out-of-bounds.
        LispVal get_cell(std::size_t row, std::size_t col) const {
            if (col >= columns.size() || row >= row_count || live_mask[row] == 0) return Nil;
            return columns[col][row];
        }
    };

} ///< namespace eta::runtime::types

