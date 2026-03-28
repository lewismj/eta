#include "vm.h"
#include "eta/runtime/factory.h"
#include "eta/runtime/types/types.h"
#include "eta/runtime/types/primitive.h"

namespace eta::runtime::vm {

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

std::expected<void, RuntimeError> VM::run_loop() {
    while (pc_ < current_func_->code.size()) {
        const auto& instr = current_func_->code[pc_++];
        switch (instr.opcode) {
            case OpCode::Nop:
                break;
            case OpCode::StoreLocal:
                stack_[fp_ + instr.arg] = pop();
                break;
            case OpCode::Values:
            case OpCode::CallWithValues:
            case OpCode::DynamicWind:
                return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::NotImplemented, "OpCode not implemented"}});
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
                if (current_closure_ != 0) {
                    HeapEntry entry;
                    if (heap_.try_get(ops::payload(current_closure_), entry) && entry.header.kind == ObjectKind::Closure) {
                         auto* closure = static_cast<Closure*>(entry.ptr);
                         push(closure->upvals[instr.arg]);
                         break;
                    }
                }
                return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError, "LoadUpval outside of a closure"}});
            }
            case OpCode::StoreUpval: {
                if (current_closure_ != 0) {
                    HeapEntry entry;
                    if (heap_.try_get(ops::payload(current_closure_), entry) && entry.header.kind == ObjectKind::Closure) {
                         auto* closure = static_cast<Closure*>(entry.ptr);
                         closure->upvals[instr.arg] = pop();
                         break;
                    }
                }
                return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError, "StoreUpval outside of a closure"}});
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
                // For now, handle BytecodeFunction (unwrapped) or Closure
                if (ops::is_boxed(callee) && ops::tag(callee) == Tag::HeapObject) {
                    HeapEntry entry;
                    if (heap_.try_get(ops::payload(callee), entry)) {
                        if (entry.header.kind == ObjectKind::Closure) {
                            auto* closure = static_cast<Closure*>(entry.ptr);
                            // Setup new frame
                            frames_.push_back({current_func_, pc_, fp_, current_closure_});
                            current_func_ = closure->func;
                            current_closure_ = callee;
                            fp_ = static_cast<uint32_t>(stack_.size() - argc);
                            pc_ = 0;
                            // Arity check
                            if (argc != current_func_->arity) {
                                return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::InvalidArity, "Wrong number of arguments"}});
                            }
                            break;
                        } else if (entry.header.kind == ObjectKind::Continuation) {
                            if (argc != 1) {
                                return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::InvalidArity, "Continuation expects 1 argument"}});
                            }
                            LispVal v = pop(); // The argument
                            auto* cont = static_cast<Continuation*>(entry.ptr);
                            
                            // Restore state
                            stack_ = cont->stack;
                            frames_ = cont->frames;
                            
                            // Restore current frame state
                            const auto& f = frames_.back();
                            current_func_ = f.func;
                            pc_ = f.pc;
                            fp_ = f.fp;
                            current_closure_ = f.closure;
                            
                            // Return v from CallCC
                            push(v);
                            break;
                        } else if (entry.header.kind == ObjectKind::Primitive) {
                            auto* prim = static_cast<Primitive*>(entry.ptr);
                            std::vector<LispVal> args;
                            for (uint32_t i = 0; i < argc; ++i) {
                                args.push_back(stack_[stack_.size() - argc + i]);
                            }
                            stack_.resize(stack_.size() - argc);
                            
                            auto res = prim->func(args);
                            if (!res) return std::unexpected(res.error());
                            push(*res);
                            break;
                        }
                    }
                }
                return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError, "Callee is not a function"}});
            }
            case OpCode::MakeClosure: {
                // arg is index in constants table for the BytecodeFunction (as a raw pointer? no, we need a better way)
                // Actually, let's assume arg is const_idx, and next instruction or arg2 is num_upvals.
                // Wait, Instruction only has one arg. 
                // Let's use arg as (const_idx << 16) | num_upvals
                uint32_t const_idx = instr.arg >> 16;
                uint32_t num_upvals = instr.arg & 0xFFFF;
                
                LispVal func_val = current_func_->constants[const_idx];
                // How do we store BytecodeFunction in constants? 
                // Maybe as a raw pointer wrapped in a LispVal (unsafe) or on the heap.
                // Let's assume it's on the heap as ObjectKind::Lambda.
                
                std::vector<LispVal> upvals;
                for (uint32_t i = 0; i < num_upvals; ++i) {
                    upvals.push_back(pop());
                }
                std::reverse(upvals.begin(), upvals.end());
                
                // Get BytecodeFunction from func_val
                HeapEntry entry;
                if (heap_.try_get(ops::payload(func_val), entry) && entry.header.kind == ObjectKind::Lambda) {
                    auto* lambda = static_cast<Lambda*>(entry.ptr);
                    // Lambda currently has std::vector<LispVal> formals, LispVal body, etc.
                    // This is not exactly BytecodeFunction.
                    // Let's assume we store BytecodeFunction* in Lambda::body for now.
                    auto* bfunc = reinterpret_cast<const BytecodeFunction*>(lambda->body);
                    auto closure_res = make_closure(heap_, bfunc, std::move(upvals));
                    if (!closure_res) return std::unexpected(closure_res.error());
                    push(*closure_res);
                } else {
                    return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError, "Invalid lambda for closure"}});
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
                LispVal v = pop();
                if (ops::is_boxed(v) && ops::tag(v) == Tag::HeapObject) {
                    HeapEntry entry;
                    if (heap_.try_get(ops::payload(v), entry) && entry.header.kind == ObjectKind::Cons) {
                        push(static_cast<Cons*>(entry.ptr)->car);
                        break;
                    }
                }
                return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError, "Not a pair"}});
            }
            case OpCode::Add:
            case OpCode::Sub:
            case OpCode::Mul:
            case OpCode::Div: {
                LispVal b = pop();
                LispVal a = pop();
                if (ops::tag(a) == Tag::Fixnum && ops::tag(b) == Tag::Fixnum) {
                    int64_t va = ops::decode<int64_t>(a).value();
                    int64_t vb = ops::decode<int64_t>(b).value();
                    int64_t res_val;
                    if (instr.opcode == OpCode::Add) res_val = va + vb;
                    else if (instr.opcode == OpCode::Sub) res_val = va - vb;
                    else if (instr.opcode == OpCode::Mul) res_val = va * vb;
                    else if (instr.opcode == OpCode::Div) {
                        if (vb == 0) return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError, "Division by zero"}});
                        res_val = va / vb;
                    } else {
                        res_val = 0;
                    }
                    auto res = make_fixnum(heap_, res_val);
                    if (!res) return std::unexpected(res.error());
                    push(*res);
                } else {
                     return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError, "Arithmetic on non-numbers"}});
                }
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
                if (ops::is_boxed(v) && ops::tag(v) == Tag::HeapObject) {
                    HeapEntry entry;
                    if (heap_.try_get(ops::payload(v), entry) && entry.header.kind == ObjectKind::Cons) {
                        push(static_cast<Cons*>(entry.ptr)->cdr);
                        break;
                    }
                }
                return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError, "Not a pair"}});
            }
            case OpCode::TailCall: {
                uint32_t argc = instr.arg;
                LispVal callee = pop();
                if (ops::is_boxed(callee) && ops::tag(callee) == Tag::HeapObject) {
                    HeapEntry entry;
                    if (heap_.try_get(ops::payload(callee), entry) && entry.header.kind == ObjectKind::Closure) {
                        auto* closure = static_cast<Closure*>(entry.ptr);
                        // Tail call: reuse current frame
                        current_func_ = closure->func;
                        current_closure_ = callee;
                        
                        // Move new arguments to current fp_
                        uint32_t new_args_start = static_cast<uint32_t>(stack_.size() - argc);
                        for (uint32_t i = 0; i < argc; ++i) {
                            stack_[fp_ + i] = stack_[new_args_start + i];
                        }
                        stack_.resize(fp_ + argc);
                        pc_ = 0;
                        
                        if (argc != current_func_->arity) {
                            return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::InvalidArity, "Wrong number of arguments"}});
                        }
                        break;
                    } else if (heap_.try_get(ops::payload(callee), entry) && entry.header.kind == ObjectKind::Continuation) {
                         if (argc != 1) {
                            return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::InvalidArity, "Continuation expects 1 argument"}});
                        }
                        LispVal v = pop(); // The argument
                        auto* cont = static_cast<Continuation*>(entry.ptr);
                        
                        // Restore state
                        stack_ = cont->stack;
                        frames_ = cont->frames;
                        
                        // Restore current frame state
                        const auto& f = frames_.back();
                        current_func_ = f.func;
                        pc_ = f.pc;
                        fp_ = f.fp;
                        current_closure_ = f.closure;
                        
                        // Return v from CallCC
                        push(v);
                        break;
                    } else if (heap_.try_get(ops::payload(callee), entry) && entry.header.kind == ObjectKind::Primitive) {
                        auto* prim = static_cast<Primitive*>(entry.ptr);
                        std::vector<LispVal> args;
                        for (uint32_t i = 0; i < argc; ++i) {
                            args.push_back(stack_[stack_.size() - argc + i]);
                        }
                        stack_.resize(stack_.size() - argc);
                        
                        auto res = prim->func(args);
                        if (!res) return std::unexpected(res.error());
                        push(*res);
                        break;
                    }
                }
                return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError, "Callee is not a function"}});
            }
            case OpCode::CallCC: {
                LispVal consumer = pop();
                
                // Capture current state
                auto cont_res = make_continuation(heap_, stack_, frames_);
                if (!cont_res) return std::unexpected(cont_res.error());
                
                // Push consumer and then the continuation, then call it
                push(consumer);
                push(*cont_res);
                
                // Now we need to call consumer with 1 argument.
                // We can reuse the Call logic.
                // Since we are already in run_loop, we can just set up the new frame.
                // ...
                // To avoid duplication, I'll extract Call logic or just repeat it for now.
                LispVal callee = consumer;
                uint32_t argc = 1;
                if (ops::is_boxed(callee) && ops::tag(callee) == Tag::HeapObject) {
                    HeapEntry entry;
                    if (heap_.try_get(ops::payload(callee), entry) && entry.header.kind == ObjectKind::Closure) {
                        auto* closure = static_cast<Closure*>(entry.ptr);
                        frames_.push_back({current_func_, pc_, fp_, current_closure_});
                        current_func_ = closure->func;
                        current_closure_ = callee;
                        fp_ = static_cast<uint32_t>(stack_.size() - argc);
                        pc_ = 0;
                        if (argc != current_func_->arity) {
                            return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::InvalidArity, "Wrong number of arguments"}});
                        }
                        break;
                    }
                }
                return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError, "CallCC consumer is not a function"}});
            }
            case OpCode::Return: {
                LispVal result = pop();
                // Pop current frame
                frames_.pop_back();
                
                // Cleanup stack: pop locals and arguments of returned function
                stack_.resize(fp_);
                
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

} // namespace eta::runtime::vm
