#pragma once

#include <atomic>
#include <cstddef>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

#include "eta/runtime/prof/aggregator.h"
#include "eta/runtime/prof/archive.h"
#include "eta/runtime/prof/frame_id.h"
#include "eta/runtime/prof/sample_buffer.h"
#include "eta/runtime/prof/speedscope.h"

namespace eta::runtime::vm {
struct BytecodeFunction;
}

namespace eta::runtime::prof {

/**
 * @brief Lightweight runtime counters exposed for tests.
 */
struct Phase0Counters {
    std::uint64_t setup_frame_calls{0};
    std::uint64_t tail_reuse_calls{0};
    std::uint64_t returns{0};
    std::uint64_t primitive_enter_calls{0};
    std::uint64_t primitive_exit_calls{0};
    std::uint64_t continuation_jumps{0};
    std::size_t shadow_depth{0};
};

/**
 * @brief Global profiler runtime used by VM hook points.
 */
class ProfilerRuntime {
public:
    /**
     * @brief Active profiler session mode.
     */
    enum class SessionMode : std::uint8_t {
        None,
        Trace,
        Sample,
    };

    ~ProfilerRuntime();

    void set_enabled(bool enabled) noexcept;
    [[nodiscard]] bool enabled() const noexcept;
    [[nodiscard]] bool sampling_active() const noexcept;

    /**
     * @brief Start a trace session and clear prior aggregates.
     * @return session id on success, nullopt when another session is active.
     */
    [[nodiscard]] std::optional<std::uint64_t> start_trace_session();

    /**
     * @brief Start a sampling session and clear prior sampled profiles.
     * @param sample_hz Requested sampling frequency (clamped to [1, 10000]).
     * @return session id on success, nullopt when another session is active.
     */
    [[nodiscard]] std::optional<std::uint64_t> start_sample_session(std::uint32_t sample_hz = 1000);

    /**
     * @brief Stop the active trace session.
     * @return true when @p session_id matches the active session.
     */
    [[nodiscard]] bool stop_trace_session(std::uint64_t session_id);

    /**
     * @brief Stop the active sampling session.
     * @return true when @p session_id matches the active session.
     */
    [[nodiscard]] bool stop_sample_session(std::uint64_t session_id);

    /**
     * @brief Active trace session id, or zero when no session is running.
     */
    [[nodiscard]] std::uint64_t active_session_id() const noexcept;

    /**
     * @brief Render the most recent trace aggregate as a pretty text report.
     */
    [[nodiscard]] std::string render_pretty_report(std::size_t top_n = 20) const;

    /**
     * @brief Render report for a completed trace session handle.
     */
    [[nodiscard]] std::optional<std::string> render_pretty_report_for_session(
        std::uint64_t session_id,
        std::size_t top_n = 20) const;

    /**
     * @brief Render the most recent trace aggregate as a JSON report.
     */
    [[nodiscard]] std::string render_json_report(std::size_t top_n = 20) const;

    /**
     * @brief Render JSON report for a completed profile session handle.
     */
    [[nodiscard]] std::optional<std::string> render_json_report_for_session(
        std::uint64_t session_id,
        std::size_t top_n = 20) const;

    /**
     * @brief Render the most recent sampled aggregate as a speedscope JSON document.
     */
    [[nodiscard]] std::string render_speedscope_report() const;

    /**
     * @brief Render speedscope JSON for a completed sampling session handle.
     */
    [[nodiscard]] std::optional<std::string> render_speedscope_report_for_session(
        std::uint64_t session_id) const;

    /**
     * @brief Render an `eta-prof` archive payload for a completed session.
     */
    [[nodiscard]] std::optional<std::string> render_archive_report_for_session(
        std::uint64_t session_id) const;

    /**
     * @brief Render Chrome trace JSON for a completed session.
     */
    [[nodiscard]] std::optional<std::string> render_chrome_report_for_session(
        std::uint64_t session_id) const;

    /**
     * @brief Increment a named profiler counter for the active session.
     */
    void add_counter(std::string_view name, std::uint64_t value = 1);
    void on_allocation(std::uint64_t bytes_allocated);

    void on_setup_frame(const vm::BytecodeFunction* callee);
    void on_tail_reuse(const vm::BytecodeFunction* callee);
    void on_return(const vm::BytecodeFunction* from_func, const vm::BytecodeFunction* to_func);
    void on_primitive_enter(const char* builtin_name = nullptr);
    void on_primitive_exit();
    void on_continuation_jump(std::size_t target_depth);
    void on_vm_safepoint();
    void push_user_region(std::string_view name);
    void pop_user_region();

    void reset_for_test();
    [[nodiscard]] Phase0Counters counters_for_test() const;
    [[nodiscard]] std::unordered_map<std::string, FlatStats> flat_by_name_for_test() const;

private:
    struct ShadowFrame {
        FrameId frame_id{0};
        std::uint64_t enter_ns{0};
        std::uint64_t child_ns{0};
    };

