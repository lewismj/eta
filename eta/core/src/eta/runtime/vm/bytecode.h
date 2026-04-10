#pragma once

#include <cstdint>
#include <ostream>
#include <vector>
#include <string>
#include "eta/runtime/nanbox.h"
#include "eta/reader/lexer.h"

namespace eta::runtime::vm {

// Tag used to distinguish function indices from raw pointers in constants.
// When encoding a function index in MakeClosure, set this bit to mark it as an index.
constexpr uint64_t FUNC_INDEX_TAG = 1ULL << 63;

// Mask for bits 62-32 — these are always zero in a valid func_index
// (encode_func_index stores only a uint32_t in the lower 32 bits).
// Negative IEEE-754 doubles also have bit 63 set but their exponent
// field (bits 62-52) is non-zero, so this mask distinguishes them.
constexpr uint64_t FUNC_INDEX_UPPER_ZERO_MASK = 0x7FFFFFFF00000000ULL;

inline nanbox::LispVal encode_func_index(uint32_t index) {
    return FUNC_INDEX_TAG | static_cast<uint64_t>(index);
}

inline bool is_func_index(nanbox::LispVal v) {
    return (v & FUNC_INDEX_TAG) != 0 && (v & FUNC_INDEX_UPPER_ZERO_MASK) == 0;
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

    // Unification / logic variables
    MakeLogicVar,   // [] -> push fresh unbound LogicVar
    Unify,          // [a b ->] pop b, pop a; unify(a,b); push #t or #f
    DerefLogicVar,  // [lvar ->] pop lvar; push fully dereferenced value
    TrailMark,      // [] -> push current trail size as fixnum (backtrack point)
    UnwindTrail,    // [mark ->] pop mark fixnum; undo all bindings since mark

    // Reserved slots — keep enum values stable for serialised bytecode
    CopyTerm,       // [term ->] pop term; push deep copy with fresh logic vars
    _Reserved1,     // was DualVal  (removed)
    _Reserved2,     // was DualBp   (removed)
};

/// Human-readable mnemonic for an OpCode (e.g. "LoadConst").
constexpr const char* to_string(OpCode op) noexcept {
    using enum OpCode;
    switch (op) {
        case Nop:               return "Nop";
        case LoadConst:         return "LoadConst";
        case LoadLocal:         return "LoadLocal";
        case StoreLocal:        return "StoreLocal";
        case LoadUpval:         return "LoadUpval";
        case StoreUpval:        return "StoreUpval";
        case LoadGlobal:        return "LoadGlobal";
        case StoreGlobal:       return "StoreGlobal";
        case Pop:               return "Pop";
        case Dup:               return "Dup";
        case Jump:              return "Jump";
        case JumpIfFalse:       return "JumpIfFalse";
        case Call:              return "Call";
        case TailCall:          return "TailCall";
        case Return:            return "Return";
        case MakeClosure:       return "MakeClosure";
        case Cons:              return "Cons";
        case Car:               return "Car";
        case Cdr:               return "Cdr";
        case Add:               return "Add";
        case Sub:               return "Sub";
        case Mul:               return "Mul";
        case Div:               return "Div";
        case Eq:                return "Eq";
        case Values:            return "Values";
        case CallWithValues:    return "CallWithValues";
        case DynamicWind:       return "DynamicWind";
        case CallCC:            return "CallCC";
        case PatchClosureUpval: return "PatchClosureUpval";
        case Apply:             return "Apply";
        case TailApply:         return "TailApply";
        case SetupCatch:        return "SetupCatch";
        case PopCatch:          return "PopCatch";
        case Throw:             return "Throw";
        case MakeLogicVar:      return "MakeLogicVar";
        case Unify:             return "Unify";
        case DerefLogicVar:     return "DerefLogicVar";
        case TrailMark:         return "TrailMark";
        case UnwindTrail:       return "UnwindTrail";
        case CopyTerm:          return "CopyTerm";
        case _Reserved1:        return "_Reserved1";
        case _Reserved2:        return "_Reserved2";
    }
    return "Unknown";
}

inline std::ostream& operator<<(std::ostream& os, OpCode op) {
    return os << to_string(op);
}

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

    /// Adjust every function-index constant by @p offset.
    /// Used to rebase indices to file-relative (0-based) before serialization
    /// (offset = -base_func_idx) and to relocate them to the runner's registry
    /// after deserialization (offset = +runner_base_idx).
    void rebase_func_indices(int32_t offset) {
        for (auto& c : constants) {
            if (is_func_index(c)) {
                auto old_idx = static_cast<int32_t>(decode_func_index(c));
                c = encode_func_index(static_cast<uint32_t>(old_idx + offset));
            }
        }
    }

    /// Return the source span for the instruction at index @p pc.
    /// Returns a zeroed Span if @p pc is out of range or the source_map
    /// was not populated (synthetic instructions have file_id == 0).
    [[nodiscard]] reader::lexer::Span span_at(std::uint32_t pc) const noexcept {
        if (pc < source_map.size()) return source_map[pc];
        return {};
    }
};

} // namespace eta::runtime::vm
