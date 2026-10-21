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

namespace {

constexpr std::string_view kNonLocalTransferSentinel = "__eta.non_local_transfer__";

[[nodiscard]] bool is_non_local_transfer_error(const RuntimeError& err) {
    if (const auto* vm_err = std::get_if<VMError>(&err)) {
        return vm_err->code == RuntimeErrorCode::InternalError &&
               vm_err->message == kNonLocalTransferSentinel;
    }
    return false;
}

[[nodiscard]] RuntimeError make_non_local_transfer_error() {
    return RuntimeError{VMError{RuntimeErrorCode::InternalError,
                                std::string(kNonLocalTransferSentinel)}};
}

} ///< namespace

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

VM::~VM() {
    alive_->store(false, std::memory_order_release);
}

VM::ExecutionScope::ExecutionScope(VM& vm)
    : vm_(vm) {
    if (vm_.is_executing()) {
        vm_.save_execution_state();
        saved_ = true;
    }
}

VM::ExecutionScope::~ExecutionScope() {
    if (saved_) {
        vm_.restore_execution_state();
    }
}

void VM::save_execution_state() {
    ExecutionSnapshot snapshot;
    snapshot.stack = std::move(stack_);
    snapshot.frames = std::move(frames_);
    snapshot.winding_stack = std::move(winding_stack_);
    snapshot.temp_roots = std::move(temp_roots_);
    snapshot.catch_stack = std::move(catch_stack_);
    snapshot.pending_exception_transfer = std::move(pending_exception_transfer_);
    snapshot.pending_unwind_thunks = std::move(pending_unwind_thunks_);
    snapshot.pending_unwind_index = pending_unwind_index_;
    snapshot.current_func = current_func_;
    snapshot.pc = pc_;
    snapshot.fp = fp_;
    snapshot.current_closure = current_closure_;
    snapshot.execute_slice_options = execute_slice_options_;
    snapshot.active_slice_entry = active_slice_entry_;
    snapshot.slice_active = slice_active_;
    snapshot.slice_reduction_budget_remaining = slice_reduction_budget_remaining_;
    snapshot.pending_run_loop_status = pending_run_loop_status_;
    snapshot.pending_blocking_primitive = pending_blocking_primitive_;
    saved_executions_.push_back(std::move(snapshot));

    clear_exception_transfer();
    stack_.clear();
    frames_.clear();
    winding_stack_.clear();
    temp_roots_.clear();
    catch_stack_.clear();
    pending_unwind_thunks_.clear();
    pending_unwind_index_ = 0;
    current_func_ = nullptr;
    pc_ = 0;
    fp_ = 0;
    current_closure_ = nanbox::Nil;
    execute_slice_options_ = ExecuteSliceOptions{};
    active_slice_entry_ = nullptr;
    slice_active_ = false;
    slice_reduction_budget_remaining_ = 0;
    pending_run_loop_status_.reset();
    pending_blocking_primitive_.reset();
}

void VM::restore_execution_state() {
    if (saved_executions_.empty()) return;

    ExecutionSnapshot snapshot = std::move(saved_executions_.back());
    saved_executions_.pop_back();

    stack_ = std::move(snapshot.stack);
    frames_ = std::move(snapshot.frames);
    winding_stack_ = std::move(snapshot.winding_stack);
    temp_roots_ = std::move(snapshot.temp_roots);
    catch_stack_ = std::move(snapshot.catch_stack);
    pending_exception_transfer_ = std::move(snapshot.pending_exception_transfer);
    pending_unwind_thunks_ = std::move(snapshot.pending_unwind_thunks);
    pending_unwind_index_ = snapshot.pending_unwind_index;
    current_func_ = snapshot.current_func;
    pc_ = snapshot.pc;
    fp_ = snapshot.fp;
    current_closure_ = snapshot.current_closure;
    execute_slice_options_ = snapshot.execute_slice_options;
    active_slice_entry_ = snapshot.active_slice_entry;
    slice_active_ = snapshot.slice_active;
    slice_reduction_budget_remaining_ = snapshot.slice_reduction_budget_remaining;
    pending_run_loop_status_ = snapshot.pending_run_loop_status;
    pending_blocking_primitive_ = snapshot.pending_blocking_primitive;
}

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

