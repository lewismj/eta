#include "vm.h"
#include "eta/runtime/factory.h"
#include "eta/runtime/types/types.h"
#include "eta/runtime/types/primitive.h"
#include "eta/runtime/memory/value_visit.h"
#include "eta/runtime/string_view.h"

#include <bit>

namespace eta::runtime::vm {

using namespace eta::runtime::memory::value_visit;

using namespace eta::runtime::memory::factory;
using namespace eta::runtime::types;

VM::VM(Heap& heap, InternTable& intern_table)
    : heap_(heap), intern_table_(intern_table) {
    stack_.reserve(1024);
    frames_.reserve(64);
}

std::expected<LispVal, RuntimeError> VM::execute(const BytecodeFunction& main) {
    current_func_ = &main;
    pc_ = 0;
    fp_ = 0;
    current_closure_ = 0; // Top-level
    
    // Initial frame
    frames_.push_back({current_func_, pc_, fp_, current_closure_});
    
    auto res = run_loop();
    if (!res) return std::unexpected(res.error());
    
    return pop();
}

// Unified helper to dispatch a callee (Closure, Continuation, or Primitive)
std::expected<DispatchResult, RuntimeError> VM::dispatch_callee(LispVal callee, uint32_t argc, bool is_tail) {
    // Use try_get_as for consistent heap access pattern

    if (auto* closure = try_get_as<ObjectKind::Closure, Closure>(callee)) {
        // Arity check early (fail fast)
        if (argc != closure->func->arity) {
            return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::InvalidArity, "Wrong number of arguments"}});
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

        // Restore state
        stack_ = cont->stack;
        frames_ = cont->frames;
        winding_stack_ = cont->winding_stack;

        // Restore current frame state
        const auto& f = frames_.back();
        current_func_ = f.func;
        pc_ = f.pc;
        fp_ = f.fp;
        current_closure_ = f.closure;

