#pragma once

/// from VM so the hot run_loop path only pays a single null-pointer test.

#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <optional>
#include <vector>

#include "eta/reader/lexer.h"
#include "bytecode.h"

namespace eta::runtime::vm {

/// Forward decls
struct BreakLocation;
struct StopEvent;
enum class StopReason;
using StopCallback = std::function<void(const StopEvent&)>;

/// BreakLocation
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

/// Stop event
enum class StopReason { Breakpoint, Step, Pause, Exception };

struct StopEvent {
    StopReason           reason{StopReason::Pause};
    reader::lexer::Span  span{};
    std::string          exception_text;
};

/// DebugState

/**
 * @brief All debug machinery extracted from VM.
 *
 * The VM holds a std::unique_ptr<DebugState> (null when not debugging).
 * In production / non-debug runs the entire debug path is a single null
 *
 * Thread safety:
 *  - check_and_wait() is called on the VM thread.
 *  - resume(), step_*(), request_pause() are called on the DAP/IDE thread.
 *  - All shared state is protected by debug_mutex_ / bp_mutex_.
 */
class DebugState {
public:
    explicit DebugState(StopCallback cb)
        : stop_callback_(std::move(cb)) {}

    /// Breakpoints
    void set_breakpoints(std::vector<BreakLocation> locs) {
        std::sort(locs.begin(), locs.end());
        std::lock_guard<std::mutex> lk(bp_mutex_);
        breakpoints_ = std::move(locs);
    }

    /// Stepping / resume
    void resume() {
        std::lock_guard<std::mutex> lk(debug_mutex_);
        step_mode_  = StepMode::None;
        is_paused_  = false;
        last_bp_file_ = 0;
        last_bp_line_ = 0;
        debug_cv_.notify_one();
    }

    void step_over(reader::lexer::Span sp, std::size_t depth) {
        std::lock_guard<std::mutex> lk(debug_mutex_);
        step_mode_         = StepMode::Over;
        step_target_depth_ = depth;
        step_origin_file_  = sp.file_id;
        step_origin_line_  = sp.start.line;
        step_epoch_        = step_current_epoch_;
        is_paused_         = false;
        debug_cv_.notify_one();
    }

    void step_in(reader::lexer::Span sp, std::size_t depth) {
        std::lock_guard<std::mutex> lk(debug_mutex_);
        step_mode_         = StepMode::In;
        step_target_depth_ = depth;
        step_origin_file_  = sp.file_id;
        step_origin_line_  = sp.start.line;
        step_epoch_        = step_current_epoch_;
        is_paused_         = false;
        debug_cv_.notify_one();
    }

    void step_out(std::size_t depth) {
        std::lock_guard<std::mutex> lk(debug_mutex_);
        step_mode_         = StepMode::Out;
        step_target_depth_ = depth;
        step_epoch_        = step_current_epoch_;
        is_paused_         = false;
        debug_cv_.notify_one();
    }

    void request_pause() {
        should_pause_.store(true, std::memory_order_relaxed);
    }

    [[nodiscard]] bool is_paused() const noexcept {
        std::lock_guard<std::mutex> lk(debug_mutex_);
        return is_paused_;
    }

    /// Called from VM when a call/cc context switch happens
    void notify_continuation_jump() {
        std::lock_guard<std::mutex> lk(debug_mutex_);
        step_current_epoch_++;
    }

    /// Called from VM when an unhandled exception is about to propagate
    void notify_exception(const std::string& msg, reader::lexer::Span sp) {
        StopEvent ev{StopReason::Exception, sp, msg};
        {
            std::lock_guard<std::mutex> lk(debug_mutex_);
            is_paused_ = true;
            stopped_span_ = sp;
        }
        stop_callback_(ev);
        {
            std::unique_lock<std::mutex> lk(debug_mutex_);
            debug_cv_.wait(lk, [this] { return !is_paused_; });
        }
    }

    /**
     * Return the source span that triggered the most recent stop.
     * Only meaningful while is_paused() is true.
     */
    [[nodiscard]] reader::lexer::Span stopped_span() const noexcept {
        std::lock_guard<std::mutex> lk(debug_mutex_);
        return stopped_span_;
    }

