#include "vm.h"
#include <algorithm>
#include <iostream>
#include "eta/runtime/factory.h"
#include "eta/runtime/types/types.h"
#include "eta/runtime/types/logic_var.h"
#include "eta/runtime/types/primitive.h"
#include "eta/runtime/types/dual.h"
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
    heap_.set_gc_callback([this]() { collect_garbage(); });

    // Initialize default console ports
    auto stdin_port = std::make_shared<ConsolePort>(ConsolePort::StreamType::Input);
    auto stdout_port = std::make_shared<ConsolePort>(ConsolePort::StreamType::Output);
    auto stderr_port = std::make_shared<ConsolePort>(ConsolePort::StreamType::Error);

    auto input_result = make_port(heap_, stdin_port);
    auto output_result = make_port(heap_, stdout_port);
    auto error_result = make_port(heap_, stderr_port);

    // Store default ports; if allocation fails, ports remain as Nil (will be handled by primitives)
    if (input_result) current_input_ = *input_result;
    if (output_result) current_output_ = *output_result;
    if (error_result) current_error_ = *error_result;
}

VM::~VM() = default;

bool VM::values_eqv(LispVal a, LispVal b) {
    // Fast path: bit-identical values are always equal (handles Nil, True, False, small Fixnums, same heap IDs)
    if (a == b) return true;
    
    // If either is not boxed (raw double), they would have matched above if equal
    if (!ops::is_boxed(a) || !ops::is_boxed(b)) return false;
    
    // For eqv?, strings and large numbers need content comparison
    
    // 1. Strings (interned): identical interned IDs match above, so different IDs mean different strings
    if (ops::tag(a) == Tag::String) return false;

    // 2. Heap-allocated values: use numeric classifier to handle heap-allocated fixnums
    if (ops::tag(a) == Tag::HeapObject && ops::tag(b) == Tag::HeapObject) {
        auto na = classify_numeric(a, heap_);
        auto nb = classify_numeric(b, heap_);
        if (na.is_fixnum() && nb.is_fixnum()) {
            return na.int_val == nb.int_val;
        }
    }
    
    // Different types or non-matching non-string/non-number values
    return false;
}

void VM::collect_garbage() {
    if (!gc_) return;

    gc_->collect(heap_, [&](auto&& visit) {
        // Mark stack
        for (auto v : stack_) visit(v);
        // Mark globals
        for (auto v : globals_) visit(v);
        // Mark current execution state
        visit(current_closure_);
        // Mark frames
        for (const auto& f : frames_) {
            visit(f.closure);
            visit(f.extra);
        }
        // Mark winding stack
        for (const auto& w : winding_stack_) {
            visit(w.before);
            visit(w.body);
            visit(w.after);
        }
        // Mark temporary roots
        for (auto v : temp_roots_) visit(v);
        // Mark current ports
        visit(current_input_);
        visit(current_output_);
        visit(current_error_);
        // Mark catch frame tags and closures
        for (const auto& cf : catch_stack_) {
            visit(cf.tag);
            visit(cf.closure);
        }
        // Mark logic-variable trail (prevents live unbound vars from being swept
        // during an active unification / backtracking context)
        for (auto v : trail_stack_) visit(v);
    });
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

    roots.push_back({"Trail Stack", collect(trail_stack_.begin(), trail_stack_.end())});

    return roots;
}

std::expected<LispVal, RuntimeError> VM::execute(const BytecodeFunction& main) {
    // Push a sentinel frame to mark the bottom of this execution call.
    // This prevents CallCC from capturing frames above the point where execute() was called.
    frames_.push_back({nullptr, 0, 0, Nil, FrameKind::Sentinel});

    current_func_ = &main;
    pc_ = 0;
    fp_ = 0;
    current_closure_ = 0; // Top-level
    
    // Initial stack resize for main
    stack_.resize(main.stack_size, Nil);
    
    auto res = run_loop();
    if (!res) return std::unexpected(res.error());
    
    return pop();
}

