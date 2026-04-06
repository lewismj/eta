#pragma once

#include <vector>
#include <expected>
#include <functional>
#include <memory>
#include <optional>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include "eta/runtime/nanbox.h"
#include "eta/runtime/memory/heap.h"
#include "eta/runtime/memory/intern_table.h"
#include "eta/runtime/error.h"
#include "eta/reader/lexer.h"
#include "bytecode.h"

namespace eta::runtime::memory::gc { class MarkSweepGC; }

namespace eta::runtime::vm {

using namespace eta::runtime::nanbox;
using namespace eta::runtime::memory::heap;
using namespace eta::runtime::memory::intern;
using namespace eta::runtime::error;

// ── Debug types ──────────────────────────────────────────────────────────────

/// A (file_id, 1-based line) pair identifying a source breakpoint.
struct BreakLocation {
    uint32_t file_id{0};
    uint32_t line{0};
    bool operator<(const BreakLocation& o) const noexcept {
        return file_id != o.file_id ? file_id < o.file_id : line < o.line;
    }
    bool operator==(const BreakLocation& o) const noexcept {
        return file_id == o.file_id && line == o.line;
    }
};

enum class StopReason { Breakpoint, Step, Pause, Exception };

struct StopEvent {
    StopReason           reason{StopReason::Pause};
    reader::lexer::Span  span{};         ///< source location of stopped instruction
    std::string          exception_text; ///< non-empty when reason == Exception
};

/// Callback invoked (on the VM thread, with debug_mutex_ held) when the VM stops.
/// The callback MUST NOT call resume()/step_*() directly — post to a queue instead.
using StopCallback = std::function<void(const StopEvent&)>;

/// (name, raw value) pair returned by get_locals / get_upvalues.
struct VarEntry {
    std::string         name;
    nanbox::LispVal     value{nanbox::Nil};
    bool                is_param{false};
};

/// Shallow frame description for stack-trace display.
struct FrameInfo {
    std::string          func_name;
    reader::lexer::Span  span{};
    std::size_t          frame_index{0};
};

enum class FrameKind : uint8_t {
    Normal,
    CallWithValuesConsumer,
    DynamicWindBody,
    DynamicWindAfter,
    DynamicWindCleanup,
    Sentinel,
    ContinuationJump,
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

    // ── Debug API ────────────────────────────────────────────────────────────

    /// Install a stop callback (called on VM thread when stopped).
    /// The callback must not call resume()/step_*() — it should post to a queue.
    void set_stop_callback(StopCallback cb) { stop_callback_ = std::move(cb); }

    /// Replace the current breakpoint set (thread-safe).
    void set_breakpoints(std::vector<BreakLocation> locs);

    void resume();        ///< Continue free-running.
    void step_over();     ///< Run until frame depth ≤ entry AND source line changes.
    void step_in();       ///< Run until any new source line.
    void step_out();      ///< Run until frame depth < entry depth.
    void request_pause(); ///< Async: stop at next instruction boundary.

    /// Inspect call stack — only valid while stopped.
    [[nodiscard]] std::vector<FrameInfo> get_frames()                        const;
    [[nodiscard]] std::vector<VarEntry>  get_locals(std::size_t frame_index) const;
    [[nodiscard]] std::vector<VarEntry>  get_upvalues(std::size_t frame_index) const;

    /// True when the VM is currently paused.
    [[nodiscard]] bool is_paused() const noexcept {
        std::lock_guard<std::mutex> lk(debug_mutex_);
        return is_paused_;
    }

    void collect_garbage();

    std::expected<LispVal, RuntimeError> execute(const BytecodeFunction& main);

    /**
     * @brief Re-entrant call into the VM — safe to call from within a running primitive.
     *
     * Invokes `proc` with the given `args`.  For primitive procedures the call
     * is direct (no VM re-entry).  For closures the VM runs a nested execution
     * loop, then restores the caller's execution state before returning.
     *
     * Typical use: implementing higher-order primitives such as map / for-each
     * that need to invoke user-supplied closures.
     *
     * @param proc  A LispVal that is either a Primitive or a Closure.
     * @param args  Arguments to pass to the procedure.
     * @return The procedure's return value, or a RuntimeError on failure.
     */
    std::expected<LispVal, RuntimeError> call_value(LispVal proc, std::vector<LispVal> args);

    // Test helper to access/modify globals
    std::vector<LispVal>& globals() { return globals_; }
    const std::vector<LispVal>& globals() const { return globals_; }

    // Port accessors
    LispVal current_input_port() const { return current_input_; }
    LispVal current_output_port() const { return current_output_; }
    LispVal current_error_port() const { return current_error_; }

    void set_current_input_port(LispVal port) { current_input_ = port; }
    void set_current_output_port(LispVal port) { current_output_ = port; }
    void set_current_error_port(LispVal port) { current_error_ = port; }


private:
    Heap& heap_;
    InternTable& intern_table_;
    FunctionResolver func_resolver_;
    std::vector<LispVal> stack_;
    std::vector<Frame> frames_;
    std::vector<LispVal> globals_;
    std::vector<WindFrame> winding_stack_;
    std::vector<LispVal> temp_roots_;

    // Current I/O ports
    LispVal current_input_{nanbox::Nil};
    LispVal current_output_{nanbox::Nil};
    LispVal current_error_{nanbox::Nil};

    // Current execution state (cached from top frame)
    const BytecodeFunction* current_func_{nullptr};
    uint32_t pc_{0};
    uint32_t fp_{0};
    LispVal current_closure_{0};

    std::unique_ptr<memory::gc::MarkSweepGC> gc_;

    // ── Debug state ──────────────────────────────────────────────────────────
    StopCallback                stop_callback_;         ///< set once before VM starts

    mutable std::mutex          debug_mutex_;
    std::condition_variable     debug_cv_;
    bool                        is_paused_{false};      ///< guarded by debug_mutex_

    std::mutex                  bp_mutex_;
    std::vector<BreakLocation>  breakpoints_;           ///< sorted; guarded by bp_mutex_

    enum class StepMode { None, Continue, Over, In, Out };
    StepMode    step_mode_{StepMode::None};             ///< guarded by debug_mutex_
    std::size_t step_target_depth_{0};                  ///< guarded by debug_mutex_
    uint32_t    step_origin_file_{0};                   ///< guarded by debug_mutex_
    uint32_t    step_origin_line_{0};                   ///< guarded by debug_mutex_
    uint32_t    step_epoch_{0};                         ///< armed epoch
    uint32_t    step_current_epoch_{0};                 ///< current epoch (incremented on call/cc)

    std::atomic<bool> should_pause_{false};

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

    template<ObjectKind Kind, typename T>
    const T* try_get_as(LispVal val) const {
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
        // Ensure we don't chop off arguments if stack_size is small
        // Use (std::max) to avoid collision with the max() macro on Windows
        uint32_t needed_size = (std::max)(func->stack_size, argc);
        stack_.resize(fp_ + needed_size, Nil);
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

    // Pack extra arguments (beyond required arity) into a rest-arg list
    // at stack_[fp_ + required]. Called after setup_frame / tail reuse.
    std::expected<void, RuntimeError> pack_rest_args(uint32_t argc, uint32_t required);

    // Debug helpers ──────────────────────────────────────────────────────────
    /// Called at the top of every instruction iteration when stop_callback_ is set.
    /// Returns a StopEvent if execution should pause, nullopt otherwise.
    std::optional<StopEvent> check_debug_stop();
};

} // namespace eta::runtime::vm