    struct ThreadState {
        std::mutex mutex;
        std::vector<ShadowFrame> stack;
        SampleBuffer samples;
        std::uint64_t thread_id{0};
        std::uint64_t last_sample_epoch{0};
    };

    [[nodiscard]] ThreadState& current_thread_state();
    [[nodiscard]] std::vector<std::shared_ptr<ThreadState>> live_thread_states() const;
    void flush_all_threads();

    [[nodiscard]] FrameId intern_function_frame(const vm::BytecodeFunction* func);
    [[nodiscard]] FrameId intern_builtin_frame(const char* builtin_name);
    [[nodiscard]] FrameId intern_continuation_resume_frame();
    [[nodiscard]] FrameId intern_user_region_frame(std::string_view name);

    void enter_frame(FrameId frame_id);
    void leave_frame();
    void unwind_to_depth(std::size_t target_depth);
    void maybe_capture_sample(ThreadState& state);
    void maybe_capture_sample_locked(ThreadState& state, std::uint64_t now_ns);

    void enter_frame_locked(ThreadState& state, FrameId frame_id, std::uint64_t now_ns);
    void leave_frame_locked(ThreadState& state, std::uint64_t now_ns);

    [[nodiscard]] std::optional<std::uint64_t> start_session(SessionMode mode, std::uint32_t sample_hz);
    [[nodiscard]] bool stop_session(std::uint64_t session_id, SessionMode mode);
    [[nodiscard]] std::optional<SessionMode> session_mode_for_handle(std::uint64_t session_id) const noexcept;
    [[nodiscard]] std::optional<ArchiveSession> archive_for_session_unlocked(
        std::uint64_t session_id) const;
    [[nodiscard]] std::optional<std::string> render_speedscope_report_for_session_unlocked(
        std::uint64_t session_id) const;
    [[nodiscard]] bool session_matches(std::uint64_t session_id, SessionMode mode) const noexcept;

    static constexpr std::uint32_t kDefaultSampleHz = 1000;
    static constexpr std::uint32_t kMaxSampleHz = 10000;
    [[nodiscard]] static std::uint32_t clamp_sample_hz(std::uint32_t sample_hz) noexcept;

    void clear_sample_profiles_locked();
    void reset_sampling_state_for_live_threads();
    void drain_sample_buffers();
    void sampler_loop();
    void start_sampler_locked(std::uint32_t sample_hz);
    void stop_sampler_locked();

    std::atomic<bool> enabled_{false};
    std::atomic<bool> sampling_active_{false};
    std::atomic<bool> trace_events_enabled_{true};
    std::atomic<std::uint64_t> sample_epoch_{0};

    std::atomic<std::uint64_t> next_session_id_{1};
    std::atomic<std::uint64_t> active_session_id_{0};
    std::atomic<std::uint64_t> last_completed_session_id_{0};
    std::atomic<SessionMode> active_session_mode_{SessionMode::None};
    std::atomic<SessionMode> last_completed_session_mode_{SessionMode::None};

    std::atomic<std::uint64_t> setup_frame_calls_{0};
    std::atomic<std::uint64_t> tail_reuse_calls_{0};
    std::atomic<std::uint64_t> returns_{0};
    std::atomic<std::uint64_t> primitive_enter_calls_{0};
    std::atomic<std::uint64_t> primitive_exit_calls_{0};
    std::atomic<std::uint64_t> continuation_jumps_{0};

    mutable std::mutex phase0_mutex_;
    std::size_t phase0_shadow_depth_{0};

    mutable std::mutex session_mutex_;
    Aggregator aggregator_;
    FrameIdInterner interner_;

    mutable std::mutex sample_profiles_mutex_;
    std::unordered_map<std::uint64_t, SpeedscopeThreadProfile> sampled_profiles_;

    mutable std::mutex counters_mutex_;
    std::unordered_map<std::string, std::uint64_t> counters_;

    std::mutex sampler_mutex_;
    std::condition_variable sampler_cv_;
    std::thread sampler_thread_;
    bool sampler_stop_requested_{false};
    std::uint32_t sample_hz_{kDefaultSampleHz};

    mutable std::mutex thread_states_mutex_;
    mutable std::vector<std::weak_ptr<ThreadState>> thread_states_;
};

/**
 * @brief RAII helper around primitive call enter/exit hooks.
 */
class ScopedPrimitiveCall {
public:
    explicit ScopedPrimitiveCall(const char* builtin_name = nullptr);
    ~ScopedPrimitiveCall();

private:
    bool active_{false};
};

/**
 * @brief Return the process-global profiler runtime instance.
 */
ProfilerRuntime& runtime_profiler();

} ///< namespace eta::runtime::prof