std::expected<LispVal, RuntimeError> VM::call_value(LispVal proc, std::vector<LispVal> args) {
    // ── Fast path: primitive procedures — direct call, no VM re-entry ──────────
    if (auto* prim = try_get_as<ObjectKind::Primitive, Primitive>(proc)) {
        uint32_t argc = static_cast<uint32_t>(args.size());
        if (prim->has_rest) {
            if (argc < prim->arity)
                return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::InvalidArity,
                    "call_value: expected at least " + std::to_string(prim->arity) + " argument(s), got " + std::to_string(argc)}});
        } else {
            if (argc != prim->arity)
                return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::InvalidArity,
                    "call_value: expected " + std::to_string(prim->arity) + " argument(s), got " + std::to_string(argc)}});
        }
        return prim->func(args);
    }

    // ── Closure path — save outer state, run nested loop, restore ──────────────
    auto* cl = try_get_as<ObjectKind::Closure, Closure>(proc);
    if (!cl) {
        return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError,
            "call_value: argument is not a procedure"}});
    }

    uint32_t argc = static_cast<uint32_t>(args.size());
    if (cl->func->has_rest) {
        if (argc < cl->func->arity)
            return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::InvalidArity,
                "call_value: expected at least " + std::to_string(cl->func->arity) + " argument(s), got " + std::to_string(argc)}});
    } else {
        if (argc != cl->func->arity)
            return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::InvalidArity,
                "call_value: expected " + std::to_string(cl->func->arity) + " argument(s), got " + std::to_string(argc)}});
    }

    // Save full outer execution context
    const BytecodeFunction* saved_func    = current_func_;
    const uint32_t          saved_pc      = pc_;
    const uint32_t          saved_fp      = fp_;
    const LispVal           saved_closure = current_closure_;
    const auto              saved_frames  = frames_.size();
    const uint32_t          nested_sp     = static_cast<uint32_t>(stack_.size());

    // Helper: restore outer state and truncate any accumulated frames
    auto restore = [&]() {
        stack_.resize(nested_sp);
        frames_.resize(saved_frames);
        current_func_    = saved_func;
        pc_              = saved_pc;
        fp_              = saved_fp;
        current_closure_ = saved_closure;
    };

    // Temporarily null out execution state so that setup_frame saves a
    // "return-to-null" frame.  When the closure eventually returns to this
    // frame, handle_return sets current_func_ = nullptr and run_loop() exits.
    current_func_    = nullptr;
    pc_              = 0;
    fp_              = nested_sp;
    current_closure_ = Nil;

    // Push arguments then let setup_frame initialise the closure's frame.
    for (const auto& a : args) push(a);

    // setup_frame saves {nullptr, 0, nested_sp, Nil, Normal} as the return
    // frame and sets current_func_ = cl->func, fp_ = nested_sp, pc_ = 0.
    setup_frame(cl->func, proc, argc);

    // Pack variadic rest args into a list if the closure uses &rest.
    if (cl->func->has_rest) {
        auto pr = pack_rest_args(argc, cl->func->arity);
        if (!pr) {
            restore();
            return std::unexpected(pr.error());
        }
    }

    // Run until current_func_ becomes null (our return-to-null frame popped).
    auto run_res = run_loop();

    if (!run_res) {
        restore();
        return std::unexpected(run_res.error());
    }

    // On success: stack has nested_sp items + 1 result on top.
    LispVal result = pop();
    stack_.resize(nested_sp); // safety: ensure no stray stack entries

    // Restore the outer execution context.
    current_func_    = saved_func;
    pc_              = saved_pc;
    fp_              = saved_fp;
    current_closure_ = saved_closure;

    return result;
}

