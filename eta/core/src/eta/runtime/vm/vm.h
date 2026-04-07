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
#include "debug_state.h"   // DebugState, BreakLocation, StopEvent, StopReason

namespace eta::runtime::memory::gc { class MarkSweepGC; }

namespace eta::runtime::vm {

using namespace eta::runtime::nanbox;
using namespace eta::runtime::memory::heap;
using namespace eta::runtime::memory::intern;
using namespace eta::runtime::error;

// BreakLocation, StopReason, StopEvent, StopCallback, VarEntry, FrameInfo
// are now defined in debug_state.h and re-exported here.

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

/// A live exception catch frame installed by SetupCatch.
struct CatchFrame {
    LispVal                     tag;          ///< symbol to match; Nil = catch-all
    const BytecodeFunction*     func;         ///< function where SetupCatch lives
    uint32_t                    handler_pc;   ///< pc to jump to on match
    uint32_t                    fp;           ///< frame pointer to restore
    LispVal                     closure;      ///< closure to restore
    std::size_t                 frame_count;  ///< frames_.size() to restore to
    uint32_t                    stack_top;    ///< stack_.size() before pushing caught value
    std::size_t                 wind_count;   ///< winding_stack_.size() to restore to
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

    /// Install a stop callback — activates the debug session.
    /// Any breakpoints already set via set_breakpoints() are forwarded to the
    /// new DebugState so call order doesn't matter.
    void set_stop_callback(StopCallback cb) {
        debug_ = std::make_unique<DebugState>(std::move(cb));
        if (!pending_breakpoints_.empty()) {
            debug_->set_breakpoints(std::move(pending_breakpoints_));
            pending_breakpoints_.clear();
        }
    }

    /// Replace the current breakpoint set (thread-safe).
    /// May be called before or after set_stop_callback().
    void set_breakpoints(std::vector<BreakLocation> locs) {
        if (debug_) {
            debug_->set_breakpoints(std::move(locs));
        } else {
            // Buffer until set_stop_callback() is called.
            pending_breakpoints_ = std::move(locs);
        }
    }

    void resume()        { if (debug_) debug_->resume(); }
    void step_over()     {
        if (debug_) {
            auto sp = current_func_ ? current_func_->span_at(pc_ > 0 ? pc_ - 1 : 0) : reader::lexer::Span{};
            debug_->step_over(sp, frames_.size());
        }
    }
    void step_in()       {
        if (debug_) {
            auto sp = current_func_ ? current_func_->span_at(pc_ > 0 ? pc_ - 1 : 0) : reader::lexer::Span{};
            debug_->step_in(sp, frames_.size());
        }
    }
    void step_out()      { if (debug_) debug_->step_out(frames_.size()); }
    void request_pause() { if (debug_) debug_->request_pause(); }

    [[nodiscard]] bool is_paused() const noexcept {
        return debug_ && debug_->is_paused();
    }

    /// Inspect call stack — only valid while stopped.
    [[nodiscard]] std::vector<FrameInfo> get_frames()                        const;
    [[nodiscard]] std::vector<VarEntry>  get_locals(std::size_t frame_index) const;
    [[nodiscard]] std::vector<VarEntry>  get_upvalues(std::size_t frame_index) const;

    void collect_garbage();

    std::expected<LispVal, RuntimeError> execute(const BytecodeFunction& main);

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
    std::vector<CatchFrame> catch_stack_;  ///< live exception handlers

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

    // ── Debug state (null when not debugging) ────────────────────────────
    std::unique_ptr<DebugState> debug_;
    std::vector<BreakLocation>  pending_breakpoints_;  ///< queued before set_stop_callback

    std::expected<void, RuntimeError> run_loop();
    std::expected<void, RuntimeError> handle_return(LispVal result);
    void push(LispVal val) { stack_.push_back(val); }
    LispVal pop() { LispVal v = stack_.back(); stack_.pop_back(); return v; }

    void unpack_to_stack(LispVal value);

    // Unified call dispatch helper
    std::expected<DispatchResult, RuntimeError> dispatch_callee(LispVal callee, uint32_t argc, bool is_tail);

    // Type-checked heap object accessor
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
        uint32_t needed_size = (std::max)(func->stack_size, argc);
        stack_.resize(fp_ + needed_size, Nil);
    }

    void restore_frame(const Frame& f) {
        current_func_ = f.func;
        pc_ = f.pc;
        fp_ = f.fp;
        current_closure_ = f.closure;
    }

    const BytecodeFunction* resolve_function(uint32_t index) {
        if (func_resolver_) return func_resolver_(index);
        return nullptr;
    }

    static RuntimeError make_type_error(const char* msg) {
        return RuntimeError{VMError{RuntimeErrorCode::TypeError, msg}};
    }

    bool values_eqv(LispVal a, LispVal b);
    std::expected<void, RuntimeError> do_binary_arithmetic(OpCode op);
    std::expected<void, RuntimeError> pack_rest_args(uint32_t argc, uint32_t required);

    /// Execute a Throw: pop tag+value, find matching CatchFrame, unwind.
    /// Returns error if unhandled.
    std::expected<void, RuntimeError> do_throw(LispVal tag, LispVal value,
                                                reader::lexer::Span span);
};

} // namespace eta::runtime::vm