        // Return v from continuation
        push(v);
        return DispatchResult{DispatchAction::Continue, nullptr, 0};
    }

    if (auto* prim = try_get_as<ObjectKind::Primitive, Primitive>(callee)) {
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
    while (pc_ < current_func_->code.size()) {
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

                // Call producer with 0 arguments
                auto prod_dispatch = dispatch_callee(producer, 0, /*is_tail=*/false);
                if (!prod_dispatch) return std::unexpected(prod_dispatch.error());

                // We need to return to the consumer after producer returns.
                // We push a special frame that will be handled by OpCode::Return.
                frames_.push_back({current_func_, pc_, fp_, current_closure_, FrameKind::CallWithValuesConsumer, consumer});

                if (prod_dispatch->action == DispatchAction::SetupFrame) {
                    current_func_ = prod_dispatch->func;
                    current_closure_ = prod_dispatch->closure;
                    fp_ = static_cast<uint32_t>(stack_.size());
                    pc_ = 0;
                } else {
                    // Producer was a primitive - result already pushed.
                    // We need to immediately transition to the consumer.
                    // This is handled by pretending we just returned.
                    LispVal result = pop();
                    Frame back = frames_.back();
                    frames_.pop_back();

                    // Restore state (though it hasn't really changed for primitive call)
                    // and then call consumer with producer's result(s).
                    unpack_to_stack(result);
                    uint32_t argc = static_cast<uint32_t>(stack_.size() - back.fp);

                    auto cons_dispatch = dispatch_callee(back.extra, argc, /*is_tail=*/false);
                    if (!cons_dispatch) return std::unexpected(cons_dispatch.error());

                    if (cons_dispatch->action == DispatchAction::SetupFrame) {
                        setup_frame(cons_dispatch->func, cons_dispatch->closure, argc, /*is_tail=*/false);
                    }
                }
                break;
            }
            case OpCode::DynamicWind: {
                // Stack: [before, body, after] (after on top)
                LispVal after_thunk = pop();
                LispVal body_thunk = pop();
                LispVal before_thunk = pop();

                // Push to winding stack
                winding_stack_.push_back({before_thunk, body_thunk, after_thunk});

                // Call before() with 0 arguments
                auto before_dispatch = dispatch_callee(before_thunk, 0, /*is_tail=*/false);
                if (!before_dispatch) return std::unexpected(before_dispatch.error());

                // Return to body after before()
                frames_.push_back({current_func_, pc_, fp_, current_closure_, FrameKind::DynamicWindBody, body_thunk});

                if (before_dispatch->action == DispatchAction::SetupFrame) {
                    current_func_ = before_dispatch->func;
                    current_closure_ = before_dispatch->closure;
                    fp_ = static_cast<uint32_t>(stack_.size());
                    pc_ = 0;
                } else {
                    // before was primitive, already executed
                    pop(); // discard result
                    Frame back = frames_.back();
                    frames_.pop_back();

                    // Call body()
                    auto body_dispatch = dispatch_callee(back.extra, 0, /*is_tail=*/false);
                    if (!body_dispatch) return std::unexpected(body_dispatch.error());

                    // Return to after after body()
                    frames_.push_back({back.func, back.pc, back.fp, back.closure, FrameKind::DynamicWindAfter, after_thunk});

                    if (body_dispatch->action == DispatchAction::SetupFrame) {
                        setup_frame(body_dispatch->func, body_dispatch->closure, 0, /*is_tail=*/false);
                    } else {
                        // body was primitive
                        LispVal body_result = pop();
                        Frame back2 = frames_.back();
                        frames_.pop_back();

                        // Call after()
                        auto after_dispatch = dispatch_callee(back2.extra, 0, /*is_tail=*/false);
                        if (!after_dispatch) return std::unexpected(after_dispatch.error());

                        if (after_dispatch->action == DispatchAction::SetupFrame) {
                            push(body_result); // Save body result to restore later
                            setup_frame(after_dispatch->func, after_dispatch->closure, 1, /*is_tail=*/false);
                        } else {
                            pop(); // discard after result
                            push(body_result);
                            // We also need to pop from winding stack if we fully finish here?
                            // Actually, we only pop from winding stack when body returns or if we jump out.
                            // If everything was primitive, we are done with this dynamic-wind.
                            winding_stack_.pop_back();
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
                if (auto* closure = try_get_as<ObjectKind::Closure, Closure>(current_closure_)) {
                    push(closure->upvals[instr.arg]);
                    break;
                }
                return std::unexpected(make_type_error("LoadUpval outside of a closure"));
            }
            case OpCode::StoreUpval: {
                if (auto* closure = try_get_as<ObjectKind::Closure, Closure>(current_closure_)) {
                    closure->upvals[instr.arg] = pop();
                    break;
                }
                return std::unexpected(make_type_error("StoreUpval outside of a closure"));
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

                auto dispatch_res = dispatch_callee(callee, argc, /*is_tail=*/false);
                if (!dispatch_res) return std::unexpected(dispatch_res.error());

                if (dispatch_res->action == DispatchAction::SetupFrame) {
                    setup_frame(dispatch_res->func, dispatch_res->closure, argc, /*is_tail=*/false);
                }
                break;
            }
            case OpCode::MakeClosure: {
                // arg is (const_idx << 16) | num_upvals
                uint32_t const_idx = instr.arg >> 16;
                uint32_t num_upvals = instr.arg & 0xFFFF;
                
                // The constant encodes a function index (high bit set) or legacy raw pointer
                LispVal func_val = current_func_->constants[const_idx];
                constexpr uint64_t FUNC_INDEX_TAG = 1ULL << 63;

                const BytecodeFunction* bfunc = nullptr;
                if (func_val & FUNC_INDEX_TAG) {
                    // New-style: function index
                    uint32_t func_idx = static_cast<uint32_t>(func_val & ~FUNC_INDEX_TAG);
                    bfunc = resolve_function(func_idx);
                    if (!bfunc) {
                        return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::InvalidInstruction, "Invalid function index"}});
                    }
                } else {
                    // Legacy: raw pointer (for backwards compatibility)
                    bfunc = reinterpret_cast<const BytecodeFunction*>(func_val);
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
            case OpCode::Cons: {
                LispVal cdr = pop();
                LispVal car = pop();
                auto res = make_cons(heap_, car, cdr);
                if (!res) return std::unexpected(res.error());
                push(*res);
                break;
            }
            case OpCode::Car: {
                LispVal v = pop();
                if (auto* cons = try_get_as<ObjectKind::Cons, Cons>(v)) {
                    push(cons->car);
                    break;
                }
                return std::unexpected(make_type_error("Not a pair"));
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
                push(a == b ? True : False);
                break;
            }
            case OpCode::Cdr: {
                LispVal v = pop();
                if (auto* cons = try_get_as<ObjectKind::Cons, Cons>(v)) {
                    push(cons->cdr);
                    break;
                }
                return std::unexpected(make_type_error("Not a pair"));
            }
            case OpCode::TailCall: {
                uint32_t argc = instr.arg;
                LispVal callee = pop();

                auto dispatch_res = dispatch_callee(callee, argc, /*is_tail=*/true);
                if (!dispatch_res) return std::unexpected(dispatch_res.error());

                if (dispatch_res->action == DispatchAction::TailReuse) {
                    current_func_ = dispatch_res->func;
                    current_closure_ = dispatch_res->closure;

                    // Move new arguments to current fp_
                    uint32_t new_args_start = static_cast<uint32_t>(stack_.size() - argc);
                    for (uint32_t i = 0; i < argc; ++i) {
                        stack_[fp_ + i] = stack_[new_args_start + i];
                    }
                    stack_.resize(fp_ + argc);
                    pc_ = 0;
                }
                // DispatchAction::Continue means result is already pushed (primitive/continuation)
                break;
            }
            case OpCode::CallCC: {
                LispVal consumer = pop();
                
                // Capture current state
                auto cont_res = make_continuation(heap_, stack_, frames_, winding_stack_);
                if (!cont_res) return std::unexpected(cont_res.error());
                
                // Push the continuation as argument
                push(*cont_res);
                
                // Dispatch consumer with 1 argument
                auto dispatch_res = dispatch_callee(consumer, 1, /*is_tail=*/false);
                if (!dispatch_res) return std::unexpected(dispatch_res.error());

                if (dispatch_res->action == DispatchAction::SetupFrame) {
                    frames_.push_back({current_func_, pc_, fp_, current_closure_});
                    current_func_ = dispatch_res->func;
                    current_closure_ = dispatch_res->closure;
                    fp_ = static_cast<uint32_t>(stack_.size() - 1);
                    pc_ = 0;
                }
                break;
            }
            case OpCode::Return: {
                LispVal result = pop();
                
                // Pop current frame
                if (frames_.empty()) {
                    return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::StackUnderflow, "Return from empty frame stack"}});
                }
                Frame current_frame = frames_.back();
                frames_.pop_back();
                
                // Cleanup stack: pop locals and arguments of returned function
                // For DynamicWindAfter, we need body_result which is on stack before resize
                LispVal body_result_for_after = (current_frame.kind == FrameKind::DynamicWindAfter) ? stack_[fp_] : 0;
                stack_.resize(fp_);

                // Handle internal return points
                if (current_frame.kind == FrameKind::CallWithValuesConsumer) {
                    // result contains the values from the producer.
                    // We need to unpack them and call the consumer.
                    unpack_to_stack(result);
                    uint32_t argc = static_cast<uint32_t>(stack_.size() - fp_);

                    LispVal consumer = current_frame.extra;
                    auto dispatch_res = dispatch_callee(consumer, argc, /*is_tail=*/false);
                    if (!dispatch_res) return std::unexpected(dispatch_res.error());

                    // Restore the frame that CALLED call-with-values
                    if (frames_.empty()) {
                        return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::StackUnderflow, "CallWithValues caller frame missing"}});
                    }
                    const auto& caller_frame = frames_.back();
                    current_func_ = caller_frame.func;
                    pc_ = caller_frame.pc;
                    fp_ = caller_frame.fp;
                    current_closure_ = caller_frame.closure;

                    if (dispatch_res->action == DispatchAction::SetupFrame) {
                        setup_frame(dispatch_res->func, dispatch_res->closure, argc, /*is_tail=*/false);
                    }
                    // If primitive, result is already on stack and we continue in caller_frame
                    break;
                } else if (current_frame.kind == FrameKind::DynamicWindBody) {
                    // Returned from 'before' thunk. result is ignored.
                    // Now call the 'body' thunk.
                    if (winding_stack_.empty()) {
                        return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::InternalError, "Winding stack empty in DynamicWindBody return"}});
                    }
                    const auto& wind = winding_stack_.back();
                    
                    auto body_dispatch = dispatch_callee(wind.body, 0, /*is_tail=*/false);
                    if (!body_dispatch) return std::unexpected(body_dispatch.error());

                    // Restore the frame that CALLED dynamic-wind
                    if (frames_.empty()) {
                         return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::StackUnderflow, "DynamicWind caller frame missing"}});
                    }
                    const auto& caller_frame = frames_.back();
                    current_func_ = caller_frame.func;
                    pc_ = caller_frame.pc;
                    fp_ = caller_frame.fp;
                    current_closure_ = caller_frame.closure;

                    // Push a frame to call 'after' when 'body' returns
                    frames_.push_back({current_func_, pc_, fp_, current_closure_, FrameKind::DynamicWindAfter, wind.after});

                    if (body_dispatch->action == DispatchAction::SetupFrame) {
                        setup_frame(body_dispatch->func, body_dispatch->closure, 0, /*is_tail=*/false);
                    } else {
                        // Body was primitive, execute 'after' immediately
                        LispVal body_result = pop();
                        // Pop the DynamicWindAfter frame we just pushed
                        frames_.pop_back();

                        auto after_dispatch = dispatch_callee(wind.after, 0, /*is_tail=*/false);
                        if (!after_dispatch) return std::unexpected(after_dispatch.error());

                        if (after_dispatch->action == DispatchAction::SetupFrame) {
                            push(body_result); // Save body result to restore after 'after' returns
                            setup_frame(after_dispatch->func, after_dispatch->closure, 1, /*is_tail=*/false);
                        } else {
                            // After was also primitive
                            pop(); // ignore after result
                            push(body_result);
                            winding_stack_.pop_back();
                        }
                    }
                    break;
                } else if (current_frame.kind == FrameKind::DynamicWindAfter) {
                    // Returned from 'after' thunk (invoked after 'body' finished).
                    // The result of dynamic-wind is the result of 'body', which we stored as an "argument" to 'after'.
                    if (winding_stack_.empty()) {
                        return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::InternalError, "Winding stack empty in DynamicWindAfter return"}});
                    }
                    winding_stack_.pop_back();

                    // Restore the frame that CALLED dynamic-wind
                    if (frames_.empty()) {
                        return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::StackUnderflow, "DynamicWind caller frame missing"}});
                    }
                    const auto& caller_frame = frames_.back();
                    current_func_ = caller_frame.func;
                    pc_ = caller_frame.pc;
                    fp_ = caller_frame.fp;
                    current_closure_ = caller_frame.closure;

                    // The body_result was stored on the stack at fp_ when setup_frame was called for 'after'.
                    // Actually, if 'after' was a SetupFrame, it had body_result as its only "argument".
                    // But we just resized stack to fp_, so we need to pop it before that or save it.
                    // Wait, stack_.resize(fp_) already happened. 
                    // When 'after' was called as SetupFrame, we did push(body_result) then setup_frame(..., 1).
                    // So fp_ of 'after' frame was at the position of body_result.
                    // So we can't get it from stack_[fp_] AFTER resize(fp_).
                    
                    // I should have saved body_result before resize.
                    // Let's fix the logic above to save body_result.
                    // Actually, I can just use 'result' if I changed my mind about what 'after' returns,
                    // but Scheme says it returns body's result.
                    
                    // If 'after' was called with 1 arg (body_result), and it's a Scheme function,
                    // its arguments are still on the stack at fp_ and above.
                    // But we want to return body_result.
                    
                    // Let's re-read: DynamicWindAfter was pushed with extra = after_thunk.
                    // When body returned, we called after.
                    
                    // Actually, the easiest way to store body_result is in 'extra' or another field of the frame.
                    // But 'extra' is already used for the thunk to call.
                    
                    // Let's use the result from the stack BEFORE resize if kind is After.
                    // Or better, let's fix the frame kind handling.
                    
                    // If kind == DynamicWindAfter, then the top of the stack (before resize) is NOT the result of 'after'
                    // that we want to return, we want the body_result which was passed as arg to 'after'.
                    
                    // Let's use a simpler approach: result of dynamic-wind is indeed what we want.
                    // If we want to strictly follow Scheme, we must preserve body_result.
                    
                    // Let's re-evaluate DynamicWindAfter.
                    // When Body returns, we push Kind::After, and call After.
                    // If After is SetupFrame, we push(body_result).
                    // So when After returns, stack_[fp_] is body_result.
                    
                    winding_stack_.pop_back();
                    
                    if (frames_.empty()) {
                        push(body_result_for_after);
                        return {};
                    }
                    const auto& cf = frames_.back();
                    current_func_ = cf.func;
                    pc_ = cf.pc;
                    fp_ = cf.fp;
                    current_closure_ = cf.closure;
                    push(body_result_for_after);
                    break;
                }
                
                if (frames_.empty()) {
                    push(result);
                    return {};
                }
                
                const auto& f = frames_.back();
                current_func_ = f.func;
                pc_ = f.pc;
                fp_ = f.fp;
                current_closure_ = f.closure;
                
                push(result);
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
enum class NumType { Fixnum, Flonum, Invalid };

