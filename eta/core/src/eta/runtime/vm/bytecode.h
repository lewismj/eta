#pragma once

#include <cstdint>
#include <vector>
#include <string>
#include "eta/runtime/nanbox.h"
#include "eta/reader/lexer.h"

namespace eta::runtime::vm {

// Tag used to distinguish function indices from raw pointers in constants.
// When encoding a function index in MakeClosure, set this bit to mark it as an index.
constexpr uint64_t FUNC_INDEX_TAG = 1ULL << 63;

inline nanbox::LispVal encode_func_index(uint32_t index) {
    return FUNC_INDEX_TAG | static_cast<uint64_t>(index);
}

inline bool is_func_index(nanbox::LispVal v) {
    return (v & FUNC_INDEX_TAG) != 0;
}

inline uint32_t decode_func_index(nanbox::LispVal v) {
    return static_cast<uint32_t>(v & ~FUNC_INDEX_TAG);
}

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

    // Closure fixup (for letrec self-reference)
    PatchClosureUpval,  // [upval_idx] -> pops value, pops closure, patches closure->upvals[upval_idx] = value

    // Apply: unpack last arg (list) and call procedure
    Apply,      // [argc] -> apply(pop(), argc) — last arg is unpacked list
    TailApply,  // [argc] -> tail_apply(pop(), argc) — tail-position apply

    // Exception handling
    // SetupCatch: arg = (tag_const_idx << 16) | pc_offset_to_handler
    //   Pushes a CatchFrame; handler_pc = pc_after_this_instr + (arg & 0xFFFF)
    //   constants[arg >> 16] is the tag symbol (Nil = catch-all)
    SetupCatch,  // [tag_const_idx:16 | offset:16]
    PopCatch,    // [] -> pop top catch frame (normal exit from protected body)
    Throw,       // [] -> pop value, pop tag; find matching catch frame or RuntimeError
};

struct Instruction {
    OpCode opcode;
    std::uint32_t arg;
};

struct BytecodeFunction {
    std::vector<Instruction>          code;
    std::vector<reader::lexer::Span>  source_map;  // parallel to code; same length
    std::vector<nanbox::LispVal>      constants;
    std::uint32_t arity{0};
    bool has_rest{false};
    std::uint32_t stack_size{0};
    std::string name;

    /// Slot-indexed parameter/local names (populated by Emitter).
    /// local_names[slot] is the Scheme identifier for that stack slot.
    /// Slots without a name are empty strings (fall back to "%N" in debugger).
    std::vector<std::string> local_names;

    /// Upvalue names parallel to closure->upvals (populated by Emitter).
    std::vector<std::string> upval_names;

    /// Return the source span for the instruction at index @p pc.
    /// Returns a zeroed Span if @p pc is out of range or the source_map
    /// was not populated (synthetic instructions have file_id == 0).
    [[nodiscard]] reader::lexer::Span span_at(std::uint32_t pc) const noexcept {
        if (pc < source_map.size()) return source_map[pc];
        return {};
    }
};

} // namespace eta::runtime::vm
