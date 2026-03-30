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

bool VM::values_eqv(LispVal a, LispVal b) {
    // Fast path: bit-identical values are always equal
    if (a == b) return true;
    
    // If either is not boxed (raw double), they would have matched above if equal
    if (!ops::is_boxed(a) || !ops::is_boxed(b)) return false;
    
    // For eqv?, only strings need content comparison
    // Symbols with same intern ID would have matched in fast path
    
    // Check if both are strings (interned or heap)
    if (StringView::is_string(a, heap_) && StringView::is_string(b, heap_)) {
        // Use StringView for unified string comparison
        auto res = StringView::equal(a, b, intern_table_, heap_);
        return res.has_value() && *res;
    }
    
    // Different types or non-matching non-string values
    return false;
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
                frames_.push_back({current_func_, pc_, fp_, current_closure_, FrameKind::CallWithValuesConsumer, consumer});

                if (prod_dispatch->action == DispatchAction::SetupFrame) {
                    setup_frame(prod_dispatch->func, prod_dispatch->closure, 0);
                } else {
                    // Producer was a primitive - result already pushed.
                    auto res = handle_return(pop());
                    if (!res) return std::unexpected(res.error());
                }
                break;
            }
            case OpCode::DynamicWind: {
                // Stack: [before, body, after] (after on top)
                LispVal after_thunk = pop();
                LispVal body_thunk = pop();
                LispVal before_thunk = pop();

                winding_stack_.push_back({before_thunk, body_thunk, after_thunk});

                // Call before() with 0 arguments
                auto before_dispatch = dispatch_callee(before_thunk, 0, /*is_tail=*/false);
                if (!before_dispatch) return std::unexpected(before_dispatch.error());

                // Return to body after before()
                frames_.push_back({current_func_, pc_, fp_, current_closure_, FrameKind::DynamicWindBody, body_thunk});

                if (before_dispatch->action == DispatchAction::SetupFrame) {
                    setup_frame(before_dispatch->func, before_dispatch->closure, 0);
                } else {
                    // before was primitive
                    auto res = handle_return(pop());
                    if (!res) return std::unexpected(res.error());
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

                auto dispatch_res = dispatch_callee(callee, argc, /*is_tail=*/false);
                if (!dispatch_res) return std::unexpected(dispatch_res.error());

                if (dispatch_res->action == DispatchAction::SetupFrame) {
                    setup_frame(dispatch_res->func, dispatch_res->closure, argc);
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
                if (func_val & FUNC_INDEX_TAG) {
                    // New-style: function index
                    uint32_t func_idx = static_cast<uint32_t>(func_val & ~FUNC_INDEX_TAG);
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
                
                auto cont_res = make_continuation(heap_, stack_, frames_, winding_stack_);
                if (!cont_res) return std::unexpected(cont_res.error());
                
                push(*cont_res);
                
                auto dispatch_res = dispatch_callee(consumer, 1, /*is_tail=*/false);
                if (!dispatch_res) return std::unexpected(dispatch_res.error());

                if (dispatch_res->action == DispatchAction::SetupFrame) {
                    setup_frame(dispatch_res->func, dispatch_res->closure, 1);
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
        unpack_to_stack(result);
        uint32_t argc = static_cast<uint32_t>(stack_.size() - fp_);

        LispVal consumer = current_frame.extra;
        auto dispatch_res = dispatch_callee(consumer, argc, /*is_tail=*/false);
        if (!dispatch_res) return std::unexpected(dispatch_res.error());

        // Restore the frame that CALLED call-with-values
        if (frames_.empty()) {
            return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::StackUnderflow, "CallWithValues caller frame missing"}});
        }
        restore_frame(frames_.back());

        if (dispatch_res->action == DispatchAction::SetupFrame) {
            setup_frame(dispatch_res->func, dispatch_res->closure, argc);
        }
        return {};
    } else if (current_frame.kind == FrameKind::DynamicWindBody) {
        // Returned from 'before' thunk. result is ignored.
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
        restore_frame(frames_.back());

        // Push a frame to call 'after' when 'body' returns
        frames_.push_back({current_func_, pc_, fp_, current_closure_, FrameKind::DynamicWindAfter, wind.after});

        if (body_dispatch->action == DispatchAction::SetupFrame) {
            setup_frame(body_dispatch->func, body_dispatch->closure, 0);
        } else {
            // Body was primitive, execute 'after' immediately
            LispVal body_result = pop();
            frames_.pop_back(); // Pop the DynamicWindAfter frame

            auto after_dispatch = dispatch_callee(wind.after, 0, /*is_tail=*/false);
            if (!after_dispatch) return std::unexpected(after_dispatch.error());

            if (after_dispatch->action == DispatchAction::SetupFrame) {
                push(body_result);
                setup_frame(after_dispatch->func, after_dispatch->closure, 1);
            } else {
                pop(); // ignore after result
                push(body_result);
                winding_stack_.pop_back();
            }
        }
        return {};
    } else if (current_frame.kind == FrameKind::DynamicWindAfter) {
        // Returned from 'after' thunk. Return body's result.
        if (winding_stack_.empty()) {
            return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::InternalError, "Winding stack empty in DynamicWindAfter return"}});
        }
        winding_stack_.pop_back();

        if (frames_.empty()) {
            push(body_result_for_after);
            return {};
        }
        restore_frame(frames_.back());
        push(body_result_for_after);
        return {};
    }

    if (frames_.empty()) {
        push(result);
        return {};
    }

    restore_frame(frames_.back());
    push(result);
    return {};
}

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