// Result type that preserves exact integer values
struct NumericValue {
    NumType type;
    int64_t int_val;    // Valid when type == Fixnum
    double float_val;   // Valid when type == Flonum
};

// Visitor that classifies a LispVal into numeric types.
// This uses the centralized ValueVisitor pattern from value_visit.h
// to avoid duplicating tag dispatch logic.
struct NumericClassifier : ValueVisitor<NumericValue> {
    Heap& heap;
    explicit NumericClassifier(Heap& h) : heap(h) {}

    NumericValue visit_fixnum(std::int64_t v) override {
        return {NumType::Fixnum, v, 0.0};
    }
    NumericValue visit_flonum(double v) override {
        return {NumType::Flonum, 0, v};
    }
    NumericValue visit_heapref(uint64_t obj_id) override {
        // Check for heap-allocated fixnum (64-bit integers that don't fit in immediate)
        HeapEntry entry;
        if (heap.try_get(obj_id, entry) && entry.header.kind == ObjectKind::Fixnum) {
            return {NumType::Fixnum, *static_cast<int64_t*>(entry.ptr), 0.0};
        }
        return {NumType::Invalid, 0, 0.0};
    }
    // Non-numeric types return Invalid
    NumericValue visit_char(char32_t) override { return {NumType::Invalid, 0, 0.0}; }
    NumericValue visit_string(uint64_t) override { return {NumType::Invalid, 0, 0.0}; }
    NumericValue visit_symbol(uint64_t) override { return {NumType::Invalid, 0, 0.0}; }
    NumericValue visit_nan() override { return {NumType::Flonum, 0, std::numeric_limits<double>::quiet_NaN()}; }
    NumericValue visit_nil() override { return {NumType::Invalid, 0, 0.0}; }
};

