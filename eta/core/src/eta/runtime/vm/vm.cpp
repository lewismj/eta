#include "vm.h"
#include <algorithm>
#include <iostream>
#include "eta/runtime/factory.h"
#include "eta/runtime/types/types.h"
#include "eta/runtime/types/logic_var.h"
#include "eta/runtime/types/primitive.h"
#include "eta/runtime/types/tape.h"
#include "eta/runtime/memory/value_visit.h"
#include "eta/runtime/memory/mark_sweep_gc.h"
#include "eta/runtime/string_view.h"
#include "eta/runtime/numeric_value.h"
#include "eta/runtime/overflow.h"
#include "eta/runtime/port.h"
#include "eta/runtime/clp/domain.h"
#include <bit>

namespace eta::runtime::vm {

using namespace eta::runtime::memory::value_visit;

using namespace eta::runtime::memory::factory;
using namespace eta::runtime::types;

VM::VM(Heap& heap, InternTable& intern_table)
    : heap_(heap), intern_table_(intern_table), gc_(std::make_unique<memory::gc::MarkSweepGC>()) {
    stack_.reserve(1024);
    frames_.reserve(64);
    wam_x_regs_.assign(64, Nil);
    wam_y_regs_.assign(64, Nil);
    heap_.set_gc_callback([this]() { collect_garbage(); });

    /// Initialize default console ports
    auto stdin_port = std::make_shared<ConsolePort>(ConsolePort::StreamType::Input);
    auto stdout_port = std::make_shared<ConsolePort>(ConsolePort::StreamType::Output);
    auto stderr_port = std::make_shared<ConsolePort>(ConsolePort::StreamType::Error);

    auto input_result = make_port(heap_, stdin_port);
    auto output_result = make_port(heap_, stdout_port);
    auto error_result = make_port(heap_, stderr_port);

    /// Store default ports; if allocation fails, ports remain as Nil (will be handled by primitives)
    if (input_result) current_input_ = *input_result;
    if (output_result) current_output_ = *output_result;
    if (error_result) current_error_ = *error_result;
}

VM::~VM() = default;

bool VM::values_eqv(LispVal a, LispVal b) const {
    /// Fast path: bit-identical values are always equal (handles Nil, True, False, small Fixnums, same heap IDs)
    if (a == b) return true;
    
    /// If either is not boxed (raw double), they would have matched above if equal
    if (!ops::is_boxed(a) || !ops::is_boxed(b)) return false;
    
    /// For eqv?, strings and large numbers need content comparison
    
    /// 1. Strings (interned): identical interned IDs match above, so different IDs mean different strings
    if (ops::tag(a) == Tag::String) return false;

    /// 2. Heap-allocated values: use numeric classifier to handle heap-allocated fixnums
    if (ops::tag(a) == Tag::HeapObject && ops::tag(b) == Tag::HeapObject) {
        auto na = classify_numeric(a, heap_);
        auto nb = classify_numeric(b, heap_);
        if (na.is_fixnum() && nb.is_fixnum()) {
            return na.int_val == nb.int_val;
        }
    }
    
    /// Different types or non-matching non-string/non-number values
    return false;
}

void VM::collect_garbage() {
    if (!gc_) return;

    gc_collections_++;

    gc_->collect(heap_, [&](auto&& visit) {
        /// Mark stack
        for (auto v : stack_) visit(v);
        /// Mark globals
        for (auto v : globals_) visit(v);
        /// Mark current execution state
        visit(current_closure_);
        /// Mark frames
        for (const auto& f : frames_) {
            visit(f.closure);
            visit(f.extra);
        }
        /// Mark winding stack
        for (const auto& w : winding_stack_) {
            visit(w.before);
            visit(w.body);
            visit(w.after);
        }
        /// Mark temporary roots
        for (auto v : temp_roots_) visit(v);
        /// Mark external roots registered by non-VM heap reconstruction helpers
        for (auto v : heap_.external_roots()) visit(v);
        /// Mark current ports
        visit(current_input_);
        visit(current_output_);
        visit(current_error_);
        /// Mark catch frame tags and closures
        for (const auto& cf : catch_stack_) {
            visit(cf.tag);
            visit(cf.closure);
        }
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

std::expected<LispVal, RuntimeError> VM::execute(const BytecodeFunction& main) {
    /**
     * Push a sentinel frame to mark the bottom of this execution call.
     * This prevents CallCC from capturing frames above the point where execute() was called.
     */
    frames_.push_back({nullptr, 0, 0, Nil, FrameKind::Sentinel});

    current_func_ = &main;
    pc_ = 0;
    fp_ = 0;
    current_closure_ = 0; ///< Top-level
    
    /// Initial stack resize for main
    stack_.resize(main.stack_size, Nil);
    
    auto res = run_loop();
    if (!res) return std::unexpected(res.error());

    process_pending_finalizers();
    
    return pop();
}

std::expected<LispVal, RuntimeError> VM::call_value(LispVal proc, std::vector<LispVal> args) {
    const bool host_call = (current_func_ == nullptr);
    const auto process_finalizers_if_host = [&]() {
        if (host_call) process_pending_finalizers();
    };

    if (auto* prim = try_get_as<ObjectKind::Primitive, Primitive>(proc)) {
        uint32_t argc = static_cast<uint32_t>(args.size());
        if (prim->has_rest) {
            if (argc < prim->arity)
                {
                    process_finalizers_if_host();
                    return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::InvalidArity,
                        "call_value: expected at least " + std::to_string(prim->arity) + " argument(s), got " + std::to_string(argc)}});
                }
        } else {
            if (argc != prim->arity)
                {
                    process_finalizers_if_host();
                    return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::InvalidArity,
                        "call_value: expected " + std::to_string(prim->arity) + " argument(s), got " + std::to_string(argc)}});
                }
        }
        auto result = prim->func(args);
        process_finalizers_if_host();
        return result;
    }

    auto* cl = try_get_as<ObjectKind::Closure, Closure>(proc);
    if (!cl) {
        process_finalizers_if_host();
        return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError,
            "call_value: argument is not a procedure"}});
    }

    uint32_t argc = static_cast<uint32_t>(args.size());
    if (cl->func->has_rest) {
        if (argc < cl->func->arity)
            {
                process_finalizers_if_host();
                return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::InvalidArity,
                    "call_value: expected at least " + std::to_string(cl->func->arity) + " argument(s), got " + std::to_string(argc)}});
            }
    } else {
        if (argc != cl->func->arity)
            {
                process_finalizers_if_host();
                return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::InvalidArity,
                    "call_value: expected " + std::to_string(cl->func->arity) + " argument(s), got " + std::to_string(argc)}});
            }
    }

    /// Save full outer execution context
    const BytecodeFunction* saved_func    = current_func_;
    const uint32_t          saved_pc      = pc_;
    const uint32_t          saved_fp      = fp_;
    const LispVal           saved_closure = current_closure_;
    const auto              saved_frames  = frames_.size();
    const uint32_t          nested_sp     = static_cast<uint32_t>(stack_.size());
    const auto              saved_temp_roots = temp_roots_.size();

    /**
     * Protect proc from GC for the entire closure execution.
     * Tail calls inside the closure body may overwrite current_closure_
     * and reuse the frame, leaving proc unreachable from normal GC roots
     * while C++ callers (e.g. for-each, map) still hold a reference to it.
     */
    temp_roots_.push_back(proc);

    /// Helper: restore outer state and truncate any accumulated frames
    auto restore = [&]() {
        stack_.resize(nested_sp);
        frames_.resize(saved_frames);
        current_func_    = saved_func;
        pc_              = saved_pc;
        fp_              = saved_fp;
        current_closure_ = saved_closure;
        temp_roots_.resize(saved_temp_roots);
    };

    /**
     * Temporarily null out execution state so that setup_frame saves a
     * "return-to-null" frame.  When the closure eventually returns to this
     * frame, handle_return sets current_func_ = nullptr and run_loop() exits.
     */
    current_func_    = nullptr;
    pc_              = 0;
    fp_              = nested_sp;
    current_closure_ = Nil;

    /// Push arguments then let setup_frame initialise the closure's frame.
    for (const auto& a : args) push(a);

    /**
     * setup_frame saves {nullptr, 0, nested_sp, Nil, Normal} as the return
     * frame and sets current_func_ = cl->func, fp_ = nested_sp, pc_ = 0.
     */
    setup_frame(cl->func, proc, argc);

    /// Pack variadic rest args into a list if the closure uses &rest.
    if (cl->func->has_rest) {
        auto pr = pack_rest_args(argc, cl->func->arity);
        if (!pr) {
            restore();
            process_finalizers_if_host();
            return std::unexpected(pr.error());
        }
    }

    /// Run until current_func_ becomes null (our return-to-null frame popped).
    auto run_res = run_loop();

    if (!run_res) {
        restore();
        process_finalizers_if_host();
        return std::unexpected(run_res.error());
    }

    /// On success: stack has nested_sp items + 1 result on top.
    LispVal result = pop();
    stack_.resize(nested_sp); ///< safety: ensure no stray stack entries

    /// Restore the outer execution context.
    current_func_    = saved_func;
    pc_              = saved_pc;
    fp_              = saved_fp;
    current_closure_ = saved_closure;
    temp_roots_.resize(saved_temp_roots);

    process_finalizers_if_host();
    return result;
}

