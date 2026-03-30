#pragma once

#include <vector>
#include <expected>
#include <functional>
#include "eta/runtime/nanbox.h"
#include "eta/runtime/memory/heap.h"
#include "eta/runtime/memory/intern_table.h"
#include "eta/runtime/error.h"
#include "bytecode.h"

namespace eta::runtime::vm {

using namespace eta::runtime::nanbox;
using namespace eta::runtime::memory::heap;
using namespace eta::runtime::memory::intern;
using namespace eta::runtime::error;

enum class FrameKind : uint8_t {
    Normal,
    CallWithValuesConsumer,
    DynamicWindBody,
    DynamicWindAfter,
};

struct Frame {
    const BytecodeFunction* func;
    uint32_t pc;
    uint32_t fp; // index of first local in stack
    LispVal closure;
    FrameKind kind{FrameKind::Normal};
    LispVal extra{0}; // Extra state (e.g., consumer for CallWithValues)
};

struct WindFrame {
    LispVal before;
    LispVal body;
    LispVal after;
};

// Result of dispatch_callee helper
enum class DispatchAction {
    Continue,       // Callee was primitive or continuation; result already pushed
    SetupFrame,     // Caller should set up a new frame (for Call)
    TailReuse,      // Caller should reuse current frame (for TailCall)
};

struct DispatchResult {
    DispatchAction action;
    const BytecodeFunction* func{nullptr};
    LispVal closure{0};
};

// Function resolver callback type - resolves function index to BytecodeFunction pointer
using FunctionResolver = std::function<const BytecodeFunction*(uint32_t)>;

class VM {
public:
    VM(Heap& heap, InternTable& intern_table);

    // Set the function resolver for index-based function lookup
    void set_function_resolver(FunctionResolver resolver) {
        func_resolver_ = std::move(resolver);
    }

    std::expected<LispVal, RuntimeError> execute(const BytecodeFunction& main);

private:
    Heap& heap_;
    InternTable& intern_table_;
    FunctionResolver func_resolver_;
    std::vector<LispVal> stack_;
    std::vector<Frame> frames_;
    std::vector<LispVal> globals_;
    std::vector<WindFrame> winding_stack_;

    // Current execution state (cached from top frame)
    const BytecodeFunction* current_func_{nullptr};
    uint32_t pc_{0};
    uint32_t fp_{0};
    LispVal current_closure_{0};

    std::expected<void, RuntimeError> run_loop();
    void push(LispVal val) { stack_.push_back(val); }
    LispVal pop() { LispVal v = stack_.back(); stack_.pop_back(); return v; }

    void unpack_to_stack(LispVal value);

    // Unified call dispatch helper
    std::expected<DispatchResult, RuntimeError> dispatch_callee(LispVal callee, uint32_t argc, bool is_tail);

    // Type-checked heap object accessor - returns nullptr if not the expected type
    template<ObjectKind Kind, typename T>
    T* try_get_as(LispVal val) {
        if (!ops::is_boxed(val) || ops::tag(val) != Tag::HeapObject) return nullptr;
        HeapEntry entry;
        if (!heap_.try_get(ops::payload(val), entry)) return nullptr;
        if (entry.header.kind != Kind) return nullptr;
        return static_cast<T*>(entry.ptr);
    }

    DispatchResult setup_frame(const BytecodeFunction* func, LispVal closure, uint32_t argc, bool is_tail) {
        if (is_tail) {
            // In a real tail call, we'd reuse the current frame.
            // For now, we return TailReuse to indicate this.
            return {DispatchAction::TailReuse, func, closure};
        }
        
        frames_.push_back({current_func_, pc_, fp_, current_closure_});
        current_func_ = func;
        current_closure_ = closure;
        fp_ = static_cast<uint32_t>(stack_.size() - argc);
        pc_ = 0;
        
        return {DispatchAction::Continue, nullptr, 0};
    }

    // Resolve function index to BytecodeFunction pointer
    const BytecodeFunction* resolve_function(uint32_t index) {
        if (func_resolver_) return func_resolver_(index);
        return nullptr;
    }

    // Helper to create type errors
    static RuntimeError make_type_error(const char* msg) {
        return RuntimeError{VMError{RuntimeErrorCode::TypeError, msg}};
    }


    // Unified binary arithmetic dispatch helper - eliminates code duplication
    // for Add, Sub, Mul, Div opcodes
    std::expected<void, RuntimeError> do_binary_arithmetic(OpCode op);
};

} // namespace eta::runtime::vm