std::expected<void, RuntimeError> VM::do_binary_arithmetic(OpCode op) {
    LispVal b = pop();
    LispVal a = pop();

    // Use ValueVisitor to classify numeric values
    NumericClassifier classifier(heap_);
    auto num_a = visit_value(a, classifier);
    auto num_b = visit_value(b, classifier);

    if (num_a.type == NumType::Invalid || num_b.type == NumType::Invalid) {
        return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError, "Expected a number"}});
    }

    // Unified arithmetic operation helper
    // Note: Only arithmetic opcodes should be passed here
    auto apply_arith_double = [](OpCode op, double a, double b) -> std::expected<double, RuntimeError> {
        if (op == OpCode::Add) return a + b;
        if (op == OpCode::Sub) return a - b;
        if (op == OpCode::Mul) return a * b;
        if (op == OpCode::Div) {
            if (b == 0) {
                return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError, "Division by zero"}});
            }
            return a / b;
        }
        return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::NotImplemented, "Unknown arithmetic op"}});
    };

    // Note: Only arithmetic opcodes should be passed here
    auto apply_arith_int = [](OpCode op, int64_t a, int64_t b) -> int64_t {
        if (op == OpCode::Add) return a + b;
        if (op == OpCode::Sub) return a - b;
        if (op == OpCode::Mul) return a * b;
        if (op == OpCode::Div) return a / b;
        return 0; // Should never reach here
    };

    // If either operand is a flonum, result is flonum
    bool result_is_flonum = (num_a.type == NumType::Flonum || num_b.type == NumType::Flonum);

    if (result_is_flonum) {
        double val_a = (num_a.type == NumType::Flonum) ? num_a.float_val : static_cast<double>(num_a.int_val);
        double val_b = (num_b.type == NumType::Flonum) ? num_b.float_val : static_cast<double>(num_b.int_val);

        auto result = apply_arith_double(op, val_a, val_b);
        if (!result) return std::unexpected(result.error());

        auto res = make_flonum(*result);
        if (!res) return std::unexpected(res.error());
        push(*res);
    } else {
        // Both are exact integers - perform exact integer arithmetic
        int64_t int_a = num_a.int_val;
        int64_t int_b = num_b.int_val;

        // Division special case: may produce flonum if not exact
        if (op == OpCode::Div) {
            if (int_b == 0) {
                return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError, "Division by zero"}});
            }
            if (int_a % int_b != 0) {
                // Non-exact division produces flonum
                double result = static_cast<double>(int_a) / static_cast<double>(int_b);
                auto res = make_flonum(result);
                if (!res) return std::unexpected(res.error());
                push(*res);
                return {};
            }
        }

        // All other cases produce exact integer
        int64_t result = apply_arith_int(op, int_a, int_b);

        auto res = make_fixnum(heap_, result);
        if (!res) return std::unexpected(res.error());
        push(*res);
    }

    return {};
}

} // namespace eta::runtime::vm