std::expected<LispVal, RuntimeError> VM::tape_binary_op(OpCode op, LispVal a, LispVal b) {
    auto saved_size = stack_.size();
    push(a);
    push(b);
    auto r = do_binary_arithmetic(op);
    if (!r) {
        stack_.resize(saved_size);
        return std::unexpected(r.error());
    }
    return pop();
}


/// Unified helper to dispatch a callee (Closure, Continuation, or Primitive)
std::expected<DispatchResult, RuntimeError> VM::dispatch_callee(
    LispVal callee, uint32_t argc, bool is_tail, reader::lexer::Span call_span) {
    /// Use try_get_as for consistent heap access pattern
    auto route_runtime_error = [&](RuntimeError err) -> std::expected<DispatchResult, RuntimeError> {
        auto handled = do_runtime_error(err, call_span);
        if (!handled) return std::unexpected(handled.error());
        return DispatchResult{DispatchAction::NonLocalTransfer, nullptr, 0};
    };

    if (!ops::is_boxed(callee)) {
        return route_runtime_error(RuntimeError{VMError{RuntimeErrorCode::TypeError, "Callee is not boxed"}});
    }
    
    if (auto* closure = try_get_as<ObjectKind::Closure, Closure>(callee)) {
        /// Arity check early (fail fast)
        if (closure->func->has_rest) {
            if (argc < closure->func->arity) {
                return route_runtime_error(RuntimeError{VMError{RuntimeErrorCode::InvalidArity,
                    "Wrong number of arguments: expected at least " + std::to_string(closure->func->arity) + ", got " + std::to_string(argc)}});
            }
        } else {
            if (argc != closure->func->arity) {
                return route_runtime_error(RuntimeError{VMError{RuntimeErrorCode::InvalidArity,
                    "Wrong number of arguments: expected " + std::to_string(closure->func->arity) + ", got " + std::to_string(argc)}});
            }
        }
        return DispatchResult{
            is_tail ? DispatchAction::TailReuse : DispatchAction::SetupFrame,
            closure->func,
            callee
        };
    }

    if (auto* cont = try_get_as<ObjectKind::Continuation, Continuation>(callee)) {
        if (argc != 1) {
            return route_runtime_error(RuntimeError{VMError{RuntimeErrorCode::InvalidArity, "Continuation expects 1 argument"}});
        }

        LispVal v = pop(); ///< The argument
        temp_roots_.push_back(v);
        temp_roots_.push_back(callee);

        /// Calculate the common ancestor of the current and target winding stacks.
        size_t common = 0;
        size_t min_len = std::min(winding_stack_.size(), cont->winding_stack.size());
        while (common < min_len &&
               winding_stack_[common].before == cont->winding_stack[common].before &&
               winding_stack_[common].after == cont->winding_stack[common].after) {
            common++;
        }

        if (common == winding_stack_.size() && common == cont->winding_stack.size()) {
            /// Find current sentinel to define the boundary
            int32_t sentinel_idx = -1;
            for (int32_t i = static_cast<int32_t>(frames_.size()) - 1; i >= 0; --i) {
                if (frames_[i].kind == FrameKind::Sentinel) {
                    sentinel_idx = i;
                    break;
                }
            }

            /// Restore state immediately if no winding/unwinding is needed.
            stack_ = cont->stack;
            
            /// Reconstruct frames: current sentinel and below, then the captured frames.
            if (sentinel_idx != -1) {
                frames_.resize(sentinel_idx + 1);
                frames_.insert(frames_.end(), cont->frames.begin(), cont->frames.end());
            } else {
                frames_ = cont->frames;
            }
            
            winding_stack_ = cont->winding_stack;

            if (debug_) debug_->notify_continuation_jump();

            if (frames_.empty()) {
                push(v);
                current_func_ = nullptr;
            } else {
                const auto f = frames_.back();
                frames_.pop_back();
                restore_frame(f);
                push(v);
            }
        } else {
            /// Prepare the list of thunks to be executed.
            std::vector<LispVal> thunks;
            for (int32_t i = static_cast<int32_t>(winding_stack_.size()) - 1; i >= static_cast<int32_t>(common); --i) {
                thunks.push_back(winding_stack_[i].after);
            }
            for (size_t i = common; i < cont->winding_stack.size(); ++i) {
                thunks.push_back(cont->winding_stack[i].before);
            }

            /**
             * Store the jump state in a Vector to be GC-rootable.
             * Format: [target_continuation, value_to_pass, next_thunk_index, ...thunks]
             */
            std::vector<LispVal> state_els;
            state_els.push_back(callee);
            state_els.push_back(v);
            state_els.push_back(ops::encode<int64_t>(1).value()); ///< Index of next thunk (after the first one)
            for (auto t : thunks) state_els.push_back(t);

            auto state_vec = make_vector(heap_, std::move(state_els));
            if (!state_vec) return std::unexpected(state_vec.error());

            /**
             * Start the sequence by calling the first thunk.
             * The thunk's return frame must be ContinuationJump so that
             * handle_return processes the continuation jump chain when the thunk returns.
             */
            LispVal first_thunk = thunks[0];
            auto dispatch = dispatch_callee(first_thunk, 0, false);
            if (!dispatch) return std::unexpected(dispatch.error());
            if (dispatch->action == DispatchAction::SetupFrame) {
                setup_frame(dispatch->func, dispatch->closure, 0, FrameKind::ContinuationJump, *state_vec);
            } else if (dispatch->action == DispatchAction::NonLocalTransfer) {
                return DispatchResult{DispatchAction::NonLocalTransfer, nullptr, 0};
            } else {
                /// Primitive thunk: push ContinuationJump frame for handle_return to process.
                frames_.push_back({current_func_, pc_, fp_, current_closure_, FrameKind::ContinuationJump, *state_vec});
                auto res = handle_return(Nil);
                if (!res) return std::unexpected(res.error());
            }
        }

        temp_roots_.pop_back();
        temp_roots_.pop_back();
        return DispatchResult{DispatchAction::Continue, nullptr, 0};
    }

    if (auto* prim = try_get_as<ObjectKind::Primitive, Primitive>(callee)) {
        /// Arity check for primitives
        if (prim->has_rest) {
            if (argc < prim->arity) {
                return route_runtime_error(RuntimeError{VMError{RuntimeErrorCode::InvalidArity,
                    "Primitive expects at least " + std::to_string(prim->arity) + " argument(s), got " + std::to_string(argc)}});
            }
        } else {
            if (argc != prim->arity) {
                return route_runtime_error(RuntimeError{VMError{RuntimeErrorCode::InvalidArity,
                    "Primitive expects " + std::to_string(prim->arity) + " argument(s), got " + std::to_string(argc)}});
            }
        }

        std::vector<LispVal> args;
        for (uint32_t i = 0; i < argc; ++i) {
            args.push_back(stack_[stack_.size() - argc + i]);
        }
        stack_.resize(stack_.size() - argc);

        auto res = prim->func(args);
        if (!res) {
            auto handled = do_runtime_error(res.error(), call_span);
            if (!handled) return std::unexpected(handled.error());
            return DispatchResult{DispatchAction::NonLocalTransfer, nullptr, 0};
        }
        push(*res);
        return DispatchResult{DispatchAction::Continue, nullptr, 0};
    }

    return route_runtime_error(RuntimeError{VMError{RuntimeErrorCode::TypeError, "Callee is not a function"}});
}