std::expected<LispVal, RuntimeError> VM::dual_binary_op(OpCode op, LispVal a, LispVal b) {
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


// Unified helper to dispatch a callee (Closure, Continuation, or Primitive)
std::expected<DispatchResult, RuntimeError> VM::dispatch_callee(LispVal callee, uint32_t argc, bool is_tail) {
    // Use try_get_as for consistent heap access pattern

    if (!ops::is_boxed(callee)) {
        return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError, "Callee is not boxed"}});
    }
    
    if (auto* closure = try_get_as<ObjectKind::Closure, Closure>(callee)) {
        // Arity check early (fail fast)
        if (closure->func->has_rest) {
            if (argc < closure->func->arity) {
                return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::InvalidArity,
                    "Wrong number of arguments: expected at least " + std::to_string(closure->func->arity) + ", got " + std::to_string(argc)}});
            }
        } else {
            if (argc != closure->func->arity) {
                return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::InvalidArity,
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
            return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::InvalidArity, "Continuation expects 1 argument"}});
        }

        LispVal v = pop(); // The argument
        temp_roots_.push_back(v);
        temp_roots_.push_back(callee);

        // Calculate the common ancestor of the current and target winding stacks.
        size_t common = 0;
        size_t min_len = std::min(winding_stack_.size(), cont->winding_stack.size());
        while (common < min_len &&
               winding_stack_[common].before == cont->winding_stack[common].before &&
               winding_stack_[common].after == cont->winding_stack[common].after) {
            common++;
        }

        if (common == winding_stack_.size() && common == cont->winding_stack.size()) {
            // Find current sentinel to define the boundary
            int32_t sentinel_idx = -1;
            for (int32_t i = static_cast<int32_t>(frames_.size()) - 1; i >= 0; --i) {
                if (frames_[i].kind == FrameKind::Sentinel) {
                    sentinel_idx = i;
                    break;
                }
            }

            // Restore state immediately if no winding/unwinding is needed.
            stack_ = cont->stack;
            
            // Reconstruct frames: current sentinel and below, then the captured frames.
            if (sentinel_idx != -1) {
                frames_.resize(sentinel_idx + 1);
                frames_.insert(frames_.end(), cont->frames.begin(), cont->frames.end());
            } else {
                frames_ = cont->frames;
            }
            
            winding_stack_ = cont->winding_stack;

            // Invalidate any in-flight step command – the stack has changed.
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
            // Prepare the list of thunks to be executed.
            std::vector<LispVal> thunks;
            for (int32_t i = static_cast<int32_t>(winding_stack_.size()) - 1; i >= static_cast<int32_t>(common); --i) {
                thunks.push_back(winding_stack_[i].after);
            }
            for (size_t i = common; i < cont->winding_stack.size(); ++i) {
                thunks.push_back(cont->winding_stack[i].before);
            }

            // Store the jump state in a Vector to be GC-rootable.
            // Format: [target_continuation, value_to_pass, next_thunk_index, ...thunks]
            std::vector<LispVal> state_els;
            state_els.push_back(callee);
            state_els.push_back(v);
            state_els.push_back(ops::encode<int64_t>(1).value()); // Index of next thunk (after the first one)
            for (auto t : thunks) state_els.push_back(t);

            auto state_vec = make_vector(heap_, std::move(state_els));
            if (!state_vec) return std::unexpected(state_vec.error());

            // Start the sequence by calling the first thunk.
            // The thunk's return frame must be ContinuationJump so that
            // handle_return processes the continuation jump chain when the thunk returns.
            LispVal first_thunk = thunks[0];
            auto dispatch = dispatch_callee(first_thunk, 0, false);
            if (!dispatch) return std::unexpected(dispatch.error());
            if (dispatch->action == DispatchAction::SetupFrame) {
                setup_frame(dispatch->func, dispatch->closure, 0, FrameKind::ContinuationJump, *state_vec);
            } else {
                // Primitive thunk: push ContinuationJump frame for handle_return to process.
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
        // Arity check for primitives
        if (prim->has_rest) {
            if (argc < prim->arity) {
                return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::InvalidArity,
                    "Primitive expects at least " + std::to_string(prim->arity) + " argument(s), got " + std::to_string(argc)}});
            }
        } else {
            if (argc != prim->arity) {
                return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::InvalidArity,
                    "Primitive expects " + std::to_string(prim->arity) + " argument(s), got " + std::to_string(argc)}});
            }
        }

        std::vector<LispVal> args;
        for (uint32_t i = 0; i < argc; ++i) {
            args.push_back(stack_[stack_.size() - argc + i]);
        }
        stack_.resize(stack_.size() - argc);

        auto res = prim->func(args);
        if (!res) return std::unexpected(res.error());
        push(*res);
        return DispatchResult{DispatchAction::Continue, nullptr, 0};
    }

    return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError, "Callee is not a function"}});
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
        // ── Debug hook ─────────────────────────────────────────────────────
        // Zero-cost when debug_ is null (non-debug runs).
        if (debug_) {
            const auto sp = current_func_->span_at(pc_);
            debug_->check_and_wait(sp, frames_.size());
        }
        // ──────────────────────────────────────────────────────────────────
        const auto& instr = current_func_->code[pc_++];
        switch (instr.opcode) {
            case OpCode::Nop:
                break;
            case OpCode::StoreLocal:
                stack_[fp_ + instr.arg] = pop();
                break;
            case OpCode::Values: {
                // [n] -> pack top n values into a MultipleValues object
                uint32_t n = instr.arg;
                if (n == 1) {
                    // Single value: leave it as-is (no wrapping needed)
                    break;
                }
                std::vector<LispVal> vals;
                vals.reserve(n);
                // Pop values in reverse order to maintain correct ordering
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
                // Stack: [producer, consumer] (consumer on top)
                LispVal consumer = pop();
                LispVal producer = pop();

                // Call producer with 0 arguments. Return to consumer afterwards.
                auto prod_dispatch = dispatch_callee(producer, 0, /*is_tail=*/false);
                if (!prod_dispatch) return std::unexpected(prod_dispatch.error());

                if (prod_dispatch->action == DispatchAction::SetupFrame) {
                    setup_frame(prod_dispatch->func, prod_dispatch->closure, 0, FrameKind::CallWithValuesConsumer, consumer);
                } else {
                    // Producer was a primitive - result already pushed.
                    // We can call consumer directly now.
                    uint32_t old_size = static_cast<uint32_t>(stack_.size());
                    unpack_to_stack(pop());
                    uint32_t argc = static_cast<uint32_t>(stack_.size() - old_size);
                    auto cons_dispatch = dispatch_callee(consumer, argc, false);
                    if (!cons_dispatch) return std::unexpected(cons_dispatch.error());
                    if (cons_dispatch->action == DispatchAction::SetupFrame) {
                        setup_frame(cons_dispatch->func, cons_dispatch->closure, argc);
                    }
                }
                break;
            }
            case OpCode::DynamicWind: {
                // Stack: [before, body, after] (after on top)
                LispVal after_thunk = pop();
                LispVal body_thunk = pop();
                LispVal before_thunk = pop();

                winding_stack_.push_back({before_thunk, body_thunk, after_thunk});

                // Call before() with 0 arguments. Return to body() afterwards.
                temp_roots_.push_back(before_thunk);
                auto before_dispatch = dispatch_callee(before_thunk, 0, /*is_tail=*/false);
                temp_roots_.pop_back();
                if (!before_dispatch) return std::unexpected(before_dispatch.error());

                if (before_dispatch->action == DispatchAction::SetupFrame) {
                    setup_frame(before_dispatch->func, before_dispatch->closure, 0, FrameKind::DynamicWindBody);
                } else {
                    // before was primitive. Call body() now.
                    auto body_dispatch = dispatch_callee(body_thunk, 0, false);
                    if (!body_dispatch) return std::unexpected(body_dispatch.error());
                    if (body_dispatch->action == DispatchAction::SetupFrame) {
                        setup_frame(body_dispatch->func, body_dispatch->closure, 0, FrameKind::DynamicWindAfter, after_thunk);
                    } else {
                        // body was primitive. Call after() now.
                        LispVal body_res = pop();
                        auto after_dispatch = dispatch_callee(after_thunk, 0, false);
                        if (!after_dispatch) return std::unexpected(after_dispatch.error());
                        if (after_dispatch->action == DispatchAction::SetupFrame) {
                            setup_frame(after_dispatch->func, after_dispatch->closure, 0, FrameKind::DynamicWindCleanup, body_res);
                        } else {
                            pop(); // ignore after() result
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
                push(stack_[fp_ + instr.arg]);
                break;
            case OpCode::LoadUpval: {
                auto closure = get_as_or_error<ObjectKind::Closure, Closure>(current_closure_, "LoadUpval outside of a closure");
                if (!closure) return std::unexpected(closure.error());
                push((*closure)->upvals[instr.arg]);
                break;
            }
            case OpCode::StoreUpval: {
                auto closure = get_as_or_error<ObjectKind::Closure, Closure>(current_closure_, "StoreUpval outside of a closure");
                if (!closure) return std::unexpected(closure.error());
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
                auto dispatch_res = dispatch_callee(callee, argc, /*is_tail=*/false);
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
                // arg is (const_idx << 16) | num_upvals
                uint32_t const_idx = instr.arg >> 16;
                uint32_t num_upvals = instr.arg & 0xFFFF;
                
                // The constant encodes a function index (high bit set) or legacy raw pointer
                LispVal func_val = current_func_->constants[const_idx];

                const BytecodeFunction* bfunc = nullptr;
                if (is_func_index(func_val)) {
                    // New-style: function index
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
                // Fixup for letrec: patch a closure's captured upval after set!
                // Stack: ... closure value → ...
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
                auto dispatch_res = dispatch_callee(callee, argc, /*is_tail=*/true);
                temp_roots_.pop_back();
                if (!dispatch_res) return std::unexpected(dispatch_res.error());

                if (dispatch_res->action == DispatchAction::TailReuse) {
                    current_func_ = dispatch_res->func;
                    current_closure_ = dispatch_res->closure;

                    // Move new arguments to current fp_
                    uint32_t new_args_start = static_cast<uint32_t>(stack_.size() - argc);
                    for (uint32_t i = 0; i < argc; ++i) {
                        stack_[fp_ + i] = stack_[new_args_start + i];
                    }
                    // Ensure we don't chop off moved arguments if stack_size is small
                    uint32_t needed_size = std::max(current_func_->stack_size, argc);
                    stack_.resize(fp_ + needed_size, Nil);
                    pc_ = 0;

                    if (current_func_->has_rest) {
                        auto r = pack_rest_args(argc, current_func_->arity);
                        if (!r) return std::unexpected(r.error());
                    }
                } else {
                    // TailCall to primitive or continuation - result already pushed.
                    // Now we MUST return that result from the current frame.
                    auto res = handle_return(pop());
                    if (!res) return std::unexpected(res.error());
                }
                break;
            }
            case OpCode::CallCC: {
                LispVal consumer = pop();
                temp_roots_.push_back(consumer);
                
                // Capture current execution state as top frame
                auto cont_frames = frames_;
                cont_frames.push_back({current_func_, pc_, fp_, current_closure_});
                
                auto cont_res = make_continuation(heap_, stack_, cont_frames, winding_stack_);
                if (!cont_res) return std::unexpected(cont_res.error());
                
                push(*cont_res);
                
                auto dispatch_res = dispatch_callee(consumer, 1, /*is_tail=*/false);
                temp_roots_.pop_back();
                if (!dispatch_res) return std::unexpected(dispatch_res.error());

                if (dispatch_res->action == DispatchAction::SetupFrame) {
                    setup_frame(dispatch_res->func, dispatch_res->closure, 1);
                }
                break;
            }
            case OpCode::Apply:
            case OpCode::TailApply: {
                // Stack: [arg1, ..., argN, proc] where argN (last explicit arg) is a list to unpack
                uint32_t argc = instr.arg; // number of explicit args on stack (including the list)
                LispVal proc = pop();
                temp_roots_.push_back(proc);

                // Pop explicit args
                std::vector<LispVal> explicit_args;
                explicit_args.reserve(argc);
                for (uint32_t i = 0; i < argc; ++i) {
                    explicit_args.push_back(stack_[stack_.size() - argc + i]);
                }
                stack_.resize(stack_.size() - argc);

                // Unpack the last arg (must be a proper list)
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

                // Push combined args back onto stack
                for (auto v : explicit_args) push(v);
                uint32_t final_argc = static_cast<uint32_t>(explicit_args.size());

                bool is_tail = (instr.opcode == OpCode::TailApply);
                auto dispatch_res = dispatch_callee(proc, final_argc, is_tail);
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
                } else {
                    // Primitive or continuation — result already pushed
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
                // arg = (tag_const_idx << 16) | pc_offset_to_handler
                uint32_t const_idx  = instr.arg >> 16;
                uint32_t pc_offset  = instr.arg & 0xFFFF;
                LispVal  tag        = (const_idx < current_func_->constants.size())
                                      ? current_func_->constants[const_idx]
                                      : Nil;
                // pc_ already past SetupCatch; handler_pc = pc_ + pc_offset
                CatchFrame cf;
                cf.tag         = tag;
                cf.func        = current_func_;
                cf.handler_pc  = pc_ + pc_offset;
                cf.fp          = fp_;
                cf.closure     = current_closure_;
                cf.frame_count = frames_.size();
                cf.stack_top   = static_cast<uint32_t>(stack_.size());
                cf.wind_count  = winding_stack_.size();
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

            // ── Unification opcodes ───────────────────────────────────────
            case OpCode::MakeLogicVar: {
                auto lv = make_logic_var(heap_);
                if (!lv) return std::unexpected(lv.error());
                push(*lv);
                break;
            }
            case OpCode::Unify: {
                LispVal b = pop();
                LispVal a = pop();
                push(unify(a, b) ? True : False);
                break;
            }
            case OpCode::DerefLogicVar: {
                push(deref(pop()));
                break;
            }
            case OpCode::TrailMark: {
                // Pack both the binding-trail size (bits 0-22) and the
                // constraint-trail size (bits 23-45) into one 47-bit fixnum.
                // Max trail depth per side: 2^23 - 1 = 8,388,607 — far beyond
                // any practical limit.  The combined value always fits within
                // FIXNUM_MAX (2^46 - 1).
                constexpr int64_t TRAIL_BITS = 23;
                constexpr int64_t TRAIL_MASK = (1LL << TRAIL_BITS) - 1; // 0x7FFFFF
                auto bsize = static_cast<int64_t>(trail_stack_.size());
                auto csize = static_cast<int64_t>(constraint_store_.trail_size());
                int64_t packed = (bsize & TRAIL_MASK) | ((csize & TRAIL_MASK) << TRAIL_BITS);
                auto enc = ops::encode<int64_t>(packed);
                if (!enc) return std::unexpected(make_type_error("trail-mark: trail too deep"));
                push(*enc);
                break;
            }
            case OpCode::UnwindTrail: {
                constexpr int64_t TRAIL_BITS = 23;
                constexpr int64_t TRAIL_MASK = (1LL << TRAIL_BITS) - 1;
                LispVal mark_val = pop();
                if (!ops::is_boxed(mark_val) || ops::tag(mark_val) != Tag::Fixnum)
                    return std::unexpected(make_type_error("unwind-trail: mark must be a fixnum"));
                auto mark_opt = ops::decode<int64_t>(mark_val);
                if (!mark_opt)
                    return std::unexpected(make_type_error("unwind-trail: invalid mark"));
                int64_t packed = *mark_opt;
                auto bmark = static_cast<std::size_t>(packed & TRAIL_MASK);
                auto cmark = static_cast<std::size_t>((packed >> TRAIL_BITS) & TRAIL_MASK);
                // Unwind binding trail
                while (trail_stack_.size() > bmark) {
                    LispVal lvar_val = trail_stack_.back();
                    trail_stack_.pop_back();
                    if (auto* lv = try_get_as<ObjectKind::LogicVar, types::LogicVar>(lvar_val))
                        lv->binding = std::nullopt;
                }
                // Unwind constraint trail (undo domain changes made since mark)
                constraint_store_.unwind(cmark);
                push(Nil);
                break;
            }

            // ── AD Dual number opcodes ──────────────────────────────────
            case OpCode::MakeDual: {
                LispVal backprop = pop();
                LispVal primal = pop();
                auto res = make_dual(heap_, primal, backprop);
                if (!res) return std::unexpected(res.error());
                push(*res);
                break;
            }
            case OpCode::DualVal: {
                LispVal v = pop();
                auto* d = try_get_as<ObjectKind::Dual, types::Dual>(v);
                push(d ? d->primal : v);
                break;
            }
            case OpCode::DualBp: {
                LispVal v = pop();
                auto* d = try_get_as<ObjectKind::Dual, types::Dual>(v);
                if (d) {
                    push(d->backprop);
                } else {
                    // Not a dual — return a no-op backpropagator (lambda (adj) '())
                    auto noop = make_primitive(heap_,
                        [](const std::vector<LispVal>&) -> std::expected<LispVal, RuntimeError> {
                            return Nil;
                        }, 1, false);
                    if (!noop) return std::unexpected(noop.error());
                    push(*noop);
                }
                break;
            }

            default:
                return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::NotImplemented, "OpCode not implemented"}});
        }
    }
    return {};
}

// ============================================================================
// Numeric Helpers - Using ValueVisitor from value_visit.h
// ============================================================================

// Numeric type enumeration for dispatch
std::expected<void, RuntimeError> VM::handle_return(LispVal result) {
    if (frames_.empty()) {
        stack_.resize(fp_);
        push(result);
        current_func_ = nullptr;
        return {};
    }
    
    // The frame we're returning to
    Frame return_frame = frames_.back();
    frames_.pop_back();

    // Handle special return kinds
    if (return_frame.kind == FrameKind::Sentinel) {
        stack_.resize(fp_);
        push(result);
        current_func_ = nullptr;
        pc_ = 0;
        return {};
    }

    // Pop the current frame's locals/args
    stack_.resize(fp_);
    
    // Restore the caller's execution state
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
                // Use ContinuationJump as frame kind so the next thunk's return
                // continues the chain via handle_return's ContinuationJump handler.
                setup_frame(dispatch->func, dispatch->closure, 0, FrameKind::ContinuationJump, return_frame.extra);
            } else {
                // Primitive thunk: re-push ContinuationJump frame and recurse
                frames_.push_back(return_frame);
                return handle_return(Nil);
            }
            return {};
        } else {
            // All thunks executed. Now perform the final state restoration.
            LispVal target_cont_val = state_vec->elements[0];
            LispVal v = state_vec->elements[1];
            auto* cont = try_get_as<ObjectKind::Continuation, Continuation>(target_cont_val);
            if (!cont) return std::unexpected(make_type_error("Invalid target continuation"));

            // Find current sentinel to define the boundary
            int32_t sentinel_idx = -1;
            for (int32_t i = static_cast<int32_t>(frames_.size()) - 1; i >= 0; --i) {
                if (frames_[i].kind == FrameKind::Sentinel) {
                    sentinel_idx = i;
                    break;
                }
            }

            stack_ = cont->stack;

            // Reconstruct frames: current sentinel and below, then the captured frames.
            if (sentinel_idx != -1) {
                frames_.resize(sentinel_idx + 1);
                frames_.insert(frames_.end(), cont->frames.begin(), cont->frames.end());
            } else {
                frames_ = cont->frames;
            }

            winding_stack_ = cont->winding_stack;

            // Invalidate any in-flight step command – the stack has changed.
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
        }
        return {};
    } else if (return_frame.kind == FrameKind::DynamicWindBody) {
        // Returned from 'before'. result is ignored.
        if (winding_stack_.empty()) {
            return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::InternalError, "Winding stack empty"}});
        }
        const auto& wind = winding_stack_.back();
        
        auto body_dispatch = dispatch_callee(wind.body, 0, false);
        if (!body_dispatch) return std::unexpected(body_dispatch.error());
        
        if (body_dispatch->action == DispatchAction::SetupFrame) {
            setup_frame(body_dispatch->func, body_dispatch->closure, 0, FrameKind::DynamicWindAfter, wind.after);
        } else {
            // body was primitive
            LispVal body_result = pop();
            auto after_dispatch = dispatch_callee(wind.after, 0, false);
            if (!after_dispatch) return std::unexpected(after_dispatch.error());
            if (after_dispatch->action == DispatchAction::SetupFrame) {
                setup_frame(after_dispatch->func, after_dispatch->closure, 0, FrameKind::DynamicWindCleanup, body_result);
            } else {
                pop(); // ignore after() result
                push(body_result);
                winding_stack_.pop_back();
            }
        }
        return {};
    } else if (return_frame.kind == FrameKind::DynamicWindAfter) {
        // Returned from 'body'. result is the body result.
        if (winding_stack_.empty()) {
            return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::InternalError, "Winding stack empty"}});
        }
        LispVal body_result = result;
        LispVal after_thunk = return_frame.extra;

        auto after_dispatch = dispatch_callee(after_thunk, 0, false);
        if (!after_dispatch) return std::unexpected(after_dispatch.error());

        if (after_dispatch->action == DispatchAction::SetupFrame) {
            setup_frame(after_dispatch->func, after_dispatch->closure, 0, FrameKind::DynamicWindCleanup, body_result);
        } else {
            // after thunk was primitive
            winding_stack_.pop_back();
            push(body_result);
        }
        return {};
    } else if (return_frame.kind == FrameKind::DynamicWindCleanup) {
        // Returned from 'after'. return_frame.extra is the body_result.
        if (winding_stack_.empty()) {
            return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::InternalError, "Winding stack empty"}});
        }
        winding_stack_.pop_back();
        push(return_frame.extra);
        return {};
    }

    // Normal case
    push(result);
    return {};
}