    /**
     * @brief Check whether the VM should pause at the current instruction.
     *
     * If a pause condition is met the function blocks (with is_paused_ = true)
     * until resume()/step_*() is called from another thread.
     *
     * @param sp     Source span of the about-to-execute instruction.
     * @param depth  Current call-frame depth (frames_.size()).
     */
    void check_and_wait(reader::lexer::Span sp, std::size_t depth) {
        auto ev = check_stop(sp, depth);
        if (!ev) return;

        {
            std::lock_guard<std::mutex> lk(debug_mutex_);
            is_paused_ = true;
            stopped_span_ = sp;
        }
        stop_callback_(*ev);   ///< called WITHOUT lock held
        {
            std::unique_lock<std::mutex> lk(debug_mutex_);
            debug_cv_.wait(lk, [this] { return !is_paused_; });
        }
    }

private:
    enum class StepMode { None, Continue, Over, In, Out };

    StopCallback               stop_callback_;

    mutable std::mutex         debug_mutex_;
    std::condition_variable    debug_cv_;
    bool                       is_paused_{false};
    reader::lexer::Span        stopped_span_{};  ///< span that triggered the most recent stop

    std::mutex                 bp_mutex_;
    std::vector<BreakLocation> breakpoints_;

    StepMode    step_mode_{StepMode::None};
    std::size_t step_target_depth_{0};
    uint32_t    step_origin_file_{0};
    uint32_t    step_origin_line_{0};
    uint32_t    step_epoch_{0};         ///< epoch when step was armed
    uint32_t    step_current_epoch_{0}; ///< incremented on call/cc

    /**
     * Track last breakpoint hit to avoid re-firing on consecutive instructions
     * with the same source location (fixes first-breakpoint off-by-one).
     */
    uint32_t    last_bp_file_{0};
    uint32_t    last_bp_line_{0};

    std::atomic<bool> should_pause_{false};

    /// Internal check (no waiting)
    std::optional<StopEvent> check_stop(reader::lexer::Span sp, std::size_t depth) {
        /// 1. Async pause request
        if (should_pause_.load(std::memory_order_relaxed)) {
            should_pause_.store(false, std::memory_order_relaxed);
            return StopEvent{StopReason::Pause, sp, {}};
        }

        /// 2. Breakpoint
        if (sp.file_id != 0) {
            BreakLocation loc{sp.file_id, sp.start.line};
            std::lock_guard<std::mutex> lk(bp_mutex_);
            if (std::binary_search(breakpoints_.begin(), breakpoints_.end(), loc)) {
                /**
                 * Skip if this is the same location as the last breakpoint hit.
                 * This prevents the first breakpoint from firing on the instruction
                 * before the actual breakpoint line when consecutive bytecodes
                 * share the same source span.
                 */
                if (sp.file_id == last_bp_file_ && sp.start.line == last_bp_line_) {
                } else {
                    std::lock_guard<std::mutex> dlk(debug_mutex_);
                    step_mode_ = StepMode::None;
                    last_bp_file_ = sp.file_id;
                    last_bp_line_ = sp.start.line;
                    return StopEvent{StopReason::Breakpoint, sp, {}};
                }
            } else {
                last_bp_file_ = 0;
                last_bp_line_ = 0;
            }
        }

        /// 3. Step mode
        {
            std::lock_guard<std::mutex> lk(debug_mutex_);
            if (step_mode_ == StepMode::None) return std::nullopt;

            /// Stale step from before a call/cc context switch
            if (step_epoch_ != step_current_epoch_) {
                step_mode_ = StepMode::None;
                return std::nullopt;
            }

            bool should_stop = false;
            switch (step_mode_) {
                case StepMode::In:
                    should_stop = (sp.file_id != step_origin_file_ || sp.start.line != step_origin_line_);
                    break;
                case StepMode::Over:
                    should_stop = (depth <= step_target_depth_) &&
                                  (sp.file_id != step_origin_file_ || sp.start.line != step_origin_line_);
                    break;
                case StepMode::Out:
                    should_stop = (depth < step_target_depth_);
                    break;
                case StepMode::None:
                case StepMode::Continue:
                    break;
            }

            if (should_stop) {
                step_mode_ = StepMode::None;
                return StopEvent{StopReason::Step, sp, {}};
            }
        }
        return std::nullopt;
    }
};

} ///< namespace eta::runtime::vm