void VM::unpack_to_stack(LispVal value) {
    if (auto* mv = try_get_as<ObjectKind::MultipleValues, types::MultipleValues>(value)) {
        for (const auto& v : mv->vals) {
            push(v);
        }
    } else {
        push(value);
    }
}

std::expected<void, RuntimeError> VM::run_loop() {
    while (current_func_ && pc_ < current_func_->code.size()) {
        if (heap_.pending_finalizer_count() != 0u) {
            process_pending_finalizers();
        }

        /**
         * Debug hook
         * Zero-cost when debug_ is null (non-debug runs).
         */
        if (debug_) {
            const auto sp = current_func_->span_at(pc_);
            debug_->check_and_wait(sp, frames_.size());
        }
        const auto& instr = current_func_->code[pc_++];
        const auto instr_span = current_func_ ? current_func_->span_at(pc_ > 0 ? pc_ - 1 : 0)
                                              : reader::lexer::Span{};
        switch (instr.opcode) {
            case OpCode::Nop:
                break;
            case OpCode::StoreLocal:
                if (fp_ + instr.arg >= stack_.size()) [[unlikely]]
                    return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::InvalidInstruction, "StoreLocal: slot out of range"}});
                stack_[fp_ + instr.arg] = pop();
                break;
            case OpCode::Values: {
                /// [n] -> pack top n values into a MultipleValues object
                uint32_t n = instr.arg;
                if (n == 1) {
                    /// Single value: leave it as-is (no wrapping needed)
                    break;
                }
                std::vector<LispVal> vals;
                vals.reserve(n);
                /// Pop values in reverse order to maintain correct ordering
                for (uint32_t i = 0; i < n; ++i) {
                    vals.push_back(pop());
                }
                std::reverse(vals.begin(), vals.end());
                auto mv = make_multiple_values(heap_, std::move(vals));
                if (!mv) return std::unexpected(mv.error());
                push(*mv);
                break;
            }
            case OpCode::CallWithValues: {
                /// Stack: [producer, consumer] (consumer on top)
                LispVal consumer = pop();
                LispVal producer = pop();

                /// Call producer with 0 arguments. Return to consumer afterwards.
                auto prod_dispatch = dispatch_callee(producer, 0, /*is_tail=*/false, instr_span);
                if (!prod_dispatch) return std::unexpected(prod_dispatch.error());

                if (prod_dispatch->action == DispatchAction::SetupFrame) {
                    setup_frame(prod_dispatch->func, prod_dispatch->closure, 0, FrameKind::CallWithValuesConsumer, consumer);
                } else if (prod_dispatch->action == DispatchAction::NonLocalTransfer) {
                    break;
                } else {
                    /**
                     * Producer was a primitive - result already pushed.
                     * We can call consumer directly now.
                     */
                    uint32_t old_size = static_cast<uint32_t>(stack_.size());
                    unpack_to_stack(pop());
                    uint32_t argc = static_cast<uint32_t>(stack_.size() - old_size);
                    auto cons_dispatch = dispatch_callee(consumer, argc, false, instr_span);
                    if (!cons_dispatch) return std::unexpected(cons_dispatch.error());
                    if (cons_dispatch->action == DispatchAction::SetupFrame) {
                        setup_frame(cons_dispatch->func, cons_dispatch->closure, argc);
                    } else if (cons_dispatch->action == DispatchAction::NonLocalTransfer) {
                        break;
                    }
                }
                break;
            }
            case OpCode::DynamicWind: {
                /// Stack: [before, body, after] (after on top)
                LispVal after_thunk = pop();
                LispVal body_thunk = pop();
                LispVal before_thunk = pop();

                winding_stack_.push_back({before_thunk, body_thunk, after_thunk});

                /// Call before() with 0 arguments. Return to body() afterwards.
                temp_roots_.push_back(before_thunk);
                auto before_dispatch = dispatch_callee(before_thunk, 0, /*is_tail=*/false, instr_span);
                temp_roots_.pop_back();
                if (!before_dispatch) return std::unexpected(before_dispatch.error());

                if (before_dispatch->action == DispatchAction::SetupFrame) {
                    setup_frame(before_dispatch->func, before_dispatch->closure, 0, FrameKind::DynamicWindBody);
                } else if (before_dispatch->action == DispatchAction::NonLocalTransfer) {
                    break;
                } else {
                    /// before was primitive. Call body() now.
                    auto body_dispatch = dispatch_callee(body_thunk, 0, false, instr_span);
                    if (!body_dispatch) return std::unexpected(body_dispatch.error());
                    if (body_dispatch->action == DispatchAction::SetupFrame) {
                        setup_frame(body_dispatch->func, body_dispatch->closure, 0, FrameKind::DynamicWindAfter, after_thunk);
                    } else if (body_dispatch->action == DispatchAction::NonLocalTransfer) {
                        break;
                    } else {
                        /// body was primitive. Call after() now.
                        LispVal body_res = pop();
                        auto after_dispatch = dispatch_callee(after_thunk, 0, false, instr_span);
                        if (!after_dispatch) return std::unexpected(after_dispatch.error());
                        if (after_dispatch->action == DispatchAction::SetupFrame) {
                            setup_frame(after_dispatch->func, after_dispatch->closure, 0, FrameKind::DynamicWindCleanup, body_res);
                        } else if (after_dispatch->action == DispatchAction::NonLocalTransfer) {
                            break;
                        } else {
                            pop(); ///< ignore after() result
                            winding_stack_.pop_back();
                            push(body_res);
                        }
                    }
                }
                break;
            }
            case OpCode::LoadConst: {
                if (instr.arg < current_func_->constants.size()) {
                    push(current_func_->constants[instr.arg]);
                } else {
                    return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::InvalidInstruction, "Constant index out of bounds"}});
                }
                break;
            }
            case OpCode::LoadLocal:
                if (fp_ + instr.arg >= stack_.size()) [[unlikely]]
                    return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::InvalidInstruction, "LoadLocal: slot out of range"}});
                push(stack_[fp_ + instr.arg]);
                break;
            case OpCode::LoadUpval: {
                auto closure = get_as_or_error<ObjectKind::Closure, Closure>(current_closure_, "LoadUpval outside of a closure");
                if (!closure) return std::unexpected(closure.error());
                if (instr.arg >= (*closure)->upvals.size()) [[unlikely]]
                    return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::InvalidInstruction, "LoadUpval: index out of range"}});
                push((*closure)->upvals[instr.arg]);
                break;
            }
            case OpCode::StoreUpval: {
                auto closure = get_as_or_error<ObjectKind::Closure, Closure>(current_closure_, "StoreUpval outside of a closure");
                if (!closure) return std::unexpected(closure.error());
                if (instr.arg >= (*closure)->upvals.size()) [[unlikely]]
                    return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::InvalidInstruction, "StoreUpval: index out of range"}});
                (*closure)->upvals[instr.arg] = pop();
                break;
            }
            case OpCode::LoadGlobal:
                if (instr.arg < globals_.size()) {
                    push(globals_[instr.arg]);
                } else {
                    return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::UndefinedGlobal, "Global index out of range"}});
                }
                break;
            case OpCode::StoreGlobal:
                if (instr.arg >= globals_.size()) {
                    globals_.resize(instr.arg + 1, Nil);
                }
                globals_[instr.arg] = pop();
                break;
            case OpCode::Pop:
                pop();
                break;
            case OpCode::Dup:
                push(stack_.back());
                break;
            case OpCode::Jump:
                pc_ += instr.arg;
                break;
            case OpCode::JumpIfFalse: {
                LispVal v = pop();
                if (v == False) pc_ += instr.arg;
                break;
            }
            case OpCode::Call: {
                uint32_t argc = instr.arg;
                LispVal callee = pop();

                temp_roots_.push_back(callee);
                auto dispatch_res = dispatch_callee(callee, argc, /*is_tail=*/false, instr_span);
                temp_roots_.pop_back();
                if (!dispatch_res) return std::unexpected(dispatch_res.error());

                if (dispatch_res->action == DispatchAction::SetupFrame) {
                    setup_frame(dispatch_res->func, dispatch_res->closure, argc);
                    if (dispatch_res->func->has_rest) {
                        auto r = pack_rest_args(argc, dispatch_res->func->arity);
                        if (!r) return std::unexpected(r.error());
                    }
                }
                break;
            }
            case OpCode::MakeClosure: {
                /// arg is (const_idx << 16) | num_upvals
                uint32_t const_idx = instr.arg >> 16;
                uint32_t num_upvals = instr.arg & 0xFFFF;

                if (const_idx >= current_func_->constants.size()) [[unlikely]]
                    return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::InvalidInstruction, "MakeClosure: constant index out of range"}});

                /// The constant encodes a function index (high bit set) or legacy raw pointer
                LispVal func_val = current_func_->constants[const_idx];

                const BytecodeFunction* bfunc = nullptr;
                if (is_func_index(func_val)) {
                    /// New-style: function index
                    uint32_t func_idx = decode_func_index(func_val);
                    bfunc = resolve_function(func_idx);
                    if (!bfunc) {
                        return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::InvalidInstruction, "Invalid function index"}});
                    }
                } else {
                    return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::InvalidInstruction, "Expected function index (FUNC_INDEX_TAG missing)"}});
                }

                std::vector<LispVal> upvals;
                for (uint32_t i = 0; i < num_upvals; ++i) {
                    upvals.push_back(pop());
                }
                std::reverse(upvals.begin(), upvals.end());
                
                auto closure_res = make_closure(heap_, bfunc, std::move(upvals));
                if (!closure_res) return std::unexpected(closure_res.error());
                push(*closure_res);
                break;
            }
            case OpCode::PatchClosureUpval: {
                /// Fixup for letrec: patch a closure's captured upval after set!
                LispVal value = pop();
                LispVal closure_val = pop();
                auto* closure = try_get_as<ObjectKind::Closure, Closure>(closure_val);
                if (closure && instr.arg < closure->upvals.size()) {
                    closure->upvals[instr.arg] = value;
                }
                break;
            }
            case OpCode::Cons: {
                LispVal cdr = pop();
                LispVal car = pop();
                auto res = make_cons(heap_, car, cdr);
                if (!res) return std::unexpected(res.error());
                push(*res);
                break;
            }
            case OpCode::Car: {
                auto v = pop();
                auto cons = get_as_or_error<ObjectKind::Cons, Cons>(v, "Not a pair");
                if (!cons) return std::unexpected(cons.error());
                push((*cons)->car);
                break;
            }
            case OpCode::Add:
            case OpCode::Sub:
            case OpCode::Mul:
            case OpCode::Div: {
                auto res = do_binary_arithmetic(instr.opcode);
                if (!res) return std::unexpected(res.error());
                break;
            }
            case OpCode::Eq: {
                LispVal b = pop();
                LispVal a = pop();
                push(values_eqv(a, b) ? True : False);
                break;
            }
            case OpCode::Cdr: {
                auto v = pop();
                auto cons = get_as_or_error<ObjectKind::Cons, Cons>(v, "Not a pair");
                if (!cons) return std::unexpected(cons.error());
                push((*cons)->cdr);
                break;
            }
            case OpCode::TailCall: {
                uint32_t argc = instr.arg;
                LispVal callee = pop();

                temp_roots_.push_back(callee);
                auto dispatch_res = dispatch_callee(callee, argc, /*is_tail=*/true, instr_span);
                temp_roots_.pop_back();
                if (!dispatch_res) return std::unexpected(dispatch_res.error());

                if (dispatch_res->action == DispatchAction::TailReuse) {
                    current_func_ = dispatch_res->func;
                    current_closure_ = dispatch_res->closure;

                    /// Move new arguments to current fp_
                    uint32_t new_args_start = static_cast<uint32_t>(stack_.size() - argc);
                    for (uint32_t i = 0; i < argc; ++i) {
                        stack_[fp_ + i] = stack_[new_args_start + i];
                    }
                    /// Ensure we don't chop off moved arguments if stack_size is small
                    uint32_t needed_size = std::max(current_func_->stack_size, argc);
                    stack_.resize(fp_ + needed_size, Nil);
                    pc_ = 0;

                    if (current_func_->has_rest) {
                        auto r = pack_rest_args(argc, current_func_->arity);
                        if (!r) return std::unexpected(r.error());
                    }
                } else if (dispatch_res->action == DispatchAction::Continue) {
                    /**
                     * TailCall to primitive or continuation - result already pushed.
                     * Now we MUST return that result from the current frame.
                     */
                    auto res = handle_return(pop());
                    if (!res) return std::unexpected(res.error());
                }
                break;
            }
            case OpCode::CallCC: {
                LispVal consumer = pop();
                temp_roots_.push_back(consumer);
                
                /// Capture current execution state as top frame
                auto cont_frames = frames_;
                cont_frames.push_back({current_func_, pc_, fp_, current_closure_});
                
                auto cont_res = make_continuation(heap_, stack_, cont_frames, winding_stack_);
                if (!cont_res) return std::unexpected(cont_res.error());
                
                push(*cont_res);
                
                auto dispatch_res = dispatch_callee(consumer, 1, /*is_tail=*/false, instr_span);
                temp_roots_.pop_back();
                if (!dispatch_res) return std::unexpected(dispatch_res.error());

                if (dispatch_res->action == DispatchAction::SetupFrame) {
                    setup_frame(dispatch_res->func, dispatch_res->closure, 1);
                }
                break;
            }
            case OpCode::Apply:
            case OpCode::TailApply: {
                /// Stack: [arg1, ..., argN, proc] where argN (last explicit arg) is a list to unpack
                uint32_t argc = instr.arg; ///< number of explicit args on stack (including the list)
                LispVal proc = pop();
                temp_roots_.push_back(proc);

                /// Pop explicit args
                std::vector<LispVal> explicit_args;
                explicit_args.reserve(argc);
                for (uint32_t i = 0; i < argc; ++i) {
                    explicit_args.push_back(stack_[stack_.size() - argc + i]);
                }
                stack_.resize(stack_.size() - argc);

                /// Unpack the last arg (must be a proper list)
                LispVal tail_list = explicit_args.back();
                explicit_args.pop_back();

                LispVal cur = tail_list;
                while (cur != Nil) {
                    auto* cons = try_get_as<ObjectKind::Cons, Cons>(cur);
                    if (!cons) {
                        temp_roots_.pop_back();
                        return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError, "apply: last argument must be a proper list"}});
                    }
                    explicit_args.push_back(cons->car);
                    cur = cons->cdr;
                }

                /// Push combined args back onto stack
                for (auto v : explicit_args) push(v);
                uint32_t final_argc = static_cast<uint32_t>(explicit_args.size());

                bool is_tail = (instr.opcode == OpCode::TailApply);
                auto dispatch_res = dispatch_callee(proc, final_argc, is_tail, instr_span);
                temp_roots_.pop_back();
                if (!dispatch_res) return std::unexpected(dispatch_res.error());

                if (dispatch_res->action == DispatchAction::SetupFrame) {
                    setup_frame(dispatch_res->func, dispatch_res->closure, final_argc);
                    if (dispatch_res->func->has_rest) {
                        auto r = pack_rest_args(final_argc, dispatch_res->func->arity);
                        if (!r) return std::unexpected(r.error());
                    }
                } else if (dispatch_res->action == DispatchAction::TailReuse) {
                    current_func_ = dispatch_res->func;
                    current_closure_ = dispatch_res->closure;

                    uint32_t new_args_start = static_cast<uint32_t>(stack_.size() - final_argc);
                    for (uint32_t i = 0; i < final_argc; ++i) {
                        stack_[fp_ + i] = stack_[new_args_start + i];
                    }
                    uint32_t needed_size = std::max(current_func_->stack_size, final_argc);
                    stack_.resize(fp_ + needed_size, Nil);
                    pc_ = 0;

                    if (current_func_->has_rest) {
                        auto r = pack_rest_args(final_argc, current_func_->arity);
                        if (!r) return std::unexpected(r.error());
                    }
                } else if (dispatch_res->action == DispatchAction::Continue) {
                    if (is_tail) {
                        auto res = handle_return(pop());
                        if (!res) return std::unexpected(res.error());
                    }
                }
                break;
            }
            case OpCode::Return: {
                auto res = handle_return(pop());
                if (!res) return std::unexpected(res.error());
                break;
            }
            case OpCode::SetupCatch: {
                /// arg = (tag_const_idx << 16) | pc_offset_to_handler
                uint32_t const_idx  = instr.arg >> 16;
                uint32_t pc_offset  = instr.arg & 0xFFFF;
                LispVal  tag        = (const_idx < current_func_->constants.size())
                                      ? current_func_->constants[const_idx]
                                      : Nil;
                /// pc_ already past SetupCatch; handler_pc = pc_ + pc_offset
                CatchFrame cf;
                cf.tag         = tag;
                cf.func        = current_func_;
                cf.handler_pc  = pc_ + pc_offset;
                cf.fp          = fp_;
                cf.closure     = current_closure_;
                cf.frame_count = frames_.size();
                cf.stack_top   = static_cast<uint32_t>(stack_.size());
                cf.wind_count  = winding_stack_.size();
                cf.tape_count  = active_tapes_.size();
                catch_stack_.push_back(cf);
                break;
            }
            case OpCode::PopCatch: {
                if (!catch_stack_.empty()) catch_stack_.pop_back();
                break;
            }
            case OpCode::Throw: {
                LispVal value = pop();
                LispVal tag   = pop();
                auto sp = current_func_ ? current_func_->span_at(pc_ > 0 ? pc_ - 1 : 0)
                                        : reader::lexer::Span{};
                auto res = do_throw(tag, value, sp);
                if (!res) return std::unexpected(res.error());
                break;
            }

            /// Unification opcodes
            case OpCode::MakeLogicVar: {
                auto lv = make_logic_var(heap_);
                if (!lv) return std::unexpected(lv.error());
                push(*lv);
                break;
            }
            case OpCode::Unify: {
                LispVal b = pop();
                LispVal a = pop();
                last_unify_cycle_error_ = false;
                bool ok = unify(a, b);
                if (!ok && last_unify_cycle_error_) {
                    last_unify_cycle_error_ = false;
                    return std::unexpected(RuntimeError{VMError{
                        RuntimeErrorCode::UserError,
                        "unify: occurs-check violation (cyclic term)"}});
                }
                push(ok ? True : False);
                break;
            }
            case OpCode::DerefLogicVar: {
                push(deref(pop()));
                break;
            }
            case OpCode::TrailMark: {
                /**
                 * With the unified domain trail, a mark
                 * is just the binding-trail depth.  Domain entries live in
                 * `trail_stack_` and are unwound alongside Bind/Attr and
                 * RealStore snapshots by UnwindTrail.
                 */
                auto bsize = static_cast<int64_t>(trail_stack_.size());
                auto enc = ops::encode<int64_t>(bsize);
                if (!enc) return std::unexpected(make_type_error("trail-mark: trail too deep"));
                push(*enc);
                break;
            }
            case OpCode::UnwindTrail: {
                LispVal mark_val = pop();
                if (!ops::is_boxed(mark_val) || ops::tag(mark_val) != Tag::Fixnum)
                    return std::unexpected(make_type_error("unwind-trail: mark must be a fixnum"));
                auto mark_opt = ops::decode<int64_t>(mark_val);
                if (!mark_opt)
                    return std::unexpected(make_type_error("unwind-trail: invalid mark"));
                auto bmark = static_cast<std::size_t>(*mark_opt);
                rollback_trail_to(bmark);
                push(Nil);
                break;
            }


            case OpCode::CopyTerm: {
                LispVal term = pop();
                auto result = copy_term(term);
                if (!result) return std::unexpected(result.error());
                push(*result);
                break;
            }

            case OpCode::_Reserved1:
            case OpCode::_Reserved2:
            case OpCode::WamGetVar:
            case OpCode::WamGetVal:
            case OpCode::WamGetConst:
            case OpCode::WamGetStruct:
            case OpCode::WamGetList:
            case OpCode::WamPutVar:
            case OpCode::WamPutVal:
            case OpCode::WamPutConst:
            case OpCode::WamPutStruct:
            case OpCode::WamPutList:
            case OpCode::WamUnifyVar:
            case OpCode::WamUnifyVal:
            case OpCode::WamUnifyConst:
            case OpCode::WamUnifyVoid:
            case OpCode::WamAllocate:
            case OpCode::WamDeallocate:
            case OpCode::WamCall:
            case OpCode::WamExecute:
            case OpCode::WamProceed:
            case OpCode::WamTryMeElse:
            case OpCode::WamRetryMeElse:
            case OpCode::WamTrustMe:
            case OpCode::WamSwitchOnTerm:
            case OpCode::WamSwitchOnConst:
            case OpCode::WamSwitchOnStruct:
            default:
                return std::unexpected(RuntimeError{VMError{
                    RuntimeErrorCode::NotImplemented,
                    std::string("OpCode not implemented: ") + to_string(instr.opcode)}});
        }
    }
    return {};
}

