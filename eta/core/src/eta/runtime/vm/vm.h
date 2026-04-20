#pragma once

#include <vector>
#include <deque>
#include <expected>
#include <functional>
#include <memory>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include "eta/runtime/nanbox.h"
#include "eta/runtime/memory/heap.h"
#include "eta/runtime/memory/intern_table.h"
#include "eta/runtime/error.h"
#include "eta/reader/lexer.h"
#include "eta/runtime/clp/constraint_store.h"
#include "eta/runtime/clp/real_store.h"
#include "bytecode.h"
#include "debug_state.h"   ///< DebugState, BreakLocation, StopEvent, StopReason

namespace eta::runtime::memory::gc { class MarkSweepGC; }

namespace eta::runtime::vm {

using namespace eta::runtime::nanbox;
using namespace eta::runtime::memory::heap;
using namespace eta::runtime::memory::intern;
using namespace eta::runtime::error;

/**
 * BreakLocation, StopReason, StopEvent, StopCallback, VarEntry, FrameInfo
 * are now defined in debug_state.h and re-exported here.
 */

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

/// GC root category with the heap object IDs reachable from it.
struct GCRootInfo {
    std::string name;
    std::vector<memory::heap::ObjectId> object_ids;
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
    uint32_t fp; ///< index of first local in stack
    LispVal closure;
    FrameKind kind{FrameKind::Normal};
    LispVal extra{0}; ///< Extra state (e.g., consumer for CallWithValues)
};

struct WindFrame {
    LispVal before;
    LispVal body;
    LispVal after;
};

/**
 * A single undoable mutation recorded by the unification / CLP machinery.
 *
 * The per-VM trail was generalised from a raw `std::vector<LispVal>`
 * (binding-only) to a tagged entry.  The `Attr` case records the InternId of
 * the module key, the previous value, and a `had_prev` flag so unwind can
 * distinguish "attribute previously unset" (erase) from "previously held
 * a value, possibly Nil" (reinstate prev_value).
 *
 * Domain changes are now trailed in the same stack via
 * `Kind::Domain` entries (carrying an `std::optional<clp::Domain>` snapshot of
 * the prior state).  This makes the binding trail the single source of truth
 *
 * `Kind::RealStore` snapshots the append-only CLP(R)
 * real-constraint log length.  Unwinding to this entry truncates the VM's
 * `RealStore` back to the prior length.
 *
 * `Kind::SimplexBound` snapshots one variable's asserted
 * simplex lower/upper bounds (`std::optional<clp::Bound>` pair). Unwinding
 * restores the prior pair without snapshotting the full tableau.
 */
struct TrailEntry {
    enum class Kind : std::uint8_t {
        Bind,    ///< LogicVar `var` was bound; on undo, reset binding to nullopt
        Attr,    ///< Attributed var: `var`'s attribute `module_key` was mutated
        Domain,  ///< CLP domain on the LogicVar `var` was installed/replaced/erased
        RealStore, ///< CLP(R) real-constraint log size snapshot
        SimplexBound, ///< CLP(R) simplex bound snapshot for one logic var id
    };
    Kind                              kind{Kind::Bind};
    LispVal                           var{nanbox::Nil};        ///< HeapObject ref to the LogicVar
    LispVal                           prev_value{nanbox::Nil}; ///< Attr: previous attr value (only if had_prev)
    memory::intern::InternId          module_key{0};           ///< Attr: which attribute slot
    bool                              had_prev{false};         ///< Attr/Domain/SimplexBound: was prev_* meaningful?
    std::optional<clp::Domain>        prev_domain{};
    std::size_t                       prev_real_store_size{0};
    std::optional<clp::Bound>         prev_simplex_lo{};
    std::optional<clp::Bound>         prev_simplex_hi{};
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
    std::size_t                 tape_count;   ///< active_tapes_.size() to restore to
};

/// Result of dispatch_callee helper
enum class DispatchAction {
    Continue,       ///< Callee was primitive or continuation; result already pushed
    SetupFrame,     ///< Caller should set up a new frame (for Call)
    TailReuse,      ///< Caller should reuse current frame (for TailCall)
    NonLocalTransfer, ///< Control transferred to a catch handler; caller must not post-process result
};

struct DispatchResult {
    DispatchAction action;
    const BytecodeFunction* func{nullptr};
    LispVal closure{0};
};

/// Function resolver callback type - resolves function index to BytecodeFunction pointer
using FunctionResolver = std::function<const BytecodeFunction*(uint32_t)>;

class VM {
public:
    VM(Heap& heap, InternTable& intern_table);
    ~VM();

    /// Set the function resolver for index-based function lookup
    void set_function_resolver(FunctionResolver resolver) {
        func_resolver_ = std::move(resolver);
    }