std::expected<void, RuntimeError> VM::do_binary_arithmetic(OpCode op) {
    LispVal b = pop();
    LispVal a = pop();

    // ── AD Dual lifting ────────────────────────────────────────────────
    // If either operand is a Dual heap object, transparently lift the
    // operation so that the backward pass builds a computation graph.
    // This is the core of reverse-on-reverse: the inner backward pass's
    // plain `*`, `+`, `-`, `/` opcodes become graph-building when the
    // outer grad seeds dual-valued adjoints.
    auto* dual_a = try_get_as<ObjectKind::Dual, Dual>(a);
    auto* dual_b = try_get_as<ObjectKind::Dual, Dual>(b);

    if (dual_a || dual_b) {
        // Extract ALL fields from the Dual structs NOW, before any
        // allocations that could invalidate the pointers.
        LispVal pa = dual_a ? dual_a->primal   : a;
        LispVal pb = dual_b ? dual_b->primal   : b;
        LispVal ba = dual_a ? dual_a->backprop  : Nil;
        LispVal bb = dual_b ? dual_b->backprop  : Nil;

        // Compute forward result using plain arithmetic on primals
        push(pa);
        push(pb);
        auto fwd = do_binary_arithmetic(op);
        if (!fwd) return fwd;
        LispVal primal_result = pop();


        // The backward closure is a Primitive that, given an adjoint,
        // applies the chain rule and returns the merged adjoint list.
        // We capture pa, pb, ba, bb, op by value in the C++ lambda.
        // All arithmetic uses dual_binary_op() which properly saves/restores
        // the stack, making nested Dual lifting safe (reverse-on-reverse).
        auto bp_func = [this, pa, pb, ba, bb, op](const std::vector<LispVal>& args)
            -> std::expected<LispVal, RuntimeError>
        {
            if (args.empty()) return std::unexpected(make_type_error("backprop: requires adjoint argument"));
            LispVal adj = args[0];

            // Compute local adjoints for each operand based on op
            LispVal adj_a, adj_b;
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wswitch-enum"
            switch (op) {
                case OpCode::Add:
                    // z = a + b;  dz/da = 1,  dz/db = 1
                    adj_a = adj;
                    adj_b = adj;
                    break;
                case OpCode::Sub:
                    // z = a - b;  dz/da = 1,  dz/db = -1
                    adj_a = adj;
                    {
                        auto neg1 = make_flonum(-1.0);
                        if (!neg1) return std::unexpected(neg1.error());
                        auto r = dual_binary_op(OpCode::Mul, adj, *neg1);
                        if (!r) return r;
                        adj_b = *r;
                    }
                    break;
                case OpCode::Mul:
                    // z = a * b;  dz/da = b,  dz/db = a
                    {
                        auto r1 = dual_binary_op(OpCode::Mul, adj, pb);
                        if (!r1) return r1;
                        adj_a = *r1;
                    }
                    {
                        auto r2 = dual_binary_op(OpCode::Mul, adj, pa);
                        if (!r2) return r2;
                        adj_b = *r2;
                    }
                    break;
                case OpCode::Div:
                    // z = a / b;  dz/da = 1/b,  dz/db = -a/b²
                    {
                        auto r1 = dual_binary_op(OpCode::Div, adj, pb);
                        if (!r1) return r1;
                        adj_a = *r1;
                    }
                    {
                        // adj_b = adj * (-a / b²)
                        auto b_sq = dual_binary_op(OpCode::Mul, pb, pb);
                        if (!b_sq) return b_sq;
                        auto a_over_bsq = dual_binary_op(OpCode::Div, pa, *b_sq);
                        if (!a_over_bsq) return a_over_bsq;
                        auto neg1 = make_flonum(-1.0);
                        if (!neg1) return std::unexpected(neg1.error());
                        auto neg_a_over_bsq = dual_binary_op(OpCode::Mul, *neg1, *a_over_bsq);
                        if (!neg_a_over_bsq) return neg_a_over_bsq;
                        auto r5 = dual_binary_op(OpCode::Mul, adj, *neg_a_over_bsq);
                        if (!r5) return r5;
                        adj_b = *r5;
                    }
                    break;
                default:
                    return std::unexpected(make_type_error("backprop: unknown arithmetic op"));
            }
#pragma clang diagnostic pop

            // Call child backpropagators and merge results
            LispVal result_a = Nil;
            if (ba != Nil) {
                auto ra = call_value(ba, {adj_a});
                if (!ra) return std::unexpected(ra.error());
                result_a = *ra;
            }

            LispVal result_b = Nil;
            if (bb != Nil) {
                auto rb = call_value(bb, {adj_b});
                if (!rb) return std::unexpected(rb.error());
                result_b = *rb;
            }

            // append result_a result_b
            if (result_a == Nil) return result_b;
            if (result_b == Nil) return result_a;

            // Walk to end of result_a list and splice result_b
            std::vector<LispVal> elems_a;
            LispVal cur = result_a;
            while (cur != Nil) {
                auto* c = try_get_as<ObjectKind::Cons, Cons>(cur);
                if (!c) break;
                elems_a.push_back(c->car);
                cur = c->cdr;
            }
            LispVal merged = result_b;
            for (auto it = elems_a.rbegin(); it != elems_a.rend(); ++it) {
                auto c = make_cons(heap_, *it, merged);
                if (!c) return std::unexpected(c.error());
                merged = *c;
            }
            return merged;
        };

        // Collect GC roots for the captured LispVals so GC won't free them
        std::vector<LispVal> roots;
        roots.reserve(4);
        roots.push_back(pa);
        roots.push_back(pb);
        if (ba != Nil) roots.push_back(ba);
        if (bb != Nil) roots.push_back(bb);

        // Protect primal_result from GC during allocations below
        temp_roots_.push_back(primal_result);

        // Allocate the backprop as a Primitive (with GC roots)
        auto bp_prim = make_primitive(heap_, bp_func, 1, false, std::move(roots));
        if (!bp_prim) {
            temp_roots_.pop_back();
            return std::unexpected(bp_prim.error());
        }

        // Allocate the result Dual
        auto dual_result = make_dual(heap_, primal_result, *bp_prim);
        temp_roots_.pop_back();
        if (!dual_result) return std::unexpected(dual_result.error());

        push(*dual_result);
        return {};
    }
    // ── End AD Dual lifting ────────────────────────────────────────────

    auto num_a = classify_numeric(a, heap_);
    auto num_b = classify_numeric(b, heap_);

    if (!num_a.is_valid() || !num_b.is_valid()) {
        return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError, "Expected a number"}});
    }

    // Double-precision path (used when either operand is a flonum, or on integer overflow)
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

    // If either operand is a flonum, result is flonum
    if (num_a.is_flonum() || num_b.is_flonum()) {
        auto result = apply_double(op, num_a.as_double(), num_b.as_double());
        if (!result) return std::unexpected(result.error());
        return push_flonum(*result);
    }

    // Both are exact integers — use overflow-checked arithmetic
    int64_t int_a = num_a.int_val;
    int64_t int_b = num_b.int_val;

    // Division: special cases for zero and non-exact results
    if (op == OpCode::Div) {
        if (int_b == 0) {
            return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError, "Division by zero"}});
        }
        if (int_a % int_b != 0) {
            return push_flonum(static_cast<double>(int_a) / static_cast<double>(int_b));
        }
        // Exact division — no overflow possible (result magnitude ≤ |int_a|)
        auto res = make_fixnum(heap_, int_a / int_b);
        if (!res) return std::unexpected(res.error());
        push(*res);
        return {};
    }

    // Add / Sub / Mul: overflow-checked, promote to double on overflow
    int64_t int_result;
    bool overflowed = false;

    if (op == OpCode::Add)       overflowed = detail::add_overflow(int_a, int_b, &int_result);
    else if (op == OpCode::Sub)  overflowed = detail::sub_overflow(int_a, int_b, &int_result);
    else if (op == OpCode::Mul)  overflowed = detail::mul_overflow(int_a, int_b, &int_result);
    else return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::NotImplemented, "Unknown arithmetic op"}});

    if (overflowed) {
        // Promote to double — use pre-overflow operand values
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
    // Build a list from args at fp_+required .. fp_+argc-1
    // and store the list at fp_+required (the rest param slot).
    LispVal rest_list = Nil;
    for (int32_t i = static_cast<int32_t>(argc) - 1; i >= static_cast<int32_t>(required); --i) {
        auto cons = make_cons(heap_, stack_[fp_ + i], rest_list);
        if (!cons) return std::unexpected(cons.error());
        rest_list = *cons;
    }
    stack_[fp_ + required] = rest_list;
    return {};
}