/**
 * Numeric Helpers - Using ValueVisitor from value_visit.h
 */

/// Numeric type enumeration for dispatch
std::expected<void, RuntimeError> VM::handle_return(LispVal result) {
    if (frames_.empty()) {
        stack_.resize(fp_);
        push(result);
        current_func_ = nullptr;
        return {};
    }
    
    /// The frame we're returning to
    Frame return_frame = frames_.back();
    frames_.pop_back();

    /// Handle special return kinds
    if (return_frame.kind == FrameKind::Sentinel) {
        stack_.resize(fp_);
        push(result);
        current_func_ = nullptr;
        pc_ = 0;
        return {};
    }

    /// Pop the current frame's locals/args
    stack_.resize(fp_);
    
    /// Restore the caller's execution state
    restore_frame(return_frame);

    if (return_frame.kind == FrameKind::ContinuationJump) {
        auto* state_vec = try_get_as<ObjectKind::Vector, types::Vector>(return_frame.extra);
        if (!state_vec) return std::unexpected(make_type_error("Invalid jump state"));

        int64_t next_idx = ops::decode<int64_t>(state_vec->elements[2]).value();
        size_t thunk_count = state_vec->elements.size() - 3;

        if (static_cast<size_t>(next_idx) < thunk_count) {
            LispVal thunk = state_vec->elements[3 + static_cast<size_t>(next_idx)];
            state_vec->elements[2] = ops::encode<int64_t>(next_idx + 1).value();

            auto dispatch = dispatch_callee(thunk, 0, false);
            if (!dispatch) return std::unexpected(dispatch.error());
            if (dispatch->action == DispatchAction::SetupFrame) {
                /**
                 * Use ContinuationJump as frame kind so the next thunk's return
                 * continues the chain via handle_return's ContinuationJump handler.
                 */
                setup_frame(dispatch->func, dispatch->closure, 0, FrameKind::ContinuationJump, return_frame.extra);
            } else if (dispatch->action == DispatchAction::NonLocalTransfer) {
                return {};
            } else {
                /// Primitive thunk: re-push ContinuationJump frame and recurse
                frames_.push_back(return_frame);
                return handle_return(Nil);
            }
            return {};
        } else {
            /// All thunks executed. Now perform the final state restoration.
            LispVal target_cont_val = state_vec->elements[0];
            LispVal v = state_vec->elements[1];
            auto* cont = try_get_as<ObjectKind::Continuation, Continuation>(target_cont_val);
            if (!cont) return std::unexpected(make_type_error("Invalid target continuation"));

            /// Find current sentinel to define the boundary
            int32_t sentinel_idx = -1;
            for (int32_t i = static_cast<int32_t>(frames_.size()) - 1; i >= 0; --i) {
                if (frames_[i].kind == FrameKind::Sentinel) {
                    sentinel_idx = i;
                    break;
                }
            }

            stack_ = cont->stack;

            /// Reconstruct frames: current sentinel and below, then the captured frames.
            if (sentinel_idx != -1) {
                frames_.resize(sentinel_idx + 1);
                frames_.insert(frames_.end(), cont->frames.begin(), cont->frames.end());
            } else {
                frames_ = cont->frames;
            }

            winding_stack_ = cont->winding_stack;

            if (debug_) debug_->notify_continuation_jump();

            if (frames_.empty()) {
                push(v);
                current_func_ = nullptr;
                return {};
            }

            const auto f = frames_.back();
            frames_.pop_back();
            restore_frame(f);
            push(v);
            return {};
        }
    }

    if (return_frame.kind == FrameKind::CallWithValuesConsumer) {
        LispVal consumer = return_frame.extra;
        uint32_t old_size = static_cast<uint32_t>(stack_.size());
        unpack_to_stack(result);
        uint32_t argc = static_cast<uint32_t>(stack_.size() - old_size);
        
        auto dispatch_res = dispatch_callee(consumer, argc, false);
        if (!dispatch_res) return std::unexpected(dispatch_res.error());
        
        if (dispatch_res->action == DispatchAction::SetupFrame) {
            setup_frame(dispatch_res->func, dispatch_res->closure, argc);
        } else if (dispatch_res->action == DispatchAction::NonLocalTransfer) {
            return {};
        }
        return {};
    } else if (return_frame.kind == FrameKind::DynamicWindBody) {
        /// Returned from 'before'. result is ignored.
        if (winding_stack_.empty()) {
            return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::InternalError, "Winding stack empty"}});
        }
        const auto& wind = winding_stack_.back();
        
        auto body_dispatch = dispatch_callee(wind.body, 0, false);
        if (!body_dispatch) return std::unexpected(body_dispatch.error());
        
        if (body_dispatch->action == DispatchAction::SetupFrame) {
            setup_frame(body_dispatch->func, body_dispatch->closure, 0, FrameKind::DynamicWindAfter, wind.after);
        } else if (body_dispatch->action == DispatchAction::NonLocalTransfer) {
            return {};
        } else {
            /// body was primitive
            LispVal body_result = pop();
            auto after_dispatch = dispatch_callee(wind.after, 0, false);
            if (!after_dispatch) return std::unexpected(after_dispatch.error());
            if (after_dispatch->action == DispatchAction::SetupFrame) {
                setup_frame(after_dispatch->func, after_dispatch->closure, 0, FrameKind::DynamicWindCleanup, body_result);
            } else if (after_dispatch->action == DispatchAction::NonLocalTransfer) {
                return {};
            } else {
                pop(); ///< ignore after() result
                push(body_result);
                winding_stack_.pop_back();
            }
        }
        return {};
    } else if (return_frame.kind == FrameKind::DynamicWindAfter) {
        /// Returned from 'body'. result is the body result.
        if (winding_stack_.empty()) {
            return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::InternalError, "Winding stack empty"}});
        }
        LispVal body_result = result;
        LispVal after_thunk = return_frame.extra;

        auto after_dispatch = dispatch_callee(after_thunk, 0, false);
        if (!after_dispatch) return std::unexpected(after_dispatch.error());

        if (after_dispatch->action == DispatchAction::SetupFrame) {
            setup_frame(after_dispatch->func, after_dispatch->closure, 0, FrameKind::DynamicWindCleanup, body_result);
        } else if (after_dispatch->action == DispatchAction::NonLocalTransfer) {
            return {};
        } else {
            /// after thunk was primitive
            winding_stack_.pop_back();
            push(body_result);
        }
        return {};
    } else if (return_frame.kind == FrameKind::DynamicWindCleanup) {
        /// Returned from 'after'. return_frame.extra is the body_result.
        if (winding_stack_.empty()) {
            return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::InternalError, "Winding stack empty"}});
        }
        winding_stack_.pop_back();
        push(return_frame.extra);
        return {};
    }

    /// Normal case
    push(result);
    return {};
}