    /// Debug API

    /**
     * Any breakpoints already set via set_breakpoints() are forwarded to the
     * new DebugState so call order doesn't matter.
     */
    void set_stop_callback(StopCallback cb) {
        debug_ = std::make_unique<DebugState>(std::move(cb));
        if (!pending_breakpoints_.empty()) {
            debug_->set_breakpoints(std::move(pending_breakpoints_));
            pending_breakpoints_.clear();
        }
    }

    /**
     * Replace the current breakpoint set (thread-safe).
     * May be called before or after set_stop_callback().
     */
    void set_breakpoints(std::vector<BreakLocation> locs) {
        if (debug_) {
            debug_->set_breakpoints(std::move(locs));
        } else {
            /// Buffer until set_stop_callback() is called.
            pending_breakpoints_ = std::move(locs);
        }
    }

    void resume()        { if (debug_) debug_->resume(); }
    void step_over()     {
        if (debug_) {
            /**
             * Use the exact span saved when the VM stopped, not pc_ - 1
             * (which is wrong because the debug hook fires pre-increment).
             */
            auto sp = debug_->stopped_span();
            debug_->step_over(sp, frames_.size());
        }
    }
    void step_in()       {
        if (debug_) {
            auto sp = debug_->stopped_span();
            debug_->step_in(sp, frames_.size());
        }
    }
    void step_out()      { if (debug_) debug_->step_out(frames_.size()); }
    void request_pause() { if (debug_) debug_->request_pause(); }

    [[nodiscard]] bool is_paused() const noexcept {
        return debug_ && debug_->is_paused();
    }

    [[nodiscard]] std::vector<FrameInfo> get_frames()                        const;
    [[nodiscard]] std::vector<VarEntry>  get_locals(std::size_t frame_index) const;
    [[nodiscard]] std::vector<VarEntry>  get_upvalues(std::size_t frame_index) const;

    [[nodiscard]] std::vector<GCRootInfo> enumerate_gc_roots() const;

    void collect_garbage();

    /**
     * @brief Drain every pending finalizer immediately.
     *
     * Test-only deterministic hook used by VM/GC unit tests.
     */
    void drain_finalizers_for_test();

    /**
     * @brief Force one GC cycle and then drain pending finalizers.
     *
     * Test-only deterministic hook used when tests need predictable
     * finalizer delivery in a single call.
     */
    void collect_garbage_and_drain_finalizers_for_test();

    std::expected<LispVal, RuntimeError> execute(const BytecodeFunction& main);

    std::expected<LispVal, RuntimeError> call_value(LispVal proc, std::vector<LispVal> args);

    /// operand is a TapeRef, enabling tape-based reverse-mode AD.
    std::expected<LispVal, RuntimeError> tape_binary_op(OpCode op, LispVal a, LispVal b);

    /// Test helper to access/modify globals
    std::vector<LispVal>& globals() { return globals_; }
    const std::vector<LispVal>& globals() const { return globals_; }

    /// CLP constraint store (exposed to core_primitives builtins)
    clp::ConstraintStore& constraint_store() { return constraint_store_; }
    const clp::ConstraintStore& constraint_store() const { return constraint_store_; }

    /// CLP(R) real-constraint store.
    clp::RealStore& real_store() { return real_store_; }
    const clp::RealStore& real_store() const { return real_store_; }

    /**
     * Trailed domain write: snapshot any prior domain on `id` onto the
     * unified trail and install `dom` via the constraint store.  An
     * UnwindTrail past this entry restores (or removes) the prior domain.
     */
    void trail_set_domain(memory::heap::ObjectId id, clp::Domain dom);

    /// Trailed domain erase: snapshot the prior domain (if any) and drop it.
    void trail_erase_domain(memory::heap::ObjectId id);

    /// Trailed real-store mark: snapshot current append-log length.
    void trail_mark_real_store();

    /**
     * Trailed simplex-bound write: snapshot prior `(lo, hi)` for `id` and
     * install the new pair in `RealStore`.
     */
    void trail_assert_simplex_bound(memory::heap::ObjectId id,
                                    std::optional<clp::Bound> lo,
                                    std::optional<clp::Bound> hi);

    /**
     * Roll back every trail entry with index >= `mark`.
     * Restores Bind/Attr/Domain/RealStore/SimplexBound side effects atomically.
     */
    void rollback_trail_to(std::size_t mark);

    /// Expose trail + hook registry to builtins.
    std::vector<TrailEntry>& trail_stack() { return trail_stack_; }
    const std::vector<TrailEntry>& trail_stack() const { return trail_stack_; }