std::expected<LispVal, RuntimeError> VM::execute(const BytecodeFunction& main) {
    auto run_result = execute_with_status(main, ExecuteSliceOptions{});
    if (!run_result) return std::unexpected(run_result.error());
    if (run_result->status != ExecuteSliceStatus::Finished) {
        return std::unexpected(RuntimeError{VMError{
            RuntimeErrorCode::InternalError,
            "execute: unexpected non-finished VM status"}});
    }
    return run_result->value;
}

std::expected<VM::ExecuteSliceResult, RuntimeError> VM::execute_with_status(
    const BytecodeFunction& main,
    ExecuteSliceOptions options) {
    ExecuteDepthScope depth_scope(*this);

    if (!slice_active_) {
        clear_exception_transfer();
        pending_run_loop_status_.reset();
        pending_blocking_primitive_.reset();
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
        active_slice_entry_ = &main;
        slice_active_ = true;
    } else if (active_slice_entry_ != &main) {
        return std::unexpected(RuntimeError{VMError{
            RuntimeErrorCode::InternalError,
            "execute_with_status: cannot switch entry function while execution is active"}});
    }

    execute_slice_options_ = options;
    if (yielding_enabled()) {
        slice_reduction_budget_remaining_ = execute_slice_options_.reduction_budget;
    } else {
        slice_reduction_budget_remaining_ = 0;
    }

    auto run_result = run_loop();
    if (!run_result) {
        slice_active_ = false;
        active_slice_entry_ = nullptr;
        pending_run_loop_status_.reset();
        pending_blocking_primitive_.reset();
        slice_reduction_budget_remaining_ = 0;
        return std::unexpected(run_result.error());
    }

    ExecuteSliceResult out;
    switch (*run_result) {
        case ExecuteSliceStatus::BudgetExhausted:
            out.status = ExecuteSliceStatus::BudgetExhausted;
            out.value = nanbox::Nil;
            return out;
        case ExecuteSliceStatus::BlockedOnReceive:
            out.status = ExecuteSliceStatus::BlockedOnReceive;
            out.value = nanbox::Nil;
            return out;
        case ExecuteSliceStatus::Finished:
            break;
    }

    process_pending_finalizers();
    out.status = ExecuteSliceStatus::Finished;
    out.value = pop();

    slice_active_ = false;
    active_slice_entry_ = nullptr;
    pending_run_loop_status_.reset();
    pending_blocking_primitive_.reset();
    slice_reduction_budget_remaining_ = 0;
    return out;
}