// ============================================================================
// Debug / inspect API
// ============================================================================


std::vector<FrameInfo> VM::get_frames() const {
    std::vector<FrameInfo> result;

    // Current (innermost) frame
    if (current_func_) {
        FrameInfo fi;
        fi.func_name  = current_func_->name;
        fi.span        = current_func_->span_at(pc_ > 0 ? pc_ - 1 : 0);
        fi.frame_index = 0;
        result.push_back(fi);
    }

    // Walk saved frames in reverse order (most recent caller first)
    for (int i = static_cast<int>(frames_.size()) - 1; i >= 0; --i) {
        const auto& f = frames_[static_cast<std::size_t>(i)];
        if (f.kind == FrameKind::Sentinel) break; // stop at execution boundary
        if (f.func == nullptr) continue;
        FrameInfo fi;
        fi.func_name  = f.func->name;
        // pc in a saved frame points to the instruction *after* the call site
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
        // Walk saved frames (most recent caller first)
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
    // Only expose the slots that the emitter gave real names to.
    // stack_size includes +32 temporary headroom that we must not expose as
    // variables — they are uninitialized scratch space.  If local_names was not
    // populated (e.g. module-init functions) fall back to just the parameters.
    uint32_t num_slots = func->local_names.empty()
                         ? num_params
                         : static_cast<uint32_t>(func->local_names.size());
    for (uint32_t slot = 0; slot < num_slots; ++slot) {
        std::size_t stack_idx = static_cast<std::size_t>(frame_fp) + slot;
        if (stack_idx >= stack_.size()) break;
        VarEntry e;
        // Use the real name from local_names if available; fall back to %N placeholder.
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
            // Use the real name from upval_names if available; fall back to &N placeholder.
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

// ============================================================================
// Exception Throw
// ============================================================================

std::expected<void, RuntimeError> VM::do_throw(LispVal tag, LispVal value,
                                                reader::lexer::Span span) {
    // Search the catch stack from top (most recent) to bottom for a matching tag.
    for (int i = static_cast<int>(catch_stack_.size()) - 1; i >= 0; --i) {
        const CatchFrame& cf = catch_stack_[static_cast<std::size_t>(i)];
        // Match if tags are equal (same symbol) OR the catch frame is a catch-all (Nil tag).
        bool matches = (cf.tag == Nil) || values_eqv(cf.tag, tag);
        if (matches) {
            // Pop all catch frames from this one upward.
            catch_stack_.resize(static_cast<std::size_t>(i));

            // Restore VM frame state.
            frames_.resize(cf.frame_count);
            winding_stack_.resize(cf.wind_count);

            // Restore stack to the saved top, then push the caught value.
            stack_.resize(cf.stack_top);
            push(value);

            // Restore execution context to the function containing SetupCatch.
            current_func_    = cf.func;
            fp_              = cf.fp;
            current_closure_ = cf.closure;
            pc_              = cf.handler_pc;

            return {};
        }
    }

    // No matching handler — notify debugger then propagate as UserThrow.
    std::string tag_str = "unknown-tag";
    if (ops::is_boxed(tag) && ops::tag(tag) == Tag::Symbol) {
        auto name = intern_table_.get_string(ops::payload(tag));
        if (name) tag_str = std::string(*name);
    }
    std::string msg = "Unhandled raise: " + tag_str;

    if (debug_) debug_->notify_exception(msg, span);

    return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::UserThrow, msg}});
}

