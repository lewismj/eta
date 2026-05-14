#include <algorithm>
#include <cmath>
#include <cstdint>
#include <expected>
#include <functional>
#include <limits>
#include <optional>
#include <sstream>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "eta/runtime/core_primitives_internal.h"
#include "eta/runtime/factory.h"
#include "eta/runtime/numeric_value.h"
#include "eta/runtime/string_view.h"
#include "eta/runtime/value_formatter.h"
#include "eta/runtime/vm/vm.h"
#include "eta/runtime/types/logic_var.h"
#include "eta/runtime/clp/domain.h"
#include "eta/runtime/clp/constraint_store.h"
#include "eta/runtime/clp/alldiff_regin.h"
#include "eta/runtime/clp/linear.h"
#include "eta/runtime/clp/quadratic.h"
#include "eta/runtime/clp/qp_solver.h"
#include "eta/runtime/clp/fm.h"
#include "eta/runtime/clp/simplex.h"

namespace eta::runtime {

void PrimReg::register_clp() {
    using Args = PrimReg::Args;
    auto& env = this->env;
    auto& heap = this->heap;
    auto& intern_table = this->intern_table;
    auto* vm = this->vm;

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

}

void PrimReg::register_clp_prop_queue_size_bridge() {
    using Args = PrimReg::Args;
    auto& env = this->env;
    auto* vm = this->vm;

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