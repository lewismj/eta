#include <bit>
#include <cstdint>
#include <functional>
#include <limits>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "eta/runtime/core_primitives_internal.h"
#include "eta/runtime/primitives/core_primitives_stats_helpers.h"
#include "eta/runtime/factory.h"
#include "eta/runtime/numeric_value.h"
#include "eta/runtime/overflow.h"
#include "eta/runtime/stats_extract.h"
#include "eta/runtime/stats_math.h"
#include "eta/runtime/types/logic_var.h"
#include "eta/runtime/types/regex.h"

namespace eta::runtime {

void PrimReg::register_stats() {
    using Args = PrimReg::Args;
    auto& env = this->env;
    auto& heap = this->heap;
    auto& intern_table = this->intern_table;

    using detail::core_primitives_stats::get_fact_table;
    using detail::core_primitives_stats::group_count_rows;
    using detail::core_primitives_stats::list_to_vector;
    using detail::core_primitives_stats::row_ids_to_list;

    /// Fact-table builtins

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

    env.register_builtin("%fact-table-insert!", 2, false, [&heap](Args args) -> std::expected<LispVal, RuntimeError> {
        auto ft_res = get_fact_table(args[0], heap, "%fact-table-insert!");
        if (!ft_res) return std::unexpected(ft_res.error());
        auto row_res = list_to_vector(args[1], heap, "%fact-table-insert!: second arg");
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
        [&heap](Args args) -> std::expected<LispVal, RuntimeError> {
            auto ft_res = get_fact_table(args[0], heap, "%fact-table-insert-clause!");
            if (!ft_res) return std::unexpected(ft_res.error());
            auto row_res = list_to_vector(args[1], heap, "%fact-table-insert-clause!: second arg");
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

    env.register_builtin("%fact-table-delete-row!", 2, false, [&heap](Args args) -> std::expected<LispVal, RuntimeError> {
        auto ft_res = get_fact_table(args[0], heap, "%fact-table-delete-row!");
        if (!ft_res) return std::unexpected(ft_res.error());
        auto row_opt = ops::decode<int64_t>(args[1]);
        if (!row_opt)
            return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError, "%fact-table-delete-row!: row index must be a fixnum"}});
        if (*row_opt < 0) return False;
        return (*ft_res)->delete_row(static_cast<std::size_t>(*row_opt)) ? True : False;
    });

    env.register_builtin("%fact-table-row-live?", 2, false, [&heap](Args args) -> std::expected<LispVal, RuntimeError> {
        auto ft_res = get_fact_table(args[0], heap, "%fact-table-row-live?");
        if (!ft_res) return std::unexpected(ft_res.error());
        auto row_opt = ops::decode<int64_t>(args[1]);
        if (!row_opt)
            return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError, "%fact-table-row-live?: row index must be a fixnum"}});
        if (*row_opt < 0) return False;
        return (*ft_res)->is_live_row(static_cast<std::size_t>(*row_opt)) ? True : False;
    });

    env.register_builtin("%fact-table-row-ground?", 2, false, [&heap](Args args) -> std::expected<LispVal, RuntimeError> {
        auto ft_res = get_fact_table(args[0], heap, "%fact-table-row-ground?");
        if (!ft_res) return std::unexpected(ft_res.error());
        auto row_opt = ops::decode<int64_t>(args[1]);
        if (!row_opt)
            return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError, "%fact-table-row-ground?: row index must be a fixnum"}});
        if (*row_opt < 0) return False;
        return (*ft_res)->is_ground_row(static_cast<std::size_t>(*row_opt)) ? True : False;
    });

    env.register_builtin("%fact-table-row-rule", 2, false, [&heap](Args args) -> std::expected<LispVal, RuntimeError> {
        auto ft_res = get_fact_table(args[0], heap, "%fact-table-row-rule");
        if (!ft_res) return std::unexpected(ft_res.error());
        auto row_opt = ops::decode<int64_t>(args[1]);
        if (!row_opt)
            return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError, "%fact-table-row-rule: row index must be a fixnum"}});
        if (*row_opt < 0) return False;
        return (*ft_res)->get_rule(static_cast<std::size_t>(*row_opt));
    });

    env.register_builtin("%fact-table-set-predicate!", 3, false,
        [&heap](Args args) -> std::expected<LispVal, RuntimeError> {
            auto ft_res = get_fact_table(args[0], heap, "%fact-table-set-predicate!");
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

    env.register_builtin("%fact-table-predicate", 1, false, [&heap](Args args) -> std::expected<LispVal, RuntimeError> {
        auto ft_res = get_fact_table(args[0], heap, "%fact-table-predicate");
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

    env.register_builtin("%fact-table-build-index!", 2, false, [&heap](Args args) -> std::expected<LispVal, RuntimeError> {
        auto ft_res = get_fact_table(args[0], heap, "%fact-table-build-index!");
        if (!ft_res) return std::unexpected(ft_res.error());
        auto col_opt = ops::decode<int64_t>(args[1]);
        if (!col_opt)
            return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError, "%fact-table-build-index!: column index must be a fixnum"}});
        (*ft_res)->build_index(static_cast<std::size_t>(*col_opt));
        return True;
    });

    env.register_builtin("%fact-table-query", 3, false, [&heap](Args args) -> std::expected<LispVal, RuntimeError> {
        auto ft_res = get_fact_table(args[0], heap, "%fact-table-query");
        if (!ft_res) return std::unexpected(ft_res.error());
        auto col_opt = ops::decode<int64_t>(args[1]);
        if (!col_opt)
            return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError, "%fact-table-query: column index must be a fixnum"}});
        auto rows = (*ft_res)->query(static_cast<std::size_t>(*col_opt), args[2]);
        return row_ids_to_list(rows, heap, "%fact-table-query");
    });

    /**
     * (%fact-table-group-count table group-col-idx)
     *
     * Returns an alist of dotted pairs: ((key . count) ...).
     */
    env.register_builtin("%fact-table-group-count", 2, false,
        [&heap](Args args) -> std::expected<LispVal, RuntimeError> {
            auto ft_res = get_fact_table(args[0], heap, "%fact-table-group-count");
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
        [&heap](Args args) -> std::expected<LispVal, RuntimeError> {
            auto ft_res = get_fact_table(args[0], heap, "%fact-table-group-sum");
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

    env.register_builtin("%fact-table-live-row-ids", 1, false, [&heap](Args args) -> std::expected<LispVal, RuntimeError> {
        auto ft_res = get_fact_table(args[0], heap, "%fact-table-live-row-ids");
        if (!ft_res) return std::unexpected(ft_res.error());
        return row_ids_to_list((*ft_res)->live_rows(), heap, "%fact-table-live-row-ids");
    });

    env.register_builtin("%fact-table-ref", 3, false, [&heap](Args args) -> std::expected<LispVal, RuntimeError> {
        auto ft_res = get_fact_table(args[0], heap, "%fact-table-ref");
        if (!ft_res) return std::unexpected(ft_res.error());
        auto row_opt = ops::decode<int64_t>(args[1]);
        auto col_opt = ops::decode<int64_t>(args[2]);
        if (!row_opt || !col_opt)
            return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError, "%fact-table-ref: indices must be fixnums"}});
        if (*row_opt < 0 || *col_opt < 0) return Nil;
        return (*ft_res)->get_cell(static_cast<std::size_t>(*row_opt), static_cast<std::size_t>(*col_opt));
    });

    env.register_builtin("%fact-table-row-count", 1, false, [&heap](Args args) -> std::expected<LispVal, RuntimeError> {
        auto ft_res = get_fact_table(args[0], heap, "%fact-table-row-count");
        if (!ft_res) return std::unexpected(ft_res.error());
        return ops::encode(static_cast<int64_t>((*ft_res)->active_row_count()));
    });

    /**
     * (%fact-table-column-names table)
     *
     * Returns the declared column names as a list of symbols.
     */
    env.register_builtin("%fact-table-column-names", 1, false, [&heap, &intern_table](Args args) -> std::expected<LispVal, RuntimeError> {
        auto ft_res = get_fact_table(args[0], heap, "%fact-table-column-names");
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
}

} // namespace eta::runtime
