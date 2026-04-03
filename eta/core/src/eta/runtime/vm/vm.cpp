#include "vm.h"
#include <iostream>
#include "eta/runtime/factory.h"
#include "eta/runtime/types/types.h"
#include "eta/runtime/types/primitive.h"
#include "eta/runtime/memory/value_visit.h"
#include "eta/runtime/memory/mark_sweep_gc.h"
#include "eta/runtime/string_view.h"
#include "eta/runtime/numeric_value.h"
#include "eta/runtime/overflow.h"
#include "eta/runtime/port.h"
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
    });
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
        const auto& wind = winding_stack_.back();
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

} // namespace eta::runtime::vm

// pack_rest_args: Build a list from args at fp_+required .. fp_+argc-1
// and store the list at fp_+required (the rest param slot).
namespace eta::runtime::vm {

std::expected<void, RuntimeError> VM::pack_rest_args(uint32_t argc, uint32_t required) {
    LispVal rest_list = Nil;
    for (int32_t i = static_cast<int32_t>(argc) - 1; i >= static_cast<int32_t>(required); --i) {
        auto cons = make_cons(heap_, stack_[fp_ + i], rest_list);
        if (!cons) return std::unexpected(cons.error());
        rest_list = *cons;
    }
    stack_[fp_ + required] = rest_list;
    return {};
}

} // namespace eta::runtime::vm