std::expected<LispVal, RuntimeError> VM::call_value(LispVal proc, std::vector<LispVal> args) {
    const bool host_call = (current_func_ == nullptr);
    const auto process_finalizers_if_host = [&]() {
        if (host_call) process_pending_finalizers();
    };
    charge_reductions();

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

    ExecuteDepthScope depth_scope(*this);

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

    /**
     * The caller's closure is saved only in this C++ local: current_closure_
     * is cleared below, so the return frame records Nil.  Keep it rooted for
     * the nested run so it is still live when restored.
     */
    temp_roots_.push_back(saved_closure);

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

    if (*run_res != ExecuteSliceStatus::Finished) {
        restore();
        process_finalizers_if_host();
        return std::unexpected(RuntimeError{VMError{
            RuntimeErrorCode::InternalError,
            "call_value: yielded during nested VM execution"}});
    }

    /**
     * A non-local transfer (raise/catch or runtime catch) may unwind past
     * this synthetic call boundary. In that case, control has already moved
     * to a handler frame and this helper must not synthesize a return value.
     */
    if (!host_call &&
        (current_func_ != nullptr || frames_.size() != saved_frames)) {
        temp_roots_.resize(saved_temp_roots);
        process_finalizers_if_host();
        return std::unexpected(make_non_local_transfer_error());
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

void VM::charge_reductions(std::uint64_t units) noexcept {
    if (units == 0) return;

    constexpr std::uint64_t max_reductions = std::numeric_limits<std::uint64_t>::max();
    if (reductions_ > (max_reductions - units)) {
        reductions_ = max_reductions;
    } else {
        reductions_ += units;
    }

    if (actor_system_) {
        actor_system_->add_current_thread_reductions(units);
    }

    if (!yielding_enabled()) return;

    if (slice_reduction_budget_remaining_ <= units) {
        slice_reduction_budget_remaining_ = 0;
        if (!pending_run_loop_status_.has_value()) {
            pending_run_loop_status_ = ExecuteSliceStatus::BudgetExhausted;
        }
    } else {
        slice_reduction_budget_remaining_ -= units;
    }
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
    charge_reductions();

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
            prof::runtime_profiler().on_continuation_jump(cont->frames.size());

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

        if (stack_.size() < argc) [[unlikely]] {
            return std::unexpected(RuntimeError{
                VMError{RuntimeErrorCode::InvalidInstruction,
                        "Primitive call stack underflow"}});
        }

        const std::size_t args_start = stack_.size() - argc;
        const std::span<const LispVal> args_span{stack_.data() + args_start,
                                                 static_cast<std::size_t>(argc)};

        const char* prim_name = prim->debug_name.empty() ? nullptr : prim->debug_name.c_str();
        const bool blocking_builtin = prim_name != nullptr
            && builtin_is_blocking(prim->debug_name);
        const bool allow_dirty_dispatch = !is_tail
            && execute_depth_ == 1
            && !pending_blocking_primitive_.has_value()
            && actor_system_
            && actor_system_->scheduler_mode() == actor::ActorSystem::SchedulerMode::Pool
            && blocking_builtin;

        if (allow_dirty_dispatch) {
            auto actor_pid = actor_system_->current_pid();
            if (actor_pid.has_value()) {
                auto task_promise =
                    std::make_shared<std::promise<std::expected<LispVal, RuntimeError>>>();
                auto task_future = task_promise->get_future().share();
                auto captured_args = std::vector<LispVal>(args_span.begin(), args_span.end());
                auto prim_label = prim->debug_name;

                const bool queued = actor_system_->enqueue_dirty_task(
                    [task_promise,
                     actor_system = actor_system_,
                     actor_pid = *actor_pid,
                     prim_func = prim->func,
                     prim_label = std::move(prim_label),
                     captured_args = std::move(captured_args)]() mutable {
                        std::expected<LispVal, RuntimeError> result;
                        try {
                            result = prim_func(std::span<const LispVal>(
                                captured_args.data(),
                                captured_args.size()));
                        } catch (const std::exception& ex) {
                            result = std::unexpected(RuntimeError{VMError{
                                RuntimeErrorCode::InternalError,
                                "dirty primitive '" + prim_label + "' threw: " + ex.what()}});
                        } catch (...) {
                            result = std::unexpected(RuntimeError{VMError{
                                RuntimeErrorCode::InternalError,
                                "dirty primitive '" + prim_label + "' threw"}});
                        }

                        try {
                            task_promise->set_value(std::move(result));
                        } catch (...) {
                        }

                        if (actor_system) {
                            actor_system->notify_external_runnable(actor_pid);
                        }
                    });

                if (queued) {
                    pending_blocking_primitive_ = PendingBlockingPrimitive{
                        .args_start = args_start,
                        .call_span = call_span,
                        .result = std::move(task_future)};
                    pending_run_loop_status_ = ExecuteSliceStatus::BlockedOnReceive;
                    return DispatchResult{DispatchAction::Continue, nullptr, 0};
                }
            }
        }

        prof::ScopedPrimitiveCall primitive_scope(prim_name);
        auto res = prim->func(args_span);
        stack_.resize(args_start);
        if (!res) {
            if (is_non_local_transfer_error(res.error())) {
                return DispatchResult{DispatchAction::NonLocalTransfer, nullptr, 0};
            }
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

std::expected<void, RuntimeError> VM::complete_pending_blocking_primitive() {
    if (!pending_blocking_primitive_.has_value()) return {};

    auto& pending = *pending_blocking_primitive_;
    if (pending.result.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready) {
        pending_run_loop_status_ = ExecuteSliceStatus::BlockedOnReceive;
        return {};
    }

    std::expected<LispVal, RuntimeError> result;
    try {
        result = pending.result.get();
    } catch (const std::exception& ex) {
        result = std::unexpected(RuntimeError{VMError{
            RuntimeErrorCode::InternalError,
            std::string("dirty primitive completion failed: ") + ex.what()}});
    } catch (...) {
        result = std::unexpected(RuntimeError{VMError{
            RuntimeErrorCode::InternalError,
            "dirty primitive completion failed"}});
    }

    const auto args_start = pending.args_start;
    const auto call_span = pending.call_span;
    pending_blocking_primitive_.reset();

    if (args_start > stack_.size()) {
        return std::unexpected(RuntimeError{VMError{
            RuntimeErrorCode::InternalError,
            "dirty primitive restored invalid stack state"}});
    }
    stack_.resize(args_start);

    if (!result) {
        if (is_non_local_transfer_error(result.error())) {
            return {};
        }
        auto handled = do_runtime_error(result.error(), call_span);
        if (!handled) return std::unexpected(handled.error());
        return {};
    }

    push(*result);
    return {};
}

std::expected<VM::ExecuteSliceStatus, RuntimeError> VM::run_loop() {
    auto pending_primitive = complete_pending_blocking_primitive();
    if (!pending_primitive) return std::unexpected(pending_primitive.error());

    if (pending_run_loop_status_.has_value()) {
        const auto pending = *pending_run_loop_status_;
        pending_run_loop_status_.reset();
        return pending;
    }

    while (current_func_ && pc_ < current_func_->code.size()) {
        pending_primitive = complete_pending_blocking_primitive();
        if (!pending_primitive) return std::unexpected(pending_primitive.error());

        if (pending_run_loop_status_.has_value()) {
            const auto pending = *pending_run_loop_status_;
            pending_run_loop_status_.reset();
            return pending;
        }

        if (prof::runtime_profiler().sampling_active()) [[unlikely]] {
            prof::runtime_profiler().on_vm_safepoint();
        }

        if (heap_.pending_finalizer_count() != 0u) {
            process_pending_finalizers();
        }

        /**
         * Debug hook and cooperative interrupt check.
         * Both share the same fast-path branch to avoid extra hot-loop work.
         */
        if (debug_ || interrupt_flag_.load(std::memory_order_relaxed)) {
            if (interrupt_flag_.exchange(false, std::memory_order_acq_rel)) {
                return std::unexpected(RuntimeError{
                    VMError{RuntimeErrorCode::UserError, "Execution interrupted"}
                });
            }
            if (debug_) {
                const auto sp = current_func_->span_at(pc_);
                debug_->check_and_wait(sp, frames_.size());
            }
        }
        const auto& instr = current_func_->code[pc_++];
        charge_reductions();
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
                /// Popped values are no longer stack roots; keep them alive across the allocation.
                const auto temp_root_mark = temp_roots_.size();
                temp_roots_.insert(temp_roots_.end(), vals.begin(), vals.end());
                auto mv = make_multiple_values(heap_, std::move(vals));
                temp_roots_.resize(temp_root_mark);
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
                if (sandbox_mode_) [[unlikely]]
                    return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::SandboxViolation, "StoreUpval blocked: sandbox is read-only"}});
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
                if (sandbox_mode_) [[unlikely]]
                    return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::SandboxViolation, "StoreGlobal blocked: sandbox is read-only"}});
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
            case OpCode::Jump: {
                const auto rel = static_cast<int32_t>(instr.arg);
                const auto target_pc =
                    static_cast<int64_t>(pc_) + static_cast<int64_t>(rel);
                if (target_pc < 0 ||
                    target_pc > static_cast<int64_t>(std::numeric_limits<uint32_t>::max())) {
                    return std::unexpected(RuntimeError{
                        VMError{RuntimeErrorCode::InvalidInstruction,
                                "Jump: target out of range"}});
                }

                /**
                 * Self-tail recursion lowered to a backward Jump targets the
                 * lambda entry. Keep the active frame shape consistent with
                 * TailCall frame reuse by trimming transient stack growth.
                 */
                if (rel < 0 && target_pc == 0) {
                    const uint32_t needed_size =
                        std::max(current_func_->stack_size, current_func_->arity);
                    stack_.resize(fp_ + needed_size, Nil);
                }

                pc_ = static_cast<uint32_t>(target_pc);
                break;
            }
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
                
                auto closure_res = make_closure(heap_, bfunc, std::move(upvals), current_closure_);
                if (!closure_res) return std::unexpected(closure_res.error());
                push(*closure_res);
                break;
            }
            case OpCode::PatchClosureUpval: {
                /// Fixup for letrec: patch a closure's captured upval after set!
                LispVal value = pop();
                LispVal closure_val = pop();
                if (sandbox_mode_) [[unlikely]]
                    return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::SandboxViolation, "PatchClosureUpval blocked: sandbox is read-only"}});
                auto* closure = try_get_as<ObjectKind::Closure, Closure>(closure_val);
                if (closure && instr.arg < closure->upvals.size()) {
                    closure->upvals[instr.arg] = value;
                }
                break;
            }
            case OpCode::Cons: {
                LispVal cdr = pop();
                LispVal car = pop();
                /// Popped operands are no longer stack roots; keep them alive across the allocation.
                const auto temp_root_mark = temp_roots_.size();
                temp_roots_.push_back(car);
                temp_roots_.push_back(cdr);
                auto res = make_cons(heap_, car, cdr);
                temp_roots_.resize(temp_root_mark);
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
                    prof::runtime_profiler().on_tail_reuse(dispatch_res->func);
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
                    prof::runtime_profiler().on_tail_reuse(dispatch_res->func);
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
    pending_run_loop_status_.reset();
    return ExecuteSliceStatus::Finished;
}