    std::unordered_map<memory::intern::InternId, LispVal>& attr_unify_hooks() { return attr_unify_hooks_; }
    const std::unordered_map<memory::intern::InternId, LispVal>& attr_unify_hooks() const { return attr_unify_hooks_; }

    /**
     * Propagation queue.  When a logic var with one of these
     * attribute keys is bound, the attribute's value is treated as a list of
     * re-propagator thunks; each thunk is enqueued for the outer-`unify` drain
     * rather than invoked synchronously.  Provides idempotent FIFO firing,
     * which generalises the synchronous hook path used by
     * `freeze` / `dif` (which remain sync via `attr_unify_hooks_`).
     */
    std::unordered_set<memory::intern::InternId>& async_thunk_attrs() { return async_thunk_attrs_; }
    const std::unordered_set<memory::intern::InternId>& async_thunk_attrs() const { return async_thunk_attrs_; }

    /**
     * Push a propagator thunk onto the back of the queue.  Idempotent on
     * GC-rooted via the queue itself.
     */
    void enqueue_propagator(LispVal thunk);

    [[nodiscard]] std::size_t prop_queue_size() const noexcept { return prop_queue_.size(); }

    /**
     * ---- Occurs-check policy ----
     * Controls VM::unify behaviour when a binding would create a cyclic term:
     *            produce cyclic terms that break subsequent traversal).
     *            user sees the offending unification instead of silent failure.
     */
    enum class OccursCheckMode : uint8_t { Always, Never, Error };

    [[nodiscard]] OccursCheckMode occurs_check_mode() const noexcept { return occurs_check_mode_; }
    void set_occurs_check_mode(OccursCheckMode m) noexcept { occurs_check_mode_ = m; }

    /**
     * True iff the most recent unify() failed specifically because of an
     * occurs-check violation while the policy was Error. Primitive code
     * reads this to turn the failure into a runtime error.
     */
    [[nodiscard]] bool last_unify_cycle_error() const noexcept { return last_unify_cycle_error_; }
    void clear_last_unify_cycle_error() noexcept { last_unify_cycle_error_ = false; }

    /**
     * AD Tape state
     * Return the currently active tape (NaN-boxed HeapObject), or Nil if none.
     */
    [[nodiscard]] LispVal active_tape() const noexcept {
        return active_tapes_.empty() ? nanbox::Nil : active_tapes_.back();
    }
    /// Push a tape onto the active-tape stack (enables nesting).
    void push_active_tape(LispVal tape) { active_tapes_.push_back(tape); }
    /// Pop the most recent active tape.  No-op if the stack is empty.
    void pop_active_tape() { if (!active_tapes_.empty()) active_tapes_.pop_back(); }
    /// Current depth of the active-tape stack (used by CatchFrame for unwind).
    [[nodiscard]] std::size_t active_tape_count() const noexcept { return active_tapes_.size(); }


    /// Port accessors
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
    std::vector<TrailEntry> trail_stack_;  ///< logic-var / CLP attribute trail for backtracking
    clp::ConstraintStore constraint_store_; ///< CLP domain store (trailed alongside bindings)
    clp::RealStore real_store_;             ///< CLP(R) posted constraint log
    std::vector<LispVal> active_tapes_;    ///< Stack of active AD tapes (supports nesting)
    std::vector<LispVal> wam_x_regs_;      ///< WAM X argument/temp registers
    std::vector<LispVal> wam_y_regs_;      ///< WAM Y environment registers

    /**
     * Attributed-variable unify hooks, keyed by module symbol (InternId).
     * Value is a callable LispVal (closure or primitive) invoked as
     */
    std::unordered_map<memory::intern::InternId, LispVal> attr_unify_hooks_;

    /**
     * Keys whose attribute value is a list of re-propagator
     * thunks to be enqueued (rather than passed to a sync hook proc) when
     * a participating logic var becomes bound.  Populated via the new
     * `register-prop-attr!` builtin.
     */
    std::unordered_set<memory::intern::InternId> async_thunk_attrs_;

    /// Outer-unify FIFO of pending propagator thunks.
    std::deque<LispVal>                            prop_queue_;
    std::unordered_set<memory::heap::ObjectId>     prop_queued_set_;
    int                                            unify_depth_{0};

    OccursCheckMode occurs_check_mode_{OccursCheckMode::Always};
    bool            last_unify_cycle_error_{false};

    /// Current I/O ports
    LispVal current_input_{nanbox::Nil};
    LispVal current_output_{nanbox::Nil};
    LispVal current_error_{nanbox::Nil};

    /// Current execution state (cached from top frame)
    const BytecodeFunction* current_func_{nullptr};
    uint32_t pc_{0};
    uint32_t fp_{0};
    LispVal current_closure_{0};

