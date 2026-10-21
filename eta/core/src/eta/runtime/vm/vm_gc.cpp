/// VM garbage collection: root marking, finalizers and GC root enumeration.
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

void VM::collect_garbage() {
    if (!gc_) return;

    gc_collections_++;

    gc_->collect(heap_, [&](auto&& visit) {
        auto mark_execution_state =
            [&](const std::vector<LispVal>& stack,
                const std::vector<Frame>& frames,
                const std::vector<WindFrame>& winding_stack,
                const std::vector<LispVal>& temp_roots,
                const std::vector<CatchFrame>& catch_stack,
                const std::optional<PendingExceptionTransfer>& pending_exception_transfer,
                const std::vector<LispVal>& pending_unwind_thunks,
                LispVal current_closure) {
                for (auto v : stack) visit(v);
                visit(current_closure);
                for (const auto& f : frames) {
                    visit(f.closure);
                    visit(f.extra);
                }
                for (const auto& w : winding_stack) {
                    visit(w.before);
                    visit(w.body);
                    visit(w.after);
                }
                for (auto v : temp_roots) visit(v);
                for (const auto& cf : catch_stack) {
                    visit(cf.tag);
                    visit(cf.closure);
                }
                if (pending_exception_transfer) {
                    visit(pending_exception_transfer->closure);
                    visit(pending_exception_transfer->payload);
                }
                for (auto thunk : pending_unwind_thunks) visit(thunk);
            };

        mark_execution_state(
            stack_,
            frames_,
            winding_stack_,
            temp_roots_,
            catch_stack_,
            pending_exception_transfer_,
            pending_unwind_thunks_,
            current_closure_);

        for (const auto& snapshot : saved_executions_) {
            mark_execution_state(
                snapshot.stack,
                snapshot.frames,
                snapshot.winding_stack,
                snapshot.temp_roots,
                snapshot.catch_stack,
                snapshot.pending_exception_transfer,
                snapshot.pending_unwind_thunks,
                snapshot.current_closure);
        }

        /// Mark globals
        for (auto v : globals_) visit(v);
        /// Mark external roots registered by non-VM heap reconstruction helpers
        for (auto v : heap_.external_roots()) visit(v);
        /// Mark long-lived roots pinned by host-side holders
        heap_.for_each_pinned_root([&](LispVal v) { visit(v); });
        /// Mark current ports
        visit(current_input_);
        visit(current_output_);
        visit(current_error_);
        /**
         * Mark logic-variable trail (prevents live unbound vars from being swept
         * during an active unification / backtracking context)
         */
        for (const auto& e : trail_stack_) {
            visit(e.var);
            if (e.kind == TrailEntry::Kind::Attr) visit(e.prev_value);
        }
        /// Posted CLP(R) constraints keep participating vars alive.
        for (auto id : real_store_.participating_vars()) {
            visit(ops::box(Tag::HeapObject, static_cast<int64_t>(id)));
        }
        /// Asserted simplex-bound snapshots also pin logic vars.
        for (auto id : real_store_.simplex_bound_vars()) {
            visit(ops::box(Tag::HeapObject, static_cast<int64_t>(id)));
        }
        /// Attr-unify-hook procedures are VM-lifetime roots.
        for (const auto& [_k, hook] : attr_unify_hooks_) visit(hook);
        /// Pending propagator thunks.
        for (auto v : prop_queue_) visit(v);
        /// WAM register files live in the same GC root set.
        for (auto v : wam_x_regs_) visit(v);
        for (auto v : wam_y_regs_) visit(v);
        /// Mark active AD tape stack
        for (auto v : active_tapes_) visit(v);
    });
}

void VM::drain_finalizers_for_test() {
    while (heap_.pending_finalizer_count() != 0u) {
        process_pending_finalizers((std::numeric_limits<std::size_t>::max)());
    }
}

void VM::collect_garbage_and_drain_finalizers_for_test() {
    collect_garbage();
    drain_finalizers_for_test();
}

