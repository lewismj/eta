/// VM logic support: unification, trail and rollback, propagators, copy_term.
#include "vm.h"
#include <algorithm>
#include <chrono>
#include <exception>
#include <future>
#include <limits>
#include "eta/runtime/actor/actor_system.h"
#include "eta/runtime/builtin_catalog.h"
#include "eta/runtime/factory.h"
#include "eta/runtime/ad_error.h"
#include "eta/runtime/types/types.h"
#include "eta/runtime/types/logic_var.h"
#include "eta/runtime/types/primitive.h"
#include "eta/runtime/types/tape.h"
#include "eta/runtime/types/tape_ref.h"
#include "eta/runtime/memory/value_visit.h"
#include "eta/runtime/memory/mark_sweep_gc.h"
#include "eta/runtime/string_view.h"
#include "eta/runtime/numeric_value.h"
#include "eta/runtime/overflow.h"
#include "eta/runtime/port.h"
#include "eta/runtime/clp/domain.h"
#include "eta/runtime/prof/profiler.h"
#include <bit>

namespace eta::runtime::vm {

using namespace eta::runtime::memory::value_visit;

using namespace eta::runtime::memory::factory;
using namespace eta::runtime::types;
namespace prof = eta::runtime::prof;

/**
 * Unification helpers
 */

LispVal VM::deref(LispVal v) {
    /// Follow the binding chain until we hit an unbound variable or a non-LogicVar.
    for (;;) {
        auto* lv = try_get_as<ObjectKind::LogicVar, types::LogicVar>(v);
        if (!lv || !lv->binding.has_value()) return v;
        v = *lv->binding;
    }
}

bool VM::occurs_check(LispVal lvar, LispVal term) {
    /// Returns true if lvar appears anywhere inside term (cycle would be created).
    term = deref(term);
    if (term == lvar) return true;
    if (auto* cons = try_get_as<ObjectKind::Cons, types::Cons>(term)) {
        return occurs_check(lvar, cons->car) || occurs_check(lvar, cons->cdr);
    }
    if (auto* vec = try_get_as<ObjectKind::Vector, types::Vector>(term)) {
        for (auto elem : vec->elements)
            if (occurs_check(lvar, elem)) return true;
    }
    if (auto* ct = try_get_as<ObjectKind::CompoundTerm, types::CompoundTerm>(term)) {
        for (auto elem : ct->args)
            if (occurs_check(lvar, elem)) return true;
    }
    return false;
}

/**
 * Outer wrapper: exactly the original unify semantics for nested
 * (recursive) calls; for top-level calls, takes a trail snapshot, runs the
 * inner step, then drains the propagation queue.  On any failure the outer
 * snapshot is restored and the queue + dedup set are cleared.
 */

void VM::trail_set_domain(memory::heap::ObjectId id, clp::Domain dom) {
    TrailEntry e{};
    e.kind = TrailEntry::Kind::Domain;
    e.var  = ops::box(Tag::HeapObject, static_cast<int64_t>(id));
    if (const auto* prev = constraint_store_.get_domain(id)) {
        e.had_prev    = true;
        e.prev_domain = *prev;
    } else {
        e.had_prev    = false;
        e.prev_domain = std::nullopt;
    }
    trail_stack_.push_back(std::move(e));
    constraint_store_.set_domain_no_trail(id, std::move(dom));
}

void VM::trail_erase_domain(memory::heap::ObjectId id) {
    const auto* prev = constraint_store_.get_domain(id);
    if (!prev) return;  ///< nothing to do; not trailed
    TrailEntry e{};
    e.kind        = TrailEntry::Kind::Domain;
    e.var         = ops::box(Tag::HeapObject, static_cast<int64_t>(id));
    e.had_prev    = true;
    e.prev_domain = *prev;
    trail_stack_.push_back(std::move(e));
    constraint_store_.erase_domain_no_trail(id);
}

void VM::trail_mark_real_store() {
    TrailEntry e{};
    e.kind = TrailEntry::Kind::RealStore;
    e.prev_real_store_size = real_store_.size();
    trail_stack_.push_back(std::move(e));
}

void VM::trail_assert_simplex_bound(memory::heap::ObjectId id,
                                    std::optional<clp::Bound> lo,
                                    std::optional<clp::Bound> hi) {
    const auto* prev = real_store_.simplex_bounds(id);
    if (prev && prev->lo == lo && prev->hi == hi) return;
    if (!prev && !lo.has_value() && !hi.has_value()) return;

    TrailEntry e{};
    e.kind = TrailEntry::Kind::SimplexBound;
    e.var = ops::box(Tag::HeapObject, static_cast<int64_t>(id));
    if (prev) {
        e.had_prev = true;
        e.prev_simplex_lo = prev->lo;
        e.prev_simplex_hi = prev->hi;
    } else {
        e.had_prev = false;
        e.prev_simplex_lo = std::nullopt;
        e.prev_simplex_hi = std::nullopt;
    }
    trail_stack_.push_back(std::move(e));

    if (!lo.has_value() && !hi.has_value()) {
        real_store_.erase_simplex_bounds_no_trail(id);
    } else {
        real_store_.set_simplex_bounds_no_trail(id, std::move(lo), std::move(hi));
    }
}

/**
 * Helper: restore trail entries created since `mark` to their pre-write
 * state.  Used by both the outer-`unify` wrapper rollback and the inner
 * per-binding rollback paths inside `unify_internal`.
 */
namespace {
    inline void rollback_one(VM& /*vm*/, TrailEntry& e,
                             memory::heap::Heap& heap_ref,
                             clp::ConstraintStore& cstore,
                             clp::RealStore& rstore) {
        switch (e.kind) {
            case TrailEntry::Kind::Bind:
                if (ops::is_boxed(e.var) && ops::tag(e.var) == Tag::HeapObject) {
                    auto* lv = heap_ref.try_get_as<ObjectKind::LogicVar, types::LogicVar>(ops::payload(e.var));
                    if (lv) lv->binding = std::nullopt;
                }
                break;
            case TrailEntry::Kind::Attr:
                if (ops::is_boxed(e.var) && ops::tag(e.var) == Tag::HeapObject) {
                    auto* lv = heap_ref.try_get_as<ObjectKind::LogicVar, types::LogicVar>(ops::payload(e.var));
                    if (lv) {
                        if (e.had_prev) lv->attrs[e.module_key] = e.prev_value;
                        else            lv->attrs.erase(e.module_key);
                    }
                }
                break;
            case TrailEntry::Kind::Domain: {
                auto id = static_cast<memory::heap::ObjectId>(ops::payload(e.var));
                if (e.had_prev && e.prev_domain.has_value())
                    cstore.set_domain_no_trail(id, *e.prev_domain);
                else
                    cstore.erase_domain_no_trail(id);
                break;
            }
            case TrailEntry::Kind::RealStore:
                rstore.truncate(e.prev_real_store_size);
                break;
            case TrailEntry::Kind::SimplexBound: {
                auto id = static_cast<memory::heap::ObjectId>(ops::payload(e.var));
                if (e.had_prev) {
                    rstore.set_simplex_bounds_no_trail(id, e.prev_simplex_lo, e.prev_simplex_hi);
                } else {
                    rstore.erase_simplex_bounds_no_trail(id);
                }
                break;
            }
        }
    }
}

void VM::rollback_trail_to(std::size_t mark) {
    while (trail_stack_.size() > mark) {
        auto& e = trail_stack_.back();
        rollback_one(*this, e, heap_, constraint_store_, real_store_);
        trail_stack_.pop_back();
    }
}

bool VM::unify(LispVal a, LispVal b) {
    if (unify_depth_ > 0) {
        return unify_internal(a, b);
    }
    ++unify_depth_;
    const auto t_snap = trail_stack_.size();

    bool ok = unify_internal(a, b);
    if (ok) ok = drain_propagators();

    if (!ok) {
        /// and drop pending propagators that were enqueued under this unify.
        while (trail_stack_.size() > t_snap) {
            auto& e = trail_stack_.back();
            rollback_one(*this, e, heap_, constraint_store_, real_store_);
            trail_stack_.pop_back();
        }
        prop_queue_.clear();
        prop_queued_set_.clear();
    }

    --unify_depth_;
    return ok;
}

void VM::enqueue_propagator(LispVal thunk) {
    if (!ops::is_boxed(thunk) || ops::tag(thunk) != Tag::HeapObject) return;
    auto id = ops::payload(thunk);
    if (!prop_queued_set_.insert(id).second) return;  ///< already pending
    prop_queue_.push_back(thunk);
}

bool VM::drain_propagators() {
    /**
     * Process FIFO; thunks may enqueue further work via nested unify calls.
     * Each thunk is removed from the dedup set BEFORE invocation so it may
     * re-enqueue itself if its body's writes re-fire `'clp.prop` on a var
     * it has already narrowed (standard CLP fixpoint).
     */
    while (!prop_queue_.empty()) {
        LispVal thunk = prop_queue_.front();
        prop_queue_.pop_front();
        if (ops::is_boxed(thunk) && ops::tag(thunk) == Tag::HeapObject)
            prop_queued_set_.erase(ops::payload(thunk));
        auto r = call_value(thunk, {});
        if (!r) {
            /**
             * Propagator raised; treat as failure and let the outer wrapper
             * roll back.  Drop any remaining pending entries.
             */
            prop_queue_.clear();
            prop_queued_set_.clear();
            return false;
        }
        if (*r == nanbox::False) {
            prop_queue_.clear();
            prop_queued_set_.clear();
            return false;
        }
    }
    return true;
}

bool VM::unify_internal(LispVal a, LispVal b) {
    a = deref(a);
    b = deref(b);

    /// Identical values (includes two unbound vars that are the same object)
    if (a == b) return true;

    /**
     * Helper: check domain constraint before committing a binding
     * When an unbound variable with a CLP domain is about to be bound to a
     * concrete (non-variable) ground value, verify the value lies in the domain.
     * constraint will be re-evaluated when that variable is eventually grounded
     * (forward-checking style).
     */
    auto check_domain = [&](LispVal lvar, LispVal candidate) -> bool {
        LispVal ground = deref(candidate);
        /// If candidate deref's to a logic variable, defer the check.
        if (try_get_as<ObjectKind::LogicVar, types::LogicVar>(ground))
            return true;
        auto id = ops::payload(lvar);
        const auto* dom = constraint_store_.get_domain(id);
        if (!dom) return true;  ///< no domain constraint
        auto n = classify_numeric(ground, heap_);
        if (!n.is_valid()) return false;
        /**
         * Real-valued domains accept any in-range numeric
         * (fixnum or flonum); Z/FD domains reject non-integers.
         */
        if (std::holds_alternative<clp::RDomain>(*dom))
            return clp::domain_contains_double(*dom, n.as_double());
        if (n.is_flonum()) return false;  ///< non-integer into integer domain
        return clp::domain_contains_int(*dom, n.int_val);
    };

    /**
     * Helper: fire attr-unify-hooks after a logic var has been bound.
     * For each attribute whose module has a registered hook, invoke
     *   (hook var bound-value attr-value)
     * synchronously.  The hook must return #t on success or #f on failure.
     * A failing hook causes unify to fail; the caller's trail-mark / unwind
     * machinery restores both the binding and any attribute writes the hook
     * may have made before returning.  Hook execution order is arbitrary.
     */
    auto fire_attr_hooks = [&](types::LogicVar* lv, LispVal var_ref,
                               LispVal bound_val) -> bool {
        if (!lv || lv->attrs.empty()) return true;
        if (attr_unify_hooks_.empty() && async_thunk_attrs_.empty()) return true;
        /// Snapshot keys so we are not iterating while the hook mutates attrs.
        std::vector<std::pair<memory::intern::InternId, LispVal>> pairs;
        pairs.reserve(lv->attrs.size());
        for (const auto& kv : lv->attrs) pairs.emplace_back(kv.first, kv.second);
        for (const auto& [key, attr_val] : pairs) {
            /**
             * re-propagator thunks; enqueue each (idempotent) for the
             * outer-unify drain rather than invoking synchronously.
             */
            if (async_thunk_attrs_.count(key)) {
                LispVal cur = attr_val;
                while (auto* cell = try_get_as<ObjectKind::Cons, types::Cons>(cur)) {
                    enqueue_propagator(cell->car);
                    cur = cell->cdr;
                }
                continue;
            }
            auto it = attr_unify_hooks_.find(key);
            if (it == attr_unify_hooks_.end()) continue;
            auto res = call_value(it->second, {var_ref, bound_val, attr_val});
            if (!res)                        return false;  ///< runtime error from hook
            if (*res == nanbox::False)       return false;  ///< hook explicitly failed
        }
        return true;
    };

    /// a is an unbound logic variable
    if (auto* lva = try_get_as<ObjectKind::LogicVar, types::LogicVar>(a)) {
        if (!lva->binding.has_value()) {
            if (occurs_check_mode_ != OccursCheckMode::Never && occurs_check(a, b)) {
                if (occurs_check_mode_ == OccursCheckMode::Error)
                    last_unify_cycle_error_ = true;
                return false;   ///< would create cycle
            }
            if (!check_domain(a, b)) return false;  ///< CLP domain violation
            /**
             * Snapshot trail so we can roll back atomically if any
             * downstream step (var-var intersect, bind, attr-unify hook)
             * rejects.  Domain writes are now also on the unified trail,
             * so a single mark suffices.
             */
            const auto trail_snap  = trail_stack_.size();
            auto rollback = [&]{
                while (trail_stack_.size() > trail_snap) {
                    auto& e = trail_stack_.back();
                    rollback_one(*this, e, heap_, constraint_store_, real_store_);
                    trail_stack_.pop_back();
                }
            };
            /**
             * When both sides are unbound domained logic vars,
             * intersect their CLP domains onto `b` (the surviving var)
             * BEFORE binding, so future ground-unify domain checks see the
             * merged constraint.  Install via trailed `trail_set_domain`;
             * an empty intersection fails the unify cleanly.
             */
            if (auto* lvb0 = try_get_as<ObjectKind::LogicVar, types::LogicVar>(b)) {
                if (!lvb0->binding.has_value()) {
                    auto a_id = ops::payload(a);
                    auto b_id = ops::payload(b);
                    const auto* dom_a = constraint_store_.get_domain(a_id);
                    const auto* dom_b = constraint_store_.get_domain(b_id);
                    if (dom_a && dom_b) {
                        auto isect = clp::domain_intersect(*dom_a, *dom_b);
                        if (clp::domain_empty(isect)) return false;
                        trail_set_domain(b_id, std::move(isect));
                    } else if (dom_a && !dom_b) {
                        trail_set_domain(b_id, *dom_a);
                    }
                }
            }
            lva->binding = b;
            trail_stack_.push_back({TrailEntry::Kind::Bind, a, nanbox::Nil});
            if (!fire_attr_hooks(lva, a, b)) {
                rollback();
                return false;
            }
            return true;
        }
    }

    /// b is an unbound logic variable
    if (auto* lvb = try_get_as<ObjectKind::LogicVar, types::LogicVar>(b)) {
        if (!lvb->binding.has_value()) {
            if (occurs_check_mode_ != OccursCheckMode::Never && occurs_check(b, a)) {
                if (occurs_check_mode_ == OccursCheckMode::Error)
                    last_unify_cycle_error_ = true;
                return false;   ///< would create cycle
            }
            if (!check_domain(b, a)) return false;  ///< CLP domain violation
            /**
             * (No var-var intersection branch needed here: `a` deref'd to a
             *  non-var above, so we're binding an unbound b to a ground a.)
             */
            const auto trail_snap  = trail_stack_.size();
            lvb->binding = a;
            trail_stack_.push_back({TrailEntry::Kind::Bind, b, nanbox::Nil});
            if (!fire_attr_hooks(lvb, b, a)) {
                while (trail_stack_.size() > trail_snap) {
                    auto& e = trail_stack_.back();
                    rollback_one(*this, e, heap_, constraint_store_, real_store_);
                    trail_stack_.pop_back();
                }
                return false;
            }
            return true;
        }
    }

    if (auto* ca = try_get_as<ObjectKind::Cons, types::Cons>(a)) {
        if (auto* cb = try_get_as<ObjectKind::Cons, types::Cons>(b)) {
            return unify(ca->car, cb->car) && unify(ca->cdr, cb->cdr);
        }
        return false;
    }

    if (auto* va = try_get_as<ObjectKind::Vector, types::Vector>(a)) {
        if (auto* vb = try_get_as<ObjectKind::Vector, types::Vector>(b)) {
            if (va->elements.size() != vb->elements.size()) return false;
            for (std::size_t i = 0; i < va->elements.size(); ++i)
                if (!unify(va->elements[i], vb->elements[i])) return false;
            return true;
        }
        return false;
    }

    if (auto* cta = try_get_as<ObjectKind::CompoundTerm, types::CompoundTerm>(a)) {
        if (auto* ctb = try_get_as<ObjectKind::CompoundTerm, types::CompoundTerm>(b)) {
            if (cta->functor != ctb->functor) return false;
            if (cta->args.size() != ctb->args.size()) return false;
            for (std::size_t i = 0; i < cta->args.size(); ++i)
                if (!unify(cta->args[i], ctb->args[i])) return false;
            return true;
        }
        return false;
    }

    /// Structural mismatch
    return false;
}

/**
 */

std::expected<LispVal, RuntimeError> VM::copy_term(LispVal term) {
    /**
     * Preserves sharing: the same unbound variable seen twice maps to the
     * same fresh copy.
     */
    std::unordered_map<ObjectId, LispVal> memo;

    /**
     * The source term (popped by OpCode::CopyTerm) and every partial copy are
     * held only in C++ locals until the final object is built; root them in
     * temp_roots_ for the duration of the walk.
     */
    const auto temp_root_mark = temp_roots_.size();
    temp_roots_.push_back(term);

    /// Recursive copy worker (as a std::function to allow self-reference).
    std::function<std::expected<LispVal, RuntimeError>(LispVal)> walk;
    walk = [&](LispVal v) -> std::expected<LispVal, RuntimeError> {
        v = deref(v);

        /// Check if this is a heap object
        if (!ops::is_boxed(v) || ops::tag(v) != Tag::HeapObject)
            return v;

        auto id = ops::payload(v);

        if (auto* lv = heap_.try_get_as<ObjectKind::LogicVar, types::LogicVar>(id)) {
            if (!lv->binding.has_value()) {
                auto it = memo.find(id);
                if (it != memo.end()) return it->second;
                auto fresh = make_logic_var(heap_);
                if (!fresh) return std::unexpected(fresh.error());
                temp_roots_.push_back(*fresh);
                memo[id] = *fresh;
                return *fresh;
            }
            /// but handle gracefully.
            return walk(*lv->binding);
        }

        if (auto* cons = heap_.try_get_as<ObjectKind::Cons, types::Cons>(id)) {
            auto car_copy = walk(cons->car);
            if (!car_copy) return car_copy;
            temp_roots_.push_back(*car_copy);
            auto cdr_copy = walk(cons->cdr);
            if (!cdr_copy) return cdr_copy;
            temp_roots_.push_back(*cdr_copy);
            return make_cons(heap_, *car_copy, *cdr_copy);
        }

        if (auto* vec = heap_.try_get_as<ObjectKind::Vector, types::Vector>(id)) {
            std::vector<LispVal> elems;
            elems.reserve(vec->elements.size());
            for (auto& elem : vec->elements) {
                auto elem_copy = walk(elem);
                if (!elem_copy) return elem_copy;
                temp_roots_.push_back(*elem_copy);
                elems.push_back(*elem_copy);
            }
            return make_vector(heap_, std::move(elems));
        }

        if (auto* ct = heap_.try_get_as<ObjectKind::CompoundTerm, types::CompoundTerm>(id)) {
            std::vector<LispVal> args;
            args.reserve(ct->args.size());
            for (auto& a : ct->args) {
                auto a_copy = walk(a);
                if (!a_copy) return a_copy;
                temp_roots_.push_back(*a_copy);
                args.push_back(*a_copy);
            }
            return make_compound(heap_, ct->functor, std::move(args));
        }

        return v;
    };

    auto result = walk(term);
    temp_roots_.resize(temp_root_mark);
    return result;
}

} ///< namespace eta::runtime::vm