/**
 * Numeric Helpers - Using ValueVisitor from value_visit.h
 */

/// Numeric type enumeration for dispatch
std::expected<void, RuntimeError> VM::handle_return(LispVal result) {
    const BytecodeFunction* caller_func = frames_.empty() ? nullptr : frames_.back().func;
    prof::runtime_profiler().on_return(current_func_, caller_func);

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
            prof::runtime_profiler().on_continuation_jump(cont->frames.size());

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

    if (return_frame.kind == FrameKind::ExceptionUnwind) {
        /// Returned from a dynamic-wind unwind thunk; result is discarded.
        return resume_exception_transfer();
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
        const LispVal active = active_tape();
        if (!ops::is_boxed(active) || ops::tag(active) != Tag::HeapObject) {
            return std::unexpected(ad::make_error(
                ad::kTagNoActiveTape,
                "tape arithmetic: no active tape",
                {ad::field("op", std::string("binary-arithmetic"))}));
        }
        auto* tape = heap_.try_get_as<ObjectKind::Tape, Tape>(ops::payload(active));
        if (!tape) {
            return std::unexpected(ad::make_error(
                ad::kTagNoActiveTape,
                "tape arithmetic: no active tape",
                {ad::field("op", std::string("binary-arithmetic"))}));
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

        auto op_name = [&]() -> std::string {
            switch (op) {
                case OpCode::Add: return "+";
                case OpCode::Sub: return "-";
                case OpCode::Mul: return "*";
                case OpCode::Div: return "/";
                default: return "unknown";
            }
        };

        auto resolve_idx = [&](LispVal v, bool is_tape, const char* side) -> std::expected<uint32_t, RuntimeError> {
            if (is_tape) {
                const auto ref = types::tape_ref::decode(v);
                if (ref.tape_id != tape->tape_id) {
                    return std::unexpected(ad::make_error(
                        ad::kTagMixedTape,
                        "tape arithmetic: reference belongs to a different tape",
                        {
                            ad::field("op", op_name()),
                            ad::field("side", std::string(side)),
                            ad::field("expected-tape-id", tape->tape_id),
                            ad::field("actual-tape-id", ref.tape_id),
                            ad::field("generation", ref.generation),
                            ad::field("node-index", ref.node_index)
                        }));
                }
                if (ref.generation != tape->generation) {
                    return std::unexpected(ad::make_error(
                        ad::kTagStaleRef,
                        "tape arithmetic: stale TapeRef generation",
                        {
                            ad::field("op", op_name()),
                            ad::field("side", std::string(side)),
                            ad::field("tape-id", tape->tape_id),
                            ad::field("expected-gen", tape->generation),
                            ad::field("actual-gen", ref.generation),
                            ad::field("node-index", ref.node_index)
                        }));
                }
                if (ref.node_index >= tape->entries.size()) {
                    return std::unexpected(ad::make_error(
                        ad::kTagStaleRef,
                        "tape arithmetic: TapeRef index out of range",
                        {
                            ad::field("op", op_name()),
                            ad::field("side", std::string(side)),
                            ad::field("tape-id", tape->tape_id),
                            ad::field("expected-gen", tape->generation),
                            ad::field("actual-gen", ref.generation),
                            ad::field("node-index", ref.node_index)
                        }));
                }
                return ref.node_index;
            }
            /// Promote plain number to tape constant
            auto nv = classify_numeric(v, heap_);
            if (!nv.is_valid()) return std::unexpected(make_type_error("tape arithmetic: operand is not a number"));
            auto idx = tape->push_const(nv.as_double());
            if (idx > types::tape_ref::MAX_NODE_INDEX) {
                return std::unexpected(RuntimeError{VMError{
                    RuntimeErrorCode::InternalError,
                    "tape arithmetic: tape node index exceeds TapeRef capacity"}});
            }
            return idx;
        };

        auto idx_a = resolve_idx(a, is_tape_a, "lhs");
        if (!idx_a) return std::unexpected(idx_a.error());
        auto idx_b = resolve_idx(b, is_tape_b, "rhs");
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
        if (new_idx > types::tape_ref::MAX_NODE_INDEX) {
            return std::unexpected(RuntimeError{VMError{
                RuntimeErrorCode::InternalError,
                "tape arithmetic: tape node index exceeds TapeRef capacity"}});
        }
        push(types::tape_ref::make(tape->tape_id, tape->generation, new_idx));
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
    /// The list built so far is held only in `rest_list` while the next cell allocates.
    const auto temp_root_mark = temp_roots_.size();
    LispVal rest_list = Nil;
    for (int32_t i = static_cast<int32_t>(argc) - 1; i >= static_cast<int32_t>(required); --i) {
        auto cons = make_cons(heap_, stack_[fp_ + i], rest_list);
        if (!cons) {
            temp_roots_.resize(temp_root_mark);
            return std::unexpected(cons.error());
        }
        rest_list = *cons;
        temp_roots_.push_back(rest_list);
    }
    temp_roots_.resize(temp_root_mark);
    stack_[fp_ + required] = rest_list;
    return {};
}
} ///< namespace eta::runtime::vm