void VM::process_pending_finalizers(const std::size_t budget) {
    if (processing_finalizers_ || budget == 0) return;
    if (heap_.pending_finalizer_count() == 0u) return;

    processing_finalizers_ = true;
    struct FinalizerProcessingReset {
        bool& flag;
        ~FinalizerProcessingReset() { flag = false; }
    } reset{processing_finalizers_};

    std::size_t processed = 0;
    while (processed < budget) {
        auto pending = heap_.dequeue_pending_finalizer();
        if (!pending.has_value()) break;

        const auto temp_root_mark = temp_roots_.size();
        temp_roots_.push_back(pending->obj);
        temp_roots_.push_back(pending->proc);

        auto call_res = call_value(pending->proc, {pending->obj});
        temp_roots_.resize(temp_root_mark);

        /**
         * Keep finalizer failure isolated from the VM host call path.
         * Continue draining subsequent entries in the same safe-point pass.
         */
        if (!call_res && debug_) {
            debug_->notify_exception(runtime_error_message(call_res.error()), reader::lexer::Span{});
        }

        ++processed;
    }
}

std::vector<GCRootInfo> VM::enumerate_gc_roots() const {
    using namespace nanbox;

    auto collect = [](auto begin, auto end) {
        std::vector<ObjectId> ids;
        for (auto it = begin; it != end; ++it) {
            LispVal v = *it;
            if (ops::is_boxed(v) && ops::tag(v) == Tag::HeapObject)
                ids.push_back(static_cast<ObjectId>(ops::payload(v)));
        }
        return ids;
    };

    auto single = [](LispVal v) -> std::vector<ObjectId> {
        if (ops::is_boxed(v) && ops::tag(v) == Tag::HeapObject)
            return {static_cast<ObjectId>(ops::payload(v))};
        return {};
    };

    std::vector<GCRootInfo> roots;

    roots.push_back({"Stack", collect(stack_.begin(), stack_.end())});
    roots.push_back({"Globals", collect(globals_.begin(), globals_.end())});

    {
        auto ids = single(current_closure_);
        if (!ids.empty()) roots.push_back({"Current Closure", std::move(ids)});
    }

    {
        std::vector<ObjectId> ids;
        for (const auto& f : frames_) {
            if (ops::is_boxed(f.closure) && ops::tag(f.closure) == Tag::HeapObject)
                ids.push_back(static_cast<ObjectId>(ops::payload(f.closure)));
            if (ops::is_boxed(f.extra) && ops::tag(f.extra) == Tag::HeapObject)
                ids.push_back(static_cast<ObjectId>(ops::payload(f.extra)));
        }
        roots.push_back({"Call Frames", std::move(ids)});
    }

    {
        std::vector<ObjectId> ids;
        for (const auto& w : winding_stack_) {
            for (LispVal v : {w.before, w.body, w.after}) {
                if (ops::is_boxed(v) && ops::tag(v) == Tag::HeapObject)
                    ids.push_back(static_cast<ObjectId>(ops::payload(v)));
            }
        }
        roots.push_back({"Winding Stack", std::move(ids)});
    }

    roots.push_back({"Temp Roots", collect(temp_roots_.begin(), temp_roots_.end())});

    {
        std::vector<ObjectId> ids;
        for (LispVal v : {current_input_, current_output_, current_error_}) {
            if (ops::is_boxed(v) && ops::tag(v) == Tag::HeapObject)
                ids.push_back(static_cast<ObjectId>(ops::payload(v)));
        }
        roots.push_back({"I/O Ports", std::move(ids)});
    }

    {
        std::vector<ObjectId> ids;
        for (const auto& cf : catch_stack_) {
            if (ops::is_boxed(cf.tag) && nanbox::ops::tag(cf.tag) == Tag::HeapObject)
                ids.push_back(static_cast<ObjectId>(ops::payload(cf.tag)));
            if (ops::is_boxed(cf.closure) && nanbox::ops::tag(cf.closure) == Tag::HeapObject)
                ids.push_back(static_cast<ObjectId>(ops::payload(cf.closure)));
        }
        roots.push_back({"Catch Stack", std::move(ids)});
    }

    {
        std::vector<ObjectId> ids;
        if (pending_exception_transfer_) {
            for (LispVal v : {pending_exception_transfer_->closure, pending_exception_transfer_->payload}) {
                if (ops::is_boxed(v) && ops::tag(v) == Tag::HeapObject) {
                    ids.push_back(static_cast<ObjectId>(ops::payload(v)));
                }
            }
        }
        for (LispVal v : pending_unwind_thunks_) {
            if (ops::is_boxed(v) && ops::tag(v) == Tag::HeapObject) {
                ids.push_back(static_cast<ObjectId>(ops::payload(v)));
            }
        }
        if (!ids.empty()) roots.push_back({"Exception Transfer", std::move(ids)});
    }

    {
        std::vector<ObjectId> ids;
        for (const auto& snapshot : saved_executions_) {
            auto append = [&](LispVal v) {
                if (ops::is_boxed(v) && ops::tag(v) == Tag::HeapObject) {
                    ids.push_back(static_cast<ObjectId>(ops::payload(v)));
                }
            };

            for (auto v : snapshot.stack) append(v);
            append(snapshot.current_closure);
            for (const auto& f : snapshot.frames) {
                append(f.closure);
                append(f.extra);
            }
            for (const auto& w : snapshot.winding_stack) {
                append(w.before);
                append(w.body);
                append(w.after);
            }
            for (auto v : snapshot.temp_roots) append(v);
            for (const auto& cf : snapshot.catch_stack) {
                append(cf.tag);
                append(cf.closure);
            }
            if (snapshot.pending_exception_transfer) {
                append(snapshot.pending_exception_transfer->closure);
                append(snapshot.pending_exception_transfer->payload);
            }
            for (auto thunk : snapshot.pending_unwind_thunks) append(thunk);
        }
        if (!ids.empty()) roots.push_back({"Saved Executions", std::move(ids)});
    }

    roots.push_back({"Trail Stack", [&]() {
        std::vector<ObjectId> ids;
        ids.reserve(trail_stack_.size());
        for (const auto& e : trail_stack_) {
            if (ops::is_boxed(e.var) && ops::tag(e.var) == Tag::HeapObject)
                ids.push_back(static_cast<ObjectId>(ops::payload(e.var)));
            if (e.kind == TrailEntry::Kind::Attr &&
                ops::is_boxed(e.prev_value) && ops::tag(e.prev_value) == Tag::HeapObject)
                ids.push_back(static_cast<ObjectId>(ops::payload(e.prev_value)));
        }
        return ids;
    }()});

    {
        auto ids = real_store_.participating_vars();
        if (!ids.empty()) roots.push_back({"Real Store", std::move(ids)});
    }

    {
        auto ids = real_store_.simplex_bound_vars();
        if (!ids.empty()) roots.push_back({"Simplex Bounds", std::move(ids)});
    }

    {
        auto ids = collect(active_tapes_.begin(), active_tapes_.end());
        if (!ids.empty()) roots.push_back({"Active Tapes", std::move(ids)});
    }

    {
        auto ids = collect(prop_queue_.begin(), prop_queue_.end());
        if (!ids.empty()) roots.push_back({"Propagation Queue", std::move(ids)});
    }

    {
        auto ids = collect(wam_x_regs_.begin(), wam_x_regs_.end());
        if (!ids.empty()) roots.push_back({"WAM X Registers", std::move(ids)});
    }

    {
        auto ids = collect(wam_y_regs_.begin(), wam_y_regs_.end());
        if (!ids.empty()) roots.push_back({"WAM Y Registers", std::move(ids)});
    }

    return roots;
}
} ///< namespace eta::runtime::vm

