#pragma once

#include <vector>
#include <expected>
#include <functional>
#include <memory>
#include "eta/runtime/nanbox.h"
#include "eta/runtime/memory/heap.h"
#include "eta/runtime/memory/intern_table.h"
#include "eta/runtime/error.h"
#include "bytecode.h"

namespace eta::runtime::memory::gc { class MarkSweepGC; }

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
    DynamicWindCleanup,
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
    ~VM();

    // Set the function resolver for index-based function lookup
    void set_function_resolver(FunctionResolver resolver) {
        func_resolver_ = std::move(resolver);
    }

    void collect_garbage();

    std::expected<LispVal, RuntimeError> execute(const BytecodeFunction& main);
    
    // Test helper to access/modify globals
    std::vector<LispVal>& globals() { return globals_; }
    const std::vector<LispVal>& globals() const { return globals_; }

private:
    Heap& heap_;
    InternTable& intern_table_;
    FunctionResolver func_resolver_;
    std::vector<LispVal> stack_;
    std::vector<Frame> frames_;
    std::vector<LispVal> globals_;
    std::vector<WindFrame> winding_stack_;
    std::vector<LispVal> temp_roots_;

    // Current execution state (cached from top frame)
    const BytecodeFunction* current_func_{nullptr};
    uint32_t pc_{0};
    uint32_t fp_{0};
    LispVal current_closure_{0};

    std::unique_ptr<memory::gc::MarkSweepGC> gc_;

    std::expected<void, RuntimeError> run_loop();
    std::expected<void, RuntimeError> handle_return(LispVal result);
    void push(LispVal val) { stack_.push_back(val); }
    LispVal pop() { LispVal v = stack_.back(); stack_.pop_back(); return v; }

    void unpack_to_stack(LispVal value);

    // Unified call dispatch helper
    std::expected<DispatchResult, RuntimeError> dispatch_callee(LispVal callee, uint32_t argc, bool is_tail);

    // Type-checked heap object accessor - returns nullptr if not the expected type
    // Delegates to Heap::try_get_as after extracting the ObjectId from LispVal
    template<ObjectKind Kind, typename T>
    T* try_get_as(LispVal val) {
        if (!ops::is_boxed(val) || ops::tag(val) != Tag::HeapObject) return nullptr;
        return heap_.try_get_as<Kind, T>(ops::payload(val));
    }

    // Helper to get typed heap object or return a TypeError
    template<ObjectKind Kind, typename T>
    std::expected<T*, RuntimeError> get_as_or_error(LispVal val, const char* msg) {
        if (auto* ptr = try_get_as<Kind, T>(val)) return ptr;
        return std::unexpected(make_type_error(msg));
    }

    void setup_frame(const BytecodeFunction* func, LispVal closure, uint32_t argc, FrameKind kind = FrameKind::Normal, LispVal extra = 0) {
        frames_.push_back({current_func_, pc_, fp_, current_closure_, kind, extra});
        current_func_ = func;
        current_closure_ = closure;
        fp_ = static_cast<uint32_t>(stack_.size() - argc);
        pc_ = 0;
        stack_.resize(fp_ + func->stack_size, Nil);
    }

    // Restore execution state from a frame (used after function returns)
    void restore_frame(const Frame& f) {
        current_func_ = f.func;
        pc_ = f.pc;
        fp_ = f.fp;
        current_closure_ = f.closure;
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

    // String-aware value equality (for eqv?/equal? semantics)
    // Returns true if values are equal, considering string content for strings
    bool values_eqv(LispVal a, LispVal b);

    // Unified binary arithmetic dispatch helper - eliminates code duplication
    // for Add, Sub, Mul, Div opcodes
    std::expected<void, RuntimeError> do_binary_arithmetic(OpCode op);
};

} // namespace eta::runtime::vm