std::expected<void, RuntimeError> VM::do_binary_arithmetic(OpCode op) {
    LispVal b = pop();
    LispVal a = pop();

    /**
     * AD Tape recording
     * If a tape is active and either operand is a TapeRef, record the
     * operation on the tape and push a new TapeRef.
     * Only the four arithmetic opcodes are relevant here; the default
     * branch handles the (impossible) case where a non-arithmetic opcode
     * reaches this path.
     */
#if defined(__clang__) || defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wswitch-enum"
#endif
    const bool is_tape_a = ops::is_boxed(a) && ops::tag(a) == Tag::TapeRef;
    const bool is_tape_b = ops::is_boxed(b) && ops::tag(b) == Tag::TapeRef;

    if (is_tape_a || is_tape_b) {
        auto* tape = heap_.try_get_as<ObjectKind::Tape, Tape>(ops::payload(active_tape()));
        if (!tape) {
            return std::unexpected(make_type_error("tape arithmetic: no active tape"));
        }

        /// Map opcode to tape op
        TapeOp tape_op;
        switch (op) {
            case OpCode::Add: tape_op = TapeOp::Add; break;
            case OpCode::Sub: tape_op = TapeOp::Sub; break;
            case OpCode::Mul: tape_op = TapeOp::Mul; break;
            case OpCode::Div: tape_op = TapeOp::Div; break;
            default:
                return std::unexpected(make_type_error("tape arithmetic: unknown op"));
        }

        auto resolve_idx = [&](LispVal v, bool is_tape) -> std::expected<uint32_t, RuntimeError> {
            if (is_tape) return static_cast<uint32_t>(ops::payload(v));
            /// Promote plain number to tape constant
            auto nv = classify_numeric(v, heap_);
            if (!nv.is_valid()) return std::unexpected(make_type_error("tape arithmetic: operand is not a number"));
            return tape->push_const(nv.as_double());
        };

        auto idx_a = resolve_idx(a, is_tape_a);
        if (!idx_a) return std::unexpected(idx_a.error());
        auto idx_b = resolve_idx(b, is_tape_b);
        if (!idx_b) return std::unexpected(idx_b.error());

        double pa = tape->entries[*idx_a].primal;
        double pb = tape->entries[*idx_b].primal;

        double result;
        switch (op) {
            case OpCode::Add: result = pa + pb; break;
            case OpCode::Sub: result = pa - pb; break;
            case OpCode::Mul: result = pa * pb; break;
            case OpCode::Div:
                if (pb == 0.0) return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError, "Division by zero"}});
                result = pa / pb;
                break;
            default: result = 0.0; break;
        }

        uint32_t new_idx = tape->push({tape_op, *idx_a, *idx_b, result, 0.0});
        push(ops::box(Tag::TapeRef, new_idx));
        return {};
    }