    std::unique_ptr<memory::gc::MarkSweepGC> gc_;
    std::size_t gc_collections_{0};  ///< DEBUG: count GC runs

    /// Max pending finalizers to execute in one VM safe-point drain pass.
    static constexpr std::size_t kDefaultFinalizerBudget = 64;
    bool processing_finalizers_{false};

    /// Debug state (null when not debugging)
    std::unique_ptr<DebugState> debug_;
    std::vector<BreakLocation>  pending_breakpoints_;  ///< queued before set_stop_callback

    /**
     * Drain pending heap finalizers at VM-safe points.
     * Finalizer failures are isolated so later entries still run.
     */
    void process_pending_finalizers(std::size_t budget = kDefaultFinalizerBudget);

    std::expected<void, RuntimeError> run_loop();
    std::expected<void, RuntimeError> handle_return(LispVal result);
    void push(LispVal val) { stack_.push_back(val); }
    LispVal pop() {
        if (stack_.empty()) [[unlikely]] {
            /**
             * This indicates corrupt bytecode; run_loop will catch the
             * InvalidInstruction error returned by ops that call pop().
             * Return Nil to avoid UB from stack_.back() on empty vector.
             */
            return nanbox::Nil;
        }
        LispVal v = stack_.back(); stack_.pop_back(); return v;
    }

    void unpack_to_stack(LispVal value);

    /// Unified call dispatch helper
    std::expected<DispatchResult, RuntimeError> dispatch_callee(
        LispVal callee, uint32_t argc, bool is_tail, reader::lexer::Span call_span = {});

    /// Type-checked heap object accessor
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

    /// Build a proper list from `elements` in order.
    std::expected<LispVal, RuntimeError> make_list_payload(const std::vector<LispVal>& elements);

    /// Map RuntimeError variant/code to the reserved runtime.* catch tag symbol.
    std::expected<LispVal, RuntimeError> runtime_error_tag(const RuntimeError& err);

    /// Human-readable message extracted from any RuntimeError variant.
    [[nodiscard]] std::string runtime_error_message(const RuntimeError& err) const;

    /**
     * Build caught runtime payload:
     *   (runtime-error <tag> <message> <span> <stack-trace>)
     */
    std::expected<LispVal, RuntimeError> build_runtime_error_payload(
        const RuntimeError& err, LispVal tag, reader::lexer::Span span);

    /// True iff a catch frame tag should catch this runtime error tag.
    [[nodiscard]] bool runtime_catch_matches(LispVal catch_tag, LispVal specific_tag, LispVal super_tag) const;

    /// Snapshot stack trace frames at the runtime-error site.
    [[nodiscard]] std::vector<FrameInfo> capture_runtime_stack_trace(reader::lexer::Span span) const;

    /**
     * Attempt to route RuntimeError through catch frames.
     * Returns success if handled, or the original RuntimeError if uncaught.
     */
    std::expected<void, RuntimeError> do_runtime_error(const RuntimeError& err, reader::lexer::Span span);

    bool values_eqv(LispVal a, LispVal b) const;
    std::expected<void, RuntimeError> do_binary_arithmetic(OpCode op);
    std::expected<void, RuntimeError> pack_rest_args(uint32_t argc, uint32_t required);

    /**
     * Execute a Throw: pop tag+value, find matching CatchFrame, unwind.
     * Returns error if unhandled.
     */
    std::expected<void, RuntimeError> do_throw(LispVal tag, LispVal value,
                                                reader::lexer::Span span);

    /**
     * Unification helpers
     * Walk the logic-variable binding chain; return the first unbound var or non-var value.
     */
    LispVal deref(LispVal v);

    /// Occurs check: return true if logic-var lvar appears anywhere inside term.
    bool occurs_check(LispVal lvar, LispVal term);

    /**
     * Robinson unification with occurs check on both binding branches.
     * Binds variables and trails them; returns true on success.
     */
    bool unify(LispVal a, LispVal b);

    /**
     * Inner unification step.  Used recursively by `unify` for compound
     * recursion; does NOT drain the propagation queue.  External callers
     * should always call `unify`.
     */
    bool unify_internal(LispVal a, LispVal b);

    /**
     * Drain any pending propagator thunks (queue).  Called by the
     * outer `unify` exactly once after the inner step succeeds.  Returns
     * false on the first thunk that returns #f or errors; queue and dedup
     * set are left empty either way.
     */
    bool drain_propagators();

    /**
     * Deep-copy a term, replacing unbound logic variables with fresh copies.
     * Shared variables map to the same fresh copy (identity preserved).
     */
    std::expected<LispVal, RuntimeError> copy_term(LispVal term);
};

} ///< namespace eta::runtime::vm