// ============================================================================
// Unification helpers
// ============================================================================

LispVal VM::deref(LispVal v) {
    // Follow the binding chain until we hit an unbound variable or a non-LogicVar.
    for (;;) {
        auto* lv = try_get_as<ObjectKind::LogicVar, types::LogicVar>(v);
        if (!lv || !lv->binding.has_value()) return v;
        v = *lv->binding;
    }
}

bool VM::occurs_check(LispVal lvar, LispVal term) {
    // Returns true if lvar appears anywhere inside term (cycle would be created).
    term = deref(term);
    if (term == lvar) return true;
    if (auto* cons = try_get_as<ObjectKind::Cons, types::Cons>(term)) {
        return occurs_check(lvar, cons->car) || occurs_check(lvar, cons->cdr);
    }
    if (auto* vec = try_get_as<ObjectKind::Vector, types::Vector>(term)) {
        for (auto elem : vec->elements)
            if (occurs_check(lvar, elem)) return true;
    }
    return false;
}

bool VM::unify(LispVal a, LispVal b) {
    a = deref(a);
    b = deref(b);

    // Identical values (includes two unbound vars that are the same object)
    if (a == b) return true;

    // ── Helper: check domain constraint before committing a binding ───────────
    // When an unbound variable with a CLP domain is about to be bound to a
    // concrete (non-variable) ground value, verify the value lies in the domain.
    // If `candidate` is itself an unbound logic variable, skip the check — the
    // constraint will be re-evaluated when that variable is eventually grounded
    // (forward-checking style).
    auto check_domain = [&](LispVal lvar, LispVal candidate) -> bool {
        LispVal ground = deref(candidate);
        // If candidate deref's to a logic variable, defer the check.
        if (try_get_as<ObjectKind::LogicVar, types::LogicVar>(ground))
            return true;
        // Ground value — look up any domain constraint on the variable.
        auto id = ops::payload(lvar);
        const auto* dom = constraint_store_.get_domain(id);
        if (!dom) return true;  // no domain constraint
        auto n = classify_numeric(ground, heap_);
        if (!n.is_valid() || n.is_flonum()) return false;  // non-integer into integer domain
        return clp::domain_contains_int(*dom, n.int_val);
    };

    // a is an unbound logic variable
    if (auto* lva = try_get_as<ObjectKind::LogicVar, types::LogicVar>(a)) {
        if (!lva->binding.has_value()) {
            if (occurs_check(a, b)) return false;   // would create cycle
            if (!check_domain(a, b)) return false;  // CLP domain violation
            lva->binding = b;
            trail_stack_.push_back(a);
            return true;
        }
    }

    // b is an unbound logic variable
    if (auto* lvb = try_get_as<ObjectKind::LogicVar, types::LogicVar>(b)) {
        if (!lvb->binding.has_value()) {
            if (occurs_check(b, a)) return false;   // would create cycle
            if (!check_domain(b, a)) return false;  // CLP domain violation
            lvb->binding = a;
            trail_stack_.push_back(b);
            return true;
        }
    }

    // Both are Cons — unify car and cdr
    if (auto* ca = try_get_as<ObjectKind::Cons, types::Cons>(a)) {
        if (auto* cb = try_get_as<ObjectKind::Cons, types::Cons>(b)) {
            return unify(ca->car, cb->car) && unify(ca->cdr, cb->cdr);
        }
        return false;
    }

    // Both are Vectors of the same length — unify element-wise
    if (auto* va = try_get_as<ObjectKind::Vector, types::Vector>(a)) {
        if (auto* vb = try_get_as<ObjectKind::Vector, types::Vector>(b)) {
            if (va->elements.size() != vb->elements.size()) return false;
            for (std::size_t i = 0; i < va->elements.size(); ++i)
                if (!unify(va->elements[i], vb->elements[i])) return false;
            return true;
        }
        return false;
    }

    // Structural mismatch
    return false;
}

} // namespace eta::runtime::vm