#if defined(__clang__) || defined(__GNUC__)
#pragma GCC diagnostic pop
#endif


    auto num_a = classify_numeric(a, heap_);
    auto num_b = classify_numeric(b, heap_);

    if (!num_a.is_valid() || !num_b.is_valid()) {
        return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError, "Expected a number"}});
    }

    /// Double-precision path (used when either operand is a flonum, or on integer overflow)
    auto apply_double = [](OpCode op, double a, double b) -> std::expected<double, RuntimeError> {
        if (op == OpCode::Add) return a + b;
        if (op == OpCode::Sub) return a - b;
        if (op == OpCode::Mul) return a * b;
        if (op == OpCode::Div) {
            if (b == 0.0) return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError, "Division by zero"}});
            return a / b;
        }
        return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::NotImplemented, "Unknown arithmetic op"}});
    };

    auto push_flonum = [&](double val) -> std::expected<void, RuntimeError> {
        auto res = make_flonum(val);
        if (!res) return std::unexpected(res.error());
        push(*res);
        return {};
    };

    /// If either operand is a flonum, result is flonum
    if (num_a.is_flonum() || num_b.is_flonum()) {
        auto result = apply_double(op, num_a.as_double(), num_b.as_double());
        if (!result) return std::unexpected(result.error());
        return push_flonum(*result);
    }

    int64_t int_a = num_a.int_val;
    int64_t int_b = num_b.int_val;

    /// Division: special cases for zero and non-exact results
    if (op == OpCode::Div) {
        if (int_b == 0) {
            return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError, "Division by zero"}});
        }
        if (int_a % int_b != 0) {
            return push_flonum(static_cast<double>(int_a) / static_cast<double>(int_b));
        }
        auto res = make_fixnum(heap_, int_a / int_b);
        if (!res) return std::unexpected(res.error());
        push(*res);
        return {};
    }

    /// Add / Sub / Mul: overflow-checked, promote to double on overflow
    int64_t int_result;
    bool overflowed = false;

    if (op == OpCode::Add)       overflowed = detail::add_overflow(int_a, int_b, &int_result);
    else if (op == OpCode::Sub)  overflowed = detail::sub_overflow(int_a, int_b, &int_result);
    else if (op == OpCode::Mul)  overflowed = detail::mul_overflow(int_a, int_b, &int_result);
    else return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::NotImplemented, "Unknown arithmetic op"}});

    if (overflowed) {
        auto result = apply_double(op, static_cast<double>(int_a), static_cast<double>(int_b));
        if (!result) return std::unexpected(result.error());
        return push_flonum(*result);
    }

    auto res = make_fixnum(heap_, int_result);
    if (!res) return std::unexpected(res.error());
    push(*res);
    return {};
}

