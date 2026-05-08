#include "eta/runtime/prof/profiler.h"

#include <algorithm>
#include <chrono>
#include <functional>
#include <string>
#include <thread>
#include <utility>

#include "eta/runtime/prof/archive.h"
#include "eta/runtime/prof/clock.h"
#include "eta/runtime/prof/chrome.h"
#include "eta/runtime/prof/report.h"
#include "eta/runtime/prof/speedscope.h"
#include "eta/runtime/vm/bytecode.h"

namespace eta::runtime::prof {

namespace {

constexpr std::string_view kAnonymousFrame = "<anonymous>";
constexpr std::string_view kBuiltinFrame = "<builtin>";
constexpr std::string_view kContinuationResumeFrame = "<continuation-resume>";
constexpr std::string_view kUserRegionFrame = "<region>";

[[nodiscard]] std::uint64_t elapsed_ns(const std::uint64_t enter_ns,
                                       const std::uint64_t leave_ns) noexcept {
    return leave_ns >= enter_ns ? (leave_ns - enter_ns) : 0;
}

[[nodiscard]] std::uint64_t current_thread_numeric_id() noexcept {
    return static_cast<std::uint64_t>(std::hash<std::thread::id>{}(std::this_thread::get_id()));
}

[[nodiscard]] FrameKind classify_function_kind(const vm::BytecodeFunction* func) {
    if (!func) return FrameKind::TopLevel;
    const std::string_view name = func->name;
    if (name.starts_with("<lambda@")) return FrameKind::AnonymousLambda;
    if (name.ends_with("_init")) return FrameKind::TopLevel;
    return FrameKind::EtaFunction;
}

[[nodiscard]] eta::reader::lexer::Span function_span(const vm::BytecodeFunction* func) {
    if (!func) return {};
    for (const auto& span : func->source_map) {
        if (span.file_id != 0) return span;
    }
    return {};
}

} ///< namespace

ProfilerRuntime& runtime_profiler() {
    static ProfilerRuntime runtime;
    return runtime;
}

ProfilerRuntime::~ProfilerRuntime() {
    enabled_.store(false, std::memory_order_relaxed);
    sampling_active_.store(false, std::memory_order_relaxed);
    sample_epoch_.store(0, std::memory_order_relaxed);
    stop_sampler_locked();
}

void ProfilerRuntime::set_enabled(const bool enabled) noexcept {
    enabled_.store(enabled, std::memory_order_relaxed);
    if (!enabled) {
        sampling_active_.store(false, std::memory_order_relaxed);
        sample_epoch_.store(0, std::memory_order_relaxed);
        if (active_session_id_.load(std::memory_order_relaxed) == 0) {
            trace_events_enabled_.store(false, std::memory_order_relaxed);
        }
        return;
    }

    if (active_session_id_.load(std::memory_order_relaxed) == 0) {
        trace_events_enabled_.store(true, std::memory_order_relaxed);
    }
}

bool ProfilerRuntime::enabled() const noexcept {
    return enabled_.load(std::memory_order_relaxed);
}

bool ProfilerRuntime::sampling_active() const noexcept {
    return sampling_active_.load(std::memory_order_relaxed);
}

std::optional<std::uint64_t> ProfilerRuntime::start_trace_session() {
    return start_session(SessionMode::Trace, kDefaultSampleHz);
}

std::optional<std::uint64_t> ProfilerRuntime::start_sample_session(const std::uint32_t sample_hz) {
    return start_session(SessionMode::Sample, sample_hz);
}

std::optional<std::uint64_t> ProfilerRuntime::start_session(const SessionMode mode,
                                                            const std::uint32_t sample_hz) {
    std::lock_guard session_lock(session_mutex_);
    if (active_session_id_.load(std::memory_order_relaxed) != 0) {
        return std::nullopt;
    }

    stop_sampler_locked();
    flush_all_threads();
    aggregator_.clear();
    interner_.clear();
    clear_sample_profiles_locked();
    reset_sampling_state_for_live_threads();
    {
        std::lock_guard counters_lock(counters_mutex_);
        counters_.clear();
    }

    const auto session_id = next_session_id_.fetch_add(1, std::memory_order_relaxed);
    last_completed_session_id_.store(0, std::memory_order_relaxed);
    last_completed_session_mode_.store(SessionMode::None, std::memory_order_relaxed);
    active_session_id_.store(session_id, std::memory_order_relaxed);
    active_session_mode_.store(mode, std::memory_order_relaxed);
    enabled_.store(true, std::memory_order_relaxed);
    sample_epoch_.store(0, std::memory_order_relaxed);

    if (mode == SessionMode::Sample) {
        trace_events_enabled_.store(false, std::memory_order_relaxed);
        sampling_active_.store(true, std::memory_order_relaxed);
        sample_epoch_.store(1, std::memory_order_relaxed);
        start_sampler_locked(clamp_sample_hz(sample_hz));
    } else {
        sampling_active_.store(false, std::memory_order_relaxed);
        trace_events_enabled_.store(true, std::memory_order_relaxed);
    }

    return session_id;
}

bool ProfilerRuntime::stop_trace_session(const std::uint64_t session_id) {
    return stop_session(session_id, SessionMode::Trace);
}

bool ProfilerRuntime::stop_sample_session(const std::uint64_t session_id) {
    return stop_session(session_id, SessionMode::Sample);
}

bool ProfilerRuntime::stop_session(const std::uint64_t session_id, const SessionMode mode) {
    std::lock_guard session_lock(session_mutex_);
    const auto active_id = active_session_id_.load(std::memory_order_relaxed);
    const auto active_mode = active_session_mode_.load(std::memory_order_relaxed);
    if (active_id != session_id || active_mode != mode) {
        return false;
    }

    enabled_.store(false, std::memory_order_relaxed);
    sampling_active_.store(false, std::memory_order_relaxed);
    sample_epoch_.store(0, std::memory_order_relaxed);

    if (mode == SessionMode::Sample) {
        stop_sampler_locked();
        drain_sample_buffers();
        flush_all_threads();
        trace_events_enabled_.store(false, std::memory_order_relaxed);
    } else {
        trace_events_enabled_.store(true, std::memory_order_relaxed);
        flush_all_threads();
        trace_events_enabled_.store(false, std::memory_order_relaxed);
    }

    active_session_id_.store(0, std::memory_order_relaxed);
    active_session_mode_.store(SessionMode::None, std::memory_order_relaxed);
    last_completed_session_id_.store(session_id, std::memory_order_relaxed);
    last_completed_session_mode_.store(mode, std::memory_order_relaxed);
    return true;
}

std::uint64_t ProfilerRuntime::active_session_id() const noexcept {
    return active_session_id_.load(std::memory_order_relaxed);
}

std::string ProfilerRuntime::render_pretty_report(const std::size_t top_n) const {
    std::lock_guard session_lock(session_mutex_);
    std::unordered_map<std::string, std::uint64_t> counters;
    {
        std::lock_guard counters_lock(counters_mutex_);
        counters = counters_;
    }
    return ::eta::runtime::prof::render_pretty_report(aggregator_, interner_, counters, top_n);
}

std::optional<std::string> ProfilerRuntime::render_pretty_report_for_session(
    const std::uint64_t session_id,
    const std::size_t top_n) const {
    std::lock_guard session_lock(session_mutex_);
    auto archive = archive_for_session_unlocked(session_id);
    if (!archive) return std::nullopt;
    return render_pretty_archive_report(*archive, top_n);
}

std::string ProfilerRuntime::render_json_report(const std::size_t top_n) const {
    std::lock_guard session_lock(session_mutex_);
    std::unordered_map<std::string, std::uint64_t> counters;
    {
        std::lock_guard counters_lock(counters_mutex_);
        counters = counters_;
    }
    return ::eta::runtime::prof::render_json_report(aggregator_, interner_, counters, top_n);
}

std::optional<std::string> ProfilerRuntime::render_json_report_for_session(
    const std::uint64_t session_id,
    const std::size_t top_n) const {
    std::lock_guard session_lock(session_mutex_);
    auto archive = archive_for_session_unlocked(session_id);
    if (!archive) return std::nullopt;
    return render_json_archive_report(*archive, top_n);
}

std::string ProfilerRuntime::render_speedscope_report() const {
    std::vector<std::pair<std::uint64_t, SpeedscopeThreadProfile>> sorted_profiles;
    {
        std::lock_guard profiles_lock(sample_profiles_mutex_);
        sorted_profiles.reserve(sampled_profiles_.size());
        for (const auto& [thread_id, profile] : sampled_profiles_) {
            sorted_profiles.emplace_back(thread_id, profile);
        }
    }

    std::sort(sorted_profiles.begin(),
              sorted_profiles.end(),
              [](const auto& lhs, const auto& rhs) { return lhs.first < rhs.first; });

    std::vector<SpeedscopeThreadProfile> profiles;
    profiles.reserve(sorted_profiles.size());
    for (auto& [thread_id, profile] : sorted_profiles) {
        (void) thread_id;
        profiles.push_back(std::move(profile));
    }
    return write_speedscope_json(interner_, profiles);
}

std::optional<std::string> ProfilerRuntime::render_speedscope_report_for_session(
    const std::uint64_t session_id) const {
    std::lock_guard session_lock(session_mutex_);
    auto archive = archive_for_session_unlocked(session_id);
    if (!archive) return std::nullopt;
    return render_speedscope_archive_report(*archive);
}

std::optional<std::string> ProfilerRuntime::render_archive_report_for_session(
    const std::uint64_t session_id) const {
    std::lock_guard session_lock(session_mutex_);
    auto archive = archive_for_session_unlocked(session_id);
    if (!archive) return std::nullopt;
    return write_eta_prof_archive(*archive);
}

std::optional<std::string> ProfilerRuntime::render_chrome_report_for_session(
    const std::uint64_t session_id) const {
    std::lock_guard session_lock(session_mutex_);
    auto archive = archive_for_session_unlocked(session_id);
    if (!archive) return std::nullopt;
    return render_chrome_archive_report(*archive);
}

std::optional<std::string> ProfilerRuntime::render_speedscope_report_for_session_unlocked(
    const std::uint64_t session_id) const {
    if (!session_matches(session_id, SessionMode::Sample)) {
        return std::nullopt;
    }
    return render_speedscope_report();
}

std::optional<ProfilerRuntime::SessionMode> ProfilerRuntime::session_mode_for_handle(
    const std::uint64_t session_id) const noexcept {
    const auto active_id = active_session_id_.load(std::memory_order_relaxed);
    if (session_id == active_id) {
        return active_session_mode_.load(std::memory_order_relaxed);
    }

    const auto completed_id = last_completed_session_id_.load(std::memory_order_relaxed);
    if (session_id == completed_id) {
        return last_completed_session_mode_.load(std::memory_order_relaxed);
    }
    return std::nullopt;
}

std::optional<ArchiveSession> ProfilerRuntime::archive_for_session_unlocked(
    const std::uint64_t session_id) const {
    auto mode = session_mode_for_handle(session_id);
    if (!mode.has_value() || *mode == SessionMode::None) {
        return std::nullopt;
    }

    ArchiveSession archive;
    archive.mode = (*mode == SessionMode::Sample) ? ArchiveMode::Sample : ArchiveMode::Trace;
    archive.frames.reserve(interner_.size());
    for (std::size_t i = 0; i < interner_.size(); ++i) {
        const auto key = interner_.key_for(static_cast<FrameId>(i));
        archive.frames.push_back(key.value_or(FrameKey{}));
    }

    {
        std::lock_guard counters_lock(counters_mutex_);
        archive.counters = counters_;
    }

    for (const auto& [frame_id, stats] : aggregator_.flat_snapshot()) {
        archive.flat_rows.push_back(ArchiveFlatRow{
            .frame_id = frame_id,
            .stats = stats,
        });
    }
    std::sort(archive.flat_rows.begin(),
              archive.flat_rows.end(),
              [](const ArchiveFlatRow& lhs, const ArchiveFlatRow& rhs) {
                  return lhs.frame_id < rhs.frame_id;
              });

    if (*mode == SessionMode::Sample) {
        std::vector<std::pair<std::uint64_t, SpeedscopeThreadProfile>> sorted_profiles;
        {
            std::lock_guard profiles_lock(sample_profiles_mutex_);
            sorted_profiles.reserve(sampled_profiles_.size());
            for (const auto& [thread_id, profile] : sampled_profiles_) {
                sorted_profiles.emplace_back(thread_id, profile);
            }
        }
        std::sort(sorted_profiles.begin(),
                  sorted_profiles.end(),
                  [](const auto& lhs, const auto& rhs) { return lhs.first < rhs.first; });
        archive.sample_profiles.reserve(sorted_profiles.size());
        for (auto& [thread_id, profile] : sorted_profiles) {
            (void) thread_id;
            archive.sample_profiles.push_back(std::move(profile));
        }
        return archive;
    }
    for (const auto& [edge, stats] : aggregator_.tree_snapshot()) {
        archive.tree_rows.push_back(ArchiveTreeRow{
            .parent_frame_id = edge.parent,
            .child_frame_id = edge.child,
            .stats = stats,
        });
    }
    std::sort(archive.tree_rows.begin(),
              archive.tree_rows.end(),
              [](const ArchiveTreeRow& lhs, const ArchiveTreeRow& rhs) {
                  if (lhs.parent_frame_id != rhs.parent_frame_id) {
                      return lhs.parent_frame_id < rhs.parent_frame_id;
                  }
                  return lhs.child_frame_id < rhs.child_frame_id;
              });
    return archive;
}

bool ProfilerRuntime::session_matches(const std::uint64_t session_id, const SessionMode mode) const noexcept {
    auto session_mode = session_mode_for_handle(session_id);
    return session_mode.has_value() && *session_mode == mode;
}

void ProfilerRuntime::on_setup_frame(const vm::BytecodeFunction* callee) {
    if (!enabled_.load(std::memory_order_relaxed)) return;
    setup_frame_calls_.fetch_add(1, std::memory_order_relaxed);
    enter_frame(intern_function_frame(callee));
}

void ProfilerRuntime::on_tail_reuse(const vm::BytecodeFunction* callee) {
    if (!enabled_.load(std::memory_order_relaxed)) return;
    tail_reuse_calls_.fetch_add(1, std::memory_order_relaxed);

    auto& state = current_thread_state();
    const auto now = ::eta::runtime::prof::now_ns();
    std::lock_guard state_lock(state.mutex);

    if (!state.stack.empty()) {
        leave_frame_locked(state, now);
    }
    enter_frame_locked(state, intern_function_frame(callee), now);
    maybe_capture_sample_locked(state, now);

    std::lock_guard phase0_lock(phase0_mutex_);
    phase0_shadow_depth_ = state.stack.size();
}

void ProfilerRuntime::on_return(const vm::BytecodeFunction* /*from_func*/,
                                const vm::BytecodeFunction* /*to_func*/) {
    if (!enabled_.load(std::memory_order_relaxed)) return;
    returns_.fetch_add(1, std::memory_order_relaxed);
    leave_frame();
}

void ProfilerRuntime::on_primitive_enter(const char* builtin_name) {
    if (!enabled_.load(std::memory_order_relaxed)) return;
    primitive_enter_calls_.fetch_add(1, std::memory_order_relaxed);
    enter_frame(intern_builtin_frame(builtin_name));
}

void ProfilerRuntime::on_primitive_exit() {
    if (!enabled_.load(std::memory_order_relaxed)) return;
    primitive_exit_calls_.fetch_add(1, std::memory_order_relaxed);
    leave_frame();
}

void ProfilerRuntime::on_continuation_jump(const std::size_t target_depth) {
    if (!enabled_.load(std::memory_order_relaxed)) return;
    continuation_jumps_.fetch_add(1, std::memory_order_relaxed);
    unwind_to_depth(target_depth);
}

void ProfilerRuntime::on_vm_safepoint() {
    if (!sampling_active_.load(std::memory_order_relaxed)) return;
    auto& state = current_thread_state();
    maybe_capture_sample(state);
}

void ProfilerRuntime::push_user_region(const std::string_view name) {
    if (!enabled_.load(std::memory_order_relaxed)) return;
    enter_frame(intern_user_region_frame(name));
}

void ProfilerRuntime::pop_user_region() {
    if (!enabled_.load(std::memory_order_relaxed)) return;
    leave_frame();
}

void ProfilerRuntime::add_counter(const std::string_view name, const std::uint64_t value) {
    if (value == 0u || name.empty()) return;
    if (!enabled_.load(std::memory_order_relaxed)) return;

    std::lock_guard counters_lock(counters_mutex_);
    counters_[std::string(name)] += value;
}

void ProfilerRuntime::on_allocation(const std::uint64_t bytes_allocated) {
    if (bytes_allocated == 0u) return;
    if (!enabled_.load(std::memory_order_relaxed)) return;

    auto& state = current_thread_state();
    std::lock_guard state_lock(state.mutex);
    if (state.stack.empty()) return;

    FrameId target_frame_id = state.stack.back().frame_id;
    for (auto it = state.stack.rbegin(); it != state.stack.rend(); ++it) {
        const auto key = interner_.key_for(it->frame_id);
        if (!key.has_value() || key->kind != FrameKind::Builtin) {
            target_frame_id = it->frame_id;
            break;
        }
    }
    aggregator_.record_alloc(target_frame_id, bytes_allocated);
}

void ProfilerRuntime::reset_for_test() {
    enabled_.store(false, std::memory_order_relaxed);
    sampling_active_.store(false, std::memory_order_relaxed);
    trace_events_enabled_.store(false, std::memory_order_relaxed);
    sample_epoch_.store(0, std::memory_order_relaxed);
    next_session_id_.store(1, std::memory_order_relaxed);
    active_session_id_.store(0, std::memory_order_relaxed);
    last_completed_session_id_.store(0, std::memory_order_relaxed);
    active_session_mode_.store(SessionMode::None, std::memory_order_relaxed);
    last_completed_session_mode_.store(SessionMode::None, std::memory_order_relaxed);
    setup_frame_calls_.store(0, std::memory_order_relaxed);
    tail_reuse_calls_.store(0, std::memory_order_relaxed);
    returns_.store(0, std::memory_order_relaxed);
    primitive_enter_calls_.store(0, std::memory_order_relaxed);
    primitive_exit_calls_.store(0, std::memory_order_relaxed);
    continuation_jumps_.store(0, std::memory_order_relaxed);

    {
        std::lock_guard phase0_lock(phase0_mutex_);
        phase0_shadow_depth_ = 0;
    }

    std::lock_guard session_lock(session_mutex_);
    stop_sampler_locked();
    flush_all_threads();
    aggregator_.clear();
    interner_.clear();
    clear_sample_profiles_locked();
    reset_sampling_state_for_live_threads();
    {
        std::lock_guard counters_lock(counters_mutex_);
        counters_.clear();
    }
}

Phase0Counters ProfilerRuntime::counters_for_test() const {
    Phase0Counters out;
    out.setup_frame_calls = setup_frame_calls_.load(std::memory_order_relaxed);
    out.tail_reuse_calls = tail_reuse_calls_.load(std::memory_order_relaxed);
    out.returns = returns_.load(std::memory_order_relaxed);
    out.primitive_enter_calls = primitive_enter_calls_.load(std::memory_order_relaxed);
    out.primitive_exit_calls = primitive_exit_calls_.load(std::memory_order_relaxed);
    out.continuation_jumps = continuation_jumps_.load(std::memory_order_relaxed);
    std::lock_guard phase0_lock(phase0_mutex_);
    out.shadow_depth = phase0_shadow_depth_;
    return out;
}

std::unordered_map<std::string, FlatStats> ProfilerRuntime::flat_by_name_for_test() const {
    std::lock_guard session_lock(session_mutex_);
    std::unordered_map<std::string, FlatStats> out;
    for (const auto& [frame_id, stats] : aggregator_.flat_snapshot()) {
        const auto key = interner_.key_for(frame_id);
        const std::string frame_name = key ? key->qualified_name : std::string(kAnonymousFrame);
        auto& slot = out[frame_name];
        slot.self_ns += stats.self_ns;
        slot.inclusive_ns += stats.inclusive_ns;
        slot.calls += stats.calls;
        slot.bytes_allocated += stats.bytes_allocated;
    }
    return out;
}

ProfilerRuntime::ThreadState& ProfilerRuntime::current_thread_state() {
    thread_local std::shared_ptr<ThreadState> tls_state;
    if (!tls_state) {
        auto created = std::make_shared<ThreadState>();
        created->thread_id = current_thread_numeric_id();
        {
            std::lock_guard lock(thread_states_mutex_);
            thread_states_.push_back(created);
        }
        tls_state = std::move(created);
    }
    return *tls_state;
}

std::vector<std::shared_ptr<ProfilerRuntime::ThreadState>> ProfilerRuntime::live_thread_states() const {
    std::vector<std::shared_ptr<ThreadState>> out;
    std::lock_guard lock(thread_states_mutex_);

    std::vector<std::weak_ptr<ThreadState>> compact;
    compact.reserve(thread_states_.size());
    for (const auto& weak : thread_states_) {
        if (auto state = weak.lock()) {
            out.push_back(state);
            compact.push_back(state);
        }
    }
    thread_states_.swap(compact);
    return out;
}

void ProfilerRuntime::flush_all_threads() {
    const auto now = ::eta::runtime::prof::now_ns();
    for (const auto& state : live_thread_states()) {
        std::lock_guard state_lock(state->mutex);
        while (!state->stack.empty()) {
            leave_frame_locked(*state, now);
        }
    }

    std::lock_guard phase0_lock(phase0_mutex_);
    phase0_shadow_depth_ = 0;
}

FrameId ProfilerRuntime::intern_function_frame(const vm::BytecodeFunction* func) {
    FrameKey key;
    key.kind = classify_function_kind(func);
    key.qualified_name =
        (func && !func->name.empty()) ? func->name : std::string(kAnonymousFrame);
    key.source_span = function_span(func);
    return interner_.intern(key);
}

FrameId ProfilerRuntime::intern_builtin_frame(const char* builtin_name) {
    FrameKey key;
    key.kind = FrameKind::Builtin;
    key.qualified_name =
        (builtin_name && builtin_name[0] != '\0') ? builtin_name : std::string(kBuiltinFrame);
    return interner_.intern(key);
}

FrameId ProfilerRuntime::intern_continuation_resume_frame() {
    FrameKey key;
    key.kind = FrameKind::ContinuationResume;
    key.qualified_name = kContinuationResumeFrame;
    return interner_.intern(key);
}

FrameId ProfilerRuntime::intern_user_region_frame(const std::string_view name) {
    FrameKey key;
    key.kind = FrameKind::UserRegion;
    key.qualified_name = name.empty() ? std::string(kUserRegionFrame) : std::string(name);
    return interner_.intern(key);
}

void ProfilerRuntime::enter_frame(const FrameId frame_id) {
    if (!enabled_.load(std::memory_order_relaxed)) return;

    auto& state = current_thread_state();
    const auto now = ::eta::runtime::prof::now_ns();
    std::lock_guard state_lock(state.mutex);
    enter_frame_locked(state, frame_id, now);
    maybe_capture_sample_locked(state, now);

    std::lock_guard phase0_lock(phase0_mutex_);
    phase0_shadow_depth_ = state.stack.size();
}

void ProfilerRuntime::leave_frame() {
    if (!enabled_.load(std::memory_order_relaxed)) return;

    auto& state = current_thread_state();
    const auto now = ::eta::runtime::prof::now_ns();
    std::lock_guard state_lock(state.mutex);
    leave_frame_locked(state, now);
    maybe_capture_sample_locked(state, now);

    std::lock_guard phase0_lock(phase0_mutex_);
    phase0_shadow_depth_ = state.stack.size();
}

void ProfilerRuntime::unwind_to_depth(const std::size_t target_depth) {
    auto& state = current_thread_state();
    const auto now = ::eta::runtime::prof::now_ns();
    std::lock_guard state_lock(state.mutex);

    while (state.stack.size() > target_depth) {
        leave_frame_locked(state, now);
    }
    while (state.stack.size() < target_depth) {
        enter_frame_locked(state, intern_continuation_resume_frame(), now);
    }
    maybe_capture_sample_locked(state, now);

    std::lock_guard phase0_lock(phase0_mutex_);
    phase0_shadow_depth_ = state.stack.size();
}

void ProfilerRuntime::maybe_capture_sample(ThreadState& state) {
    if (!sampling_active_.load(std::memory_order_relaxed)) return;
    const auto now = ::eta::runtime::prof::now_ns();
    std::lock_guard state_lock(state.mutex);
    maybe_capture_sample_locked(state, now);
}

void ProfilerRuntime::maybe_capture_sample_locked(ThreadState& state, const std::uint64_t now_ns_value) {
    if (!sampling_active_.load(std::memory_order_relaxed)) return;
    const auto epoch = sample_epoch_.load(std::memory_order_relaxed);
    if (epoch == 0 || epoch == state.last_sample_epoch) return;

    state.last_sample_epoch = epoch;
    SampleRecord sample;
    sample.timestamp_ns = now_ns_value;
    sample.stack_frame_ids.reserve(state.stack.size());
    for (const auto& frame : state.stack) {
        sample.stack_frame_ids.push_back(frame.frame_id);
    }
    (void) state.samples.push(std::move(sample));
}

void ProfilerRuntime::enter_frame_locked(ThreadState& state,
                                         const FrameId frame_id,
                                         const std::uint64_t now_ns_value) {
    state.stack.push_back(ShadowFrame{
        .frame_id = frame_id,
        .enter_ns = now_ns_value,
        .child_ns = 0,
    });
}

void ProfilerRuntime::leave_frame_locked(ThreadState& state, const std::uint64_t now_ns_value) {
    if (state.stack.empty()) return;

    const ShadowFrame frame = state.stack.back();
    state.stack.pop_back();

    const auto inclusive_ns = elapsed_ns(frame.enter_ns, now_ns_value);
    const auto self_ns = (inclusive_ns >= frame.child_ns) ? (inclusive_ns - frame.child_ns) : 0;

    if (!trace_events_enabled_.load(std::memory_order_relaxed)) {
        return;
    }

    aggregator_.record_flat(frame.frame_id, self_ns, inclusive_ns, 1);
    if (!state.stack.empty()) {
        auto& parent = state.stack.back();
        aggregator_.record_edge(parent.frame_id, frame.frame_id, inclusive_ns, 1);
        parent.child_ns += inclusive_ns;
    }
}

std::uint32_t ProfilerRuntime::clamp_sample_hz(std::uint32_t sample_hz) noexcept {
    if (sample_hz == 0) return kDefaultSampleHz;
    if (sample_hz > kMaxSampleHz) return kMaxSampleHz;
    return sample_hz;
}

void ProfilerRuntime::clear_sample_profiles_locked() {
    std::lock_guard profiles_lock(sample_profiles_mutex_);
    sampled_profiles_.clear();
}

void ProfilerRuntime::reset_sampling_state_for_live_threads() {
    for (const auto& state : live_thread_states()) {
        std::lock_guard state_lock(state->mutex);
        state->samples.clear();
        state->last_sample_epoch = 0;
    }
}

void ProfilerRuntime::drain_sample_buffers() {
    const auto states = live_thread_states();
    std::lock_guard profiles_lock(sample_profiles_mutex_);

    for (const auto& state : states) {
        SampleRecord record;
        while (state->samples.try_pop(record)) {
            auto& profile = sampled_profiles_[state->thread_id];
            if (profile.name.empty()) {
                profile.name = "thread-" + std::to_string(state->thread_id);
            }
            profile.timestamps_ns.push_back(record.timestamp_ns);
            profile.samples.push_back(std::move(record.stack_frame_ids));
        }
    }
}

void ProfilerRuntime::sampler_loop() {
    const auto period_ns = (std::max)(
        std::uint64_t{1},
        std::uint64_t{1'000'000'000} / static_cast<std::uint64_t>(sample_hz_));
    const auto period = std::chrono::nanoseconds(period_ns);

    std::unique_lock sampler_lock(sampler_mutex_);
    while (true) {
        if (sampler_cv_.wait_for(
                sampler_lock,
                period,
                [this] { return sampler_stop_requested_; })) {
            break;
        }

        sampler_lock.unlock();
        sample_epoch_.fetch_add(1, std::memory_order_relaxed);
        drain_sample_buffers();
        sampler_lock.lock();
    }
}

void ProfilerRuntime::start_sampler_locked(const std::uint32_t sample_hz) {
    stop_sampler_locked();
    {
        std::lock_guard sampler_lock(sampler_mutex_);
        sample_hz_ = sample_hz;
        sampler_stop_requested_ = false;
    }
    sampler_thread_ = std::thread([this] { sampler_loop(); });
}

void ProfilerRuntime::stop_sampler_locked() {
    {
        std::lock_guard sampler_lock(sampler_mutex_);
        sampler_stop_requested_ = true;
    }
    sampler_cv_.notify_all();
    if (sampler_thread_.joinable()) {
        sampler_thread_.join();
    }
}

ScopedPrimitiveCall::ScopedPrimitiveCall(const char* builtin_name) {
    auto& profiler = runtime_profiler();
    if (!profiler.enabled()) return;
    active_ = true;
    profiler.on_primitive_enter(builtin_name);
}

ScopedPrimitiveCall::~ScopedPrimitiveCall() {
    if (!active_) return;
    runtime_profiler().on_primitive_exit();
}

} ///< namespace eta::runtime::prof
