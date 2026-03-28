#pragma once

#include <cstdint>
#include <vector>
#include <string>
#include "eta/runtime/nanbox.h"

namespace eta::runtime::vm {

enum class OpCode : std::uint8_t {
    // Basic operations
    Nop,
    LoadConst,   // [const_idx] -> push(consts[const_idx])
    LoadLocal,   // [slot] -> push(stack[fp + slot])
    StoreLocal,  // [slot] -> stack[fp + slot] = pop()
    LoadUpval,   // [index] -> push(closure->upvals[index])
    StoreUpval,  // [index] -> closure->upvals[index] = pop()
    LoadGlobal,  // [global_idx] -> push(globals[global_idx])
    StoreGlobal, // [global_idx] -> globals[global_idx] = pop()

    // Stack management
    Pop,
    Dup,

    // Control flow
    Jump,        // [offset] -> pc += offset
    JumpIfFalse, // [offset] -> if (pop() == #f) pc += offset
    Call,        // [argc] -> call(pop(), argc)
    TailCall,    // [argc] -> tail_call(pop(), argc)
    Return,      // return pop()

    // Closures
    MakeClosure, // [const_idx, num_upvals] -> push(make_closure(consts[const_idx], pop_n(num_upvals)))

    // Primitives (Specialized instructions for speed)
    Cons,
    Car,
    Cdr,
    Add,
    Sub,
    Mul,
    Div,
    Eq,
    
    // Values
    Values,      // [n] -> return n values from stack
    CallWithValues,

    // Control flow specialized
    DynamicWind,
    CallCC,
};

struct Instruction {
    OpCode opcode;
    std::uint32_t arg;
};

struct BytecodeFunction {
    std::vector<Instruction> code;
    std::vector<nanbox::LispVal> constants;
    std::uint32_t arity;
    bool has_rest;
    std::uint32_t stack_size;
    std::string name;
};

} // namespace eta::runtime::vm