std::expected<void, RuntimeError> VM::pack_rest_args(uint32_t argc, uint32_t required) {
    /**
     * Build a list from args at fp_+required .. fp_+argc-1
     * and store the list at fp_+required (the rest param slot).
     */
    LispVal rest_list = Nil;
    for (int32_t i = static_cast<int32_t>(argc) - 1; i >= static_cast<int32_t>(required); --i) {
        auto cons = make_cons(heap_, stack_[fp_ + i], rest_list);
        if (!cons) return std::unexpected(cons.error());
        rest_list = *cons;
    }
    stack_[fp_ + required] = rest_list;
    return {};
}

/**
 * Debug / inspect API
 */


std::vector<FrameInfo> VM::get_frames() const {
    std::vector<FrameInfo> result;

    /// Current (innermost) frame
    if (current_func_) {
        FrameInfo fi;
        fi.func_name  = current_func_->name;
        /**
         * When the VM is paused at the debug hook, pc_ points to the
         * about-to-execute instruction (pre-increment).  Using pc_ - 1
         * Use the exact span saved by DebugState when it stopped.
         */
        if (debug_ && debug_->is_paused()) {
            fi.span = debug_->stopped_span();
        } else {
            fi.span = current_func_->span_at(pc_ > 0 ? pc_ - 1 : 0);
        }
        fi.frame_index = 0;
        result.push_back(fi);
    }

    /// Walk saved frames in reverse order (most recent caller first)
    for (int i = static_cast<int>(frames_.size()) - 1; i >= 0; --i) {
        const auto& f = frames_[static_cast<std::size_t>(i)];
        if (f.kind == FrameKind::Sentinel) break; ///< stop at execution boundary
        if (f.func == nullptr) continue;
        FrameInfo fi;
        fi.func_name  = f.func->name;
        /// pc in a saved frame points to the instruction *after* the call site
        uint32_t call_pc = f.pc > 0 ? f.pc - 1 : 0;
        fi.span        = f.func->span_at(call_pc);
        fi.frame_index = result.size();
        result.push_back(fi);
    }
    return result;
}

std::vector<VarEntry> VM::get_locals(std::size_t frame_index) const {
    std::vector<VarEntry> result;

    const BytecodeFunction* func = nullptr;
    uint32_t frame_fp = fp_;

    if (frame_index == 0) {
        func     = current_func_;
        frame_fp = fp_;
    } else {
        /// Walk saved frames (most recent caller first)
        std::size_t idx = 1;
        for (int i = static_cast<int>(frames_.size()) - 1; i >= 0; --i) {
            const auto& f = frames_[static_cast<std::size_t>(i)];
            if (f.kind == FrameKind::Sentinel) break;
            if (f.func == nullptr) continue;
            if (idx == frame_index) {
                func     = f.func;
                frame_fp = f.fp;
                break;
            }
            ++idx;
        }
    }

    if (!func) return result;

    uint32_t num_params = func->arity + (func->has_rest ? 1 : 0);
    /**
     * Only expose the slots that the emitter gave real names to.
     * stack_size includes +32 temporary headroom that we must not expose as
     * populated (e.g. module-init functions) fall back to num_params or, if
     * that is also zero, scan the stack for meaningful (non-Nil) values so the
     * Variables panel is not completely empty.
     */
    uint32_t num_slots;
    if (!func->local_names.empty()) {
        num_slots = static_cast<uint32_t>(func->local_names.size());
    } else if (num_params > 0) {
        num_slots = num_params;
    } else {
        /// Module-init: expose occupied stack slots up to stack_size minus headroom
        uint32_t headroom = 32;
        num_slots = func->stack_size > headroom ? func->stack_size - headroom : func->stack_size;
        /// Clamp to actual stack extent
        uint32_t avail = static_cast<uint32_t>(stack_.size()) - frame_fp;
        if (num_slots > avail) num_slots = avail;
    }
    for (uint32_t slot = 0; slot < num_slots; ++slot) {
        std::size_t stack_idx = static_cast<std::size_t>(frame_fp) + slot;
        if (stack_idx >= stack_.size()) break;
        VarEntry e;
        /// Use the real name from local_names if available; fall back to %N placeholder.
        if (slot < func->local_names.size() && !func->local_names[slot].empty()) {
            e.name = func->local_names[slot];
        } else {
            e.name = "%" + std::to_string(slot);
        }
        e.value    = stack_[stack_idx];
        e.is_param = (slot < num_params);
        result.push_back(e);
    }
    return result;
}

std::vector<VarEntry> VM::get_upvalues(std::size_t frame_index) const {
    std::vector<VarEntry> result;

    LispVal closure_val = current_closure_;

    if (frame_index != 0) {
        std::size_t idx = 1;
        for (int i = static_cast<int>(frames_.size()) - 1; i >= 0; --i) {
            const auto& f = frames_[static_cast<std::size_t>(i)];
            if (f.kind == FrameKind::Sentinel) break;
            if (f.func == nullptr) continue;
            if (idx == frame_index) {
                closure_val = f.closure;
                break;
            }
            ++idx;
        }
    }

    if (auto* cl = try_get_as<ObjectKind::Closure, types::Closure>(closure_val)) {
        for (std::size_t i = 0; i < cl->upvals.size(); ++i) {
            VarEntry e;
            /// Use the real name from upval_names if available; fall back to &N placeholder.
            if (cl->func && i < cl->func->upval_names.size() && !cl->func->upval_names[i].empty()) {
                e.name = cl->func->upval_names[i];
            } else {
                e.name = "&" + std::to_string(i);
            }
            e.value = cl->upvals[i];
            result.push_back(e);
        }
    }
    return result;
}

std::expected<LispVal, RuntimeError> VM::make_list_payload(const std::vector<LispVal>& elements) {
    LispVal out = Nil;
    for (auto it = elements.rbegin(); it != elements.rend(); ++it) {
        temp_roots_.push_back(out);
        temp_roots_.push_back(*it);
        auto cell = make_cons(heap_, *it, out);
        temp_roots_.pop_back();
        temp_roots_.pop_back();
        if (!cell) return std::unexpected(cell.error());
        out = *cell;
    }
    return out;
}

std::expected<LispVal, RuntimeError> VM::runtime_error_tag(const RuntimeError& err) {
    const char* tag_name = "runtime.internal-error";
    std::visit([&](auto&& e) {
        using T = std::decay_t<decltype(e)>;
        if constexpr (std::is_same_v<T, VMError>) {
            switch (e.code) {
                case RuntimeErrorCode::TypeError:       tag_name = "runtime.type-error"; break;
                case RuntimeErrorCode::InvalidArity:    tag_name = "runtime.invalid-arity"; break;
                case RuntimeErrorCode::UserError:       tag_name = "runtime.user-error"; break;
                case RuntimeErrorCode::UndefinedGlobal: tag_name = "runtime.undefined-global"; break;
                case RuntimeErrorCode::NotImplemented:
                case RuntimeErrorCode::InternalError:
                case RuntimeErrorCode::StackOverflow:
                case RuntimeErrorCode::StackUnderflow:
                case RuntimeErrorCode::FrameOverflow:
                case RuntimeErrorCode::InvalidInstruction:
                case RuntimeErrorCode::UserThrow:
                default:
                    tag_name = "runtime.internal-error";
                    break;
            }
        } else if constexpr (std::is_same_v<T, NaNBoxError>) {
            tag_name = "runtime.nanbox-error";
        } else if constexpr (std::is_same_v<T, HeapError>) {
            tag_name = "runtime.heap-error";
        } else if constexpr (std::is_same_v<T, InternTableError>) {
            tag_name = "runtime.intern-error";
        }
    }, err);
    return make_symbol(intern_table_, tag_name);
}

std::string VM::runtime_error_message(const RuntimeError& err) const {
    return std::visit([](auto&& e) -> std::string {
        using T = std::decay_t<decltype(e)>;
        if constexpr (std::is_same_v<T, VMError>) {
            return e.message;
        } else if constexpr (std::is_same_v<T, NaNBoxError>) {
            return std::string("nanbox error: ") + eta::runtime::nanbox::to_string(e);
        } else if constexpr (std::is_same_v<T, HeapError>) {
            return std::string("heap error: ") + eta::runtime::memory::heap::to_string(e);
        } else if constexpr (std::is_same_v<T, InternTableError>) {
            return std::string("intern error: ") + eta::runtime::memory::intern::to_string(e);
        }
        return std::string("runtime error");
    }, err);
}

std::vector<FrameInfo> VM::capture_runtime_stack_trace(reader::lexer::Span span) const {
    auto trace = get_frames();
    if (!trace.empty()) trace[0].span = span;
    return trace;
}

bool VM::runtime_catch_matches(LispVal catch_tag, LispVal specific_tag, LispVal super_tag) const {
    /// Runtime catch-all (tagless catch) and explicit runtime tags both match.
    if (catch_tag == Nil) return true;
    return values_eqv(catch_tag, specific_tag) || values_eqv(catch_tag, super_tag);
}

std::expected<LispVal, RuntimeError> VM::build_runtime_error_payload(
    const RuntimeError& err, LispVal tag, reader::lexer::Span span) {
    const auto roots_base = temp_roots_.size();
    auto restore_roots = [&] { temp_roots_.resize(roots_base); };

    auto runtime_error_sym = make_symbol(intern_table_, "runtime-error");
    if (!runtime_error_sym) return std::unexpected(runtime_error_sym.error());
    temp_roots_.push_back(*runtime_error_sym);

    auto span_sym = make_symbol(intern_table_, "span");
    if (!span_sym) {
        restore_roots();
        return std::unexpected(span_sym.error());
    }
    temp_roots_.push_back(*span_sym);

    auto frame_sym = make_symbol(intern_table_, "frame");
    if (!frame_sym) {
        restore_roots();
        return std::unexpected(frame_sym.error());
    }
    temp_roots_.push_back(*frame_sym);

    auto to_fixnum = [&](std::uint32_t v) -> std::expected<LispVal, RuntimeError> {
        auto enc = ops::encode<int64_t>(static_cast<int64_t>(v));
        if (enc) return *enc;
        return make_fixnum(heap_, static_cast<int64_t>(v));
    };

    auto build_span_record = [&](const reader::lexer::Span& s) -> std::expected<LispVal, RuntimeError> {
        auto file_id = to_fixnum(s.file_id);
        if (!file_id) return std::unexpected(file_id.error());
        auto start_line = to_fixnum(s.start.line);
        if (!start_line) return std::unexpected(start_line.error());
        auto start_col = to_fixnum(s.start.column);
        if (!start_col) return std::unexpected(start_col.error());
        auto end_line = to_fixnum(s.end.line);
        if (!end_line) return std::unexpected(end_line.error());
        auto end_col = to_fixnum(s.end.column);
        if (!end_col) return std::unexpected(end_col.error());

        std::vector<LispVal> fields{*span_sym, *file_id, *start_line, *start_col, *end_line, *end_col};
        return make_list_payload(fields);
    };

    auto msg_val = make_string(heap_, intern_table_, runtime_error_message(err));
    if (!msg_val) {
        restore_roots();
        return std::unexpected(msg_val.error());
    }
    temp_roots_.push_back(*msg_val);

    auto span_val = build_span_record(span);
    if (!span_val) {
        restore_roots();
        return std::unexpected(span_val.error());
    }
    temp_roots_.push_back(*span_val);

    std::vector<LispVal> frame_rows;
    auto trace = capture_runtime_stack_trace(span);
    frame_rows.reserve(trace.size());
    for (const auto& fr : trace) {
        auto fn = make_string(heap_, intern_table_, fr.func_name);
        if (!fn) {
            restore_roots();
            return std::unexpected(fn.error());
        }
        temp_roots_.push_back(*fn);

        auto frame_span = build_span_record(fr.span);
        if (!frame_span) {
            restore_roots();
            return std::unexpected(frame_span.error());
        }
        temp_roots_.push_back(*frame_span);

        auto frame_row = make_list_payload({*frame_sym, *fn, *frame_span});
        if (!frame_row) {
            restore_roots();
            return std::unexpected(frame_row.error());
        }
        frame_rows.push_back(*frame_row);
        temp_roots_.push_back(*frame_row);
    }

    auto stack_trace = make_list_payload(frame_rows);
    if (!stack_trace) {
        restore_roots();
        return std::unexpected(stack_trace.error());
    }
    temp_roots_.push_back(*stack_trace);

    auto payload = make_list_payload(
        {*runtime_error_sym, tag, *msg_val, *span_val, *stack_trace});
    if (!payload) {
        restore_roots();
        return std::unexpected(payload.error());
    }

    restore_roots();
    return *payload;
}

std::expected<void, RuntimeError> VM::do_runtime_error(const RuntimeError& err, reader::lexer::Span span) {
    auto specific_tag = runtime_error_tag(err);
    if (!specific_tag) return std::unexpected(err);

    auto super_tag = make_symbol(intern_table_, "runtime.error");
    if (!super_tag) return std::unexpected(err);

    for (int i = static_cast<int>(catch_stack_.size()) - 1; i >= 0; --i) {
        const CatchFrame& cf = catch_stack_[static_cast<std::size_t>(i)];
        if (!runtime_catch_matches(cf.tag, *specific_tag, *super_tag)) continue;

        auto payload = build_runtime_error_payload(err, *specific_tag, span);
        if (!payload) return std::unexpected(err);

        /// Pop all catch frames from this one upward.
        catch_stack_.resize(static_cast<std::size_t>(i));

        /// Restore VM frame state.
        frames_.resize(cf.frame_count);
        winding_stack_.resize(cf.wind_count);
        active_tapes_.resize(cf.tape_count);

        /// Restore stack to the saved top, then push the caught payload.
        stack_.resize(cf.stack_top);
        push(*payload);

        /// Restore execution context to the function containing SetupCatch.
        current_func_    = cf.func;
        fp_              = cf.fp;
        current_closure_ = cf.closure;
        pc_              = cf.handler_pc;

        return {};
    }

    if (debug_) debug_->notify_exception(runtime_error_message(err), span);
    return std::unexpected(err);
}

/**
 * Exception Throw
 */

std::expected<void, RuntimeError> VM::do_throw(LispVal tag, LispVal value,
                                                reader::lexer::Span span) {
    /// Search the catch stack from top (most recent) to bottom for a matching tag.
    for (int i = static_cast<int>(catch_stack_.size()) - 1; i >= 0; --i) {
        const CatchFrame& cf = catch_stack_[static_cast<std::size_t>(i)];
        /// Match if tags are equal (same symbol) OR the catch frame is a catch-all (Nil tag).
        bool matches = (cf.tag == Nil) || values_eqv(cf.tag, tag);
        if (matches) {
            /// Pop all catch frames from this one upward.
            catch_stack_.resize(static_cast<std::size_t>(i));

            /// Restore VM frame state.
            frames_.resize(cf.frame_count);
            winding_stack_.resize(cf.wind_count);
            active_tapes_.resize(cf.tape_count);

            /// Restore stack to the saved top, then push the caught value.
            stack_.resize(cf.stack_top);
            push(value);

            /// Restore execution context to the function containing SetupCatch.
            current_func_    = cf.func;
            fp_              = cf.fp;
            current_closure_ = cf.closure;
            pc_              = cf.handler_pc;

            return {};
        }
    }

    std::string tag_str = "unknown-tag";
    if (ops::is_boxed(tag) && ops::tag(tag) == Tag::Symbol) {
        auto name = intern_table_.get_string(ops::payload(tag));
        if (name) tag_str = std::string(*name);
    }
    std::string msg = "Unhandled raise: " + tag_str;

    if (debug_) debug_->notify_exception(msg, span);

    return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::UserThrow, msg}});
}

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
                memo[id] = *fresh;
                return *fresh;
            }
            /// but handle gracefully.
            return walk(*lv->binding);
        }

        if (auto* cons = heap_.try_get_as<ObjectKind::Cons, types::Cons>(id)) {
            auto car_copy = walk(cons->car);
            if (!car_copy) return car_copy;
            auto cdr_copy = walk(cons->cdr);
            if (!cdr_copy) return cdr_copy;
            return make_cons(heap_, *car_copy, *cdr_copy);
        }

        if (auto* vec = heap_.try_get_as<ObjectKind::Vector, types::Vector>(id)) {
            std::vector<LispVal> elems;
            elems.reserve(vec->elements.size());
            for (auto& elem : vec->elements) {
                auto elem_copy = walk(elem);
                if (!elem_copy) return elem_copy;
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
                args.push_back(*a_copy);
            }
            return make_compound(heap_, ct->functor, std::move(args));
        }

        return v;
    };

    return walk(term);
}

} ///< namespace eta::runtime::vm

