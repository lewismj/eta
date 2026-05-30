#include <boost/test/unit_test.hpp>

#include <atomic>
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <limits>
#include <optional>
#include <sstream>
#include <span>
#include <string>
#include <string_view>
#include <vector>
#ifdef _WIN32
#include <process.h>
#endif

#include "eta/runtime/actor/actor_system.h"

namespace {

namespace fs = std::filesystem;

using Clock = std::chrono::steady_clock;
using DurationMs = std::chrono::milliseconds;
using DurationUs = std::chrono::microseconds;
using eta::runtime::actor::ActorSystem;

#ifndef ETA_ETAI_PATH
#define ETA_ETAI_PATH ""
#endif

#ifndef ETA_STDLIB_DIR
#define ETA_STDLIB_DIR ""
#endif

struct ScopedEnvVar {
    std::string key;
    std::optional<std::string> original;

    ScopedEnvVar(std::string key_in, std::string value)
        : key(std::move(key_in)) {
        if (const char* existing = std::getenv(key.c_str())) {
            original = std::string(existing);
        }
        set(value);
    }

    ~ScopedEnvVar() {
        if (original.has_value()) {
            set(*original);
        } else {
            clear();
        }
    }

private:
    void set(const std::string& value) const {
#ifdef _WIN32
        _putenv_s(key.c_str(), value.c_str());
#else
        setenv(key.c_str(), value.c_str(), 1);
#endif
    }

    void clear() const {
#ifdef _WIN32
        _putenv_s(key.c_str(), "");
#else
        unsetenv(key.c_str());
#endif
    }
};

[[nodiscard]] bool parse_bool_env_flag(const char* name, bool default_value = false) {
    const char* value = std::getenv(name);
    if (!value || value[0] == '\0') return default_value;

    std::string normalized(value);
    for (auto& ch : normalized) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    if (normalized == "1" || normalized == "true" || normalized == "yes" || normalized == "on") {
        return true;
    }
    if (normalized == "0" || normalized == "false" || normalized == "no" || normalized == "off") {
        return false;
    }
    return default_value;
}

[[nodiscard]] std::size_t parse_size_t_env(const char* name, std::size_t default_value) {
    const char* value = std::getenv(name);
    if (!value || value[0] == '\0') return default_value;

    char* end = nullptr;
    errno = 0;
    const auto parsed = std::strtoull(value, &end, 10);
    if (end == value || errno == ERANGE) return default_value;
    if (end && *end != '\0') return default_value;
    if (parsed == 0ULL) return default_value;

    constexpr auto kMaxSizeT = static_cast<unsigned long long>(
        (std::numeric_limits<std::size_t>::max)());
    if (parsed > kMaxSizeT) return default_value;
    return static_cast<std::size_t>(parsed);
}

[[nodiscard]] ActorSystem::SchedulerMode parse_scheduler_mode_env() {
    const char* raw = std::getenv("ETA_ACTOR_PI_BENCH_SCHEDULER");
    if (!raw || raw[0] == '\0') return ActorSystem::SchedulerMode::Pool;

    std::string mode(raw);
    for (auto& ch : mode) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    if (mode == "thread-per-actor") return ActorSystem::SchedulerMode::ThreadPerActor;
    if (mode == "pool-shadow") return ActorSystem::SchedulerMode::PoolShadow;
    return ActorSystem::SchedulerMode::Pool;
}

[[nodiscard]] std::string scheduler_mode_name(ActorSystem::SchedulerMode mode) {
    switch (mode) {
        case ActorSystem::SchedulerMode::ThreadPerActor:
            return "thread-per-actor";
        case ActorSystem::SchedulerMode::Pool:
            return "pool";
        case ActorSystem::SchedulerMode::PoolShadow:
            return "pool-shadow";
    }
    return "unknown";
}

[[nodiscard]] std::uint64_t splitmix64(std::uint64_t& state) {
    std::uint64_t z = (state += 0x9E3779B97F4A7C15ULL);
    z = (z ^ (z >> 30U)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27U)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31U);
}

[[nodiscard]] double unit_from_u64(std::uint64_t bits) {
    constexpr double kInv53 = 1.0 / 9007199254740992.0; // 2^53
    return static_cast<double>(bits >> 11U) * kInv53;
}

struct PiShard {
    std::uint64_t inside{0};
    std::uint64_t samples{0};
};

[[nodiscard]] PiShard run_pi_shard(std::uint64_t seed, std::uint64_t samples) {
    std::uint64_t rng = (seed == 0) ? 1 : seed;
    PiShard shard{};
    shard.samples = samples;
    for (std::uint64_t i = 0; i < samples; ++i) {
        const double x = unit_from_u64(splitmix64(rng));
        const double y = unit_from_u64(splitmix64(rng));
        const double d2 = (x * x) + (y * y);
        if (d2 <= 1.0) ++shard.inside;
    }
    return shard;
}

[[nodiscard]] double estimate_pi(const PiShard& shard) {
    if (shard.samples == 0) return 0.0;
    return 4.0 * (static_cast<double>(shard.inside) / static_cast<double>(shard.samples));
}

void encode_u64_le(std::vector<std::uint8_t>& out, std::uint64_t value) {
    for (int i = 0; i < 8; ++i) {
        out.push_back(static_cast<std::uint8_t>((value >> (i * 8)) & 0xFFu));
    }
}

[[nodiscard]] std::optional<std::uint64_t> decode_u64_le(
    std::span<const std::uint8_t> payload,
    std::size_t offset) {
    if (offset + 8 > payload.size()) return std::nullopt;
    std::uint64_t value = 0;
    for (int i = 0; i < 8; ++i) {
        value |= static_cast<std::uint64_t>(payload[offset + static_cast<std::size_t>(i)])
            << (i * 8);
    }
    return value;
}

struct ActorPiRun {
    PiShard totals{};
    DurationMs spawn_ms{0};
    DurationMs dispatch_ms{0};
    DurationMs collect_ms{0};
    DurationMs join_ms{0};
    DurationMs total_ms{0};
    std::size_t spawned_workers{0};
    std::vector<std::uint64_t> worker_elapsed_us{};
    std::vector<std::uint64_t> worker_samples{};
};

[[nodiscard]] std::vector<std::uint64_t> build_shards(
    std::uint64_t total_samples,
    std::size_t requested_workers,
    std::uint64_t max_shard_size) {
    if (total_samples == 0) return {};
    const auto workers = std::max<std::size_t>(
        1,
        std::min<std::size_t>(requested_workers, static_cast<std::size_t>(total_samples)));
    const std::uint64_t base = total_samples / static_cast<std::uint64_t>(workers);
    const std::uint64_t extra = total_samples % static_cast<std::uint64_t>(workers);

    std::vector<std::uint64_t> shards;
    shards.reserve(workers);
    for (std::size_t i = 0; i < workers; ++i) {
        std::uint64_t chunk = base + ((i < extra) ? 1ULL : 0ULL);
        if (max_shard_size == 0 || chunk <= max_shard_size) {
            shards.push_back(chunk);
            continue;
        }
        while (chunk > max_shard_size) {
            shards.push_back(max_shard_size);
            chunk -= max_shard_size;
        }
        if (chunk > 0) shards.push_back(chunk);
    }
    return shards;
}

[[nodiscard]] ActorPiRun run_actor_pi_for_shards(
    const std::vector<std::uint64_t>& shard_samples,
    ActorSystem::SchedulerMode mode,
    DurationMs per_message_timeout) {
    ActorPiRun run{};
    ActorSystem actor_system;
    actor_system.set_scheduler_mode(mode);

    auto controller_pid = actor_system.register_current_thread_actor();
    BOOST_REQUIRE(controller_pid.has_value());

    std::vector<eta::runtime::types::Pid> worker_pids;
    worker_pids.reserve(shard_samples.size());

    const auto t_total_start = Clock::now();

    const auto t_spawn_start = Clock::now();
    for (std::size_t worker_index = 0; worker_index < shard_samples.size(); ++worker_index) {
        const std::uint64_t worker_seed = 0xA5A5A5A500000000ULL
            ^ static_cast<std::uint64_t>(worker_index + 1U) * 0x9E3779B97F4A7C15ULL;
        auto pid = actor_system.spawn(
            [&actor_system, controller = *controller_pid, worker_seed](
                const eta::runtime::types::Pid& self_pid) {
                auto msg = actor_system.receive(self_pid, std::nullopt);
                if (!msg.has_value()) return;
                if (msg->kind != ActorSystem::Message::Kind::Payload) return;

                auto requested = decode_u64_le(msg->payload, 0);
                if (!requested.has_value()) return;

                const auto t_work_start = Clock::now();
                const auto shard = run_pi_shard(worker_seed, *requested);
                const auto t_work_end = Clock::now();
                const auto elapsed_us =
                    std::chrono::duration_cast<DurationUs>(t_work_end - t_work_start).count();

                ActorSystem::BinaryMessage reply;
                reply.reserve(24);
                encode_u64_le(reply, shard.inside);
                encode_u64_le(reply, shard.samples);
                encode_u64_le(reply, static_cast<std::uint64_t>(elapsed_us));
                (void)actor_system.send(controller, std::move(reply));
            });
        BOOST_REQUIRE(pid.has_value());
        worker_pids.push_back(*pid);
    }
    const auto t_spawn_end = Clock::now();
    run.spawned_workers = worker_pids.size();

    const auto t_dispatch_start = Clock::now();
    for (std::size_t i = 0; i < worker_pids.size(); ++i) {
        ActorSystem::BinaryMessage payload;
        payload.reserve(8);
        encode_u64_le(payload, shard_samples[i]);
        BOOST_REQUIRE(actor_system.send(worker_pids[i], std::move(payload)));
    }
    const auto t_dispatch_end = Clock::now();

    run.worker_elapsed_us.reserve(worker_pids.size());
    run.worker_samples.reserve(worker_pids.size());

    const auto t_collect_start = Clock::now();
    for (std::size_t i = 0; i < worker_pids.size(); ++i) {
        auto msg = actor_system.receive(*controller_pid, per_message_timeout);
        BOOST_REQUIRE_MESSAGE(msg.has_value(), "timed out waiting for worker result");
        BOOST_REQUIRE(msg->kind == ActorSystem::Message::Kind::Payload);
        BOOST_REQUIRE_MESSAGE(msg->payload.size() >= 24, "malformed worker payload");

        auto inside = decode_u64_le(msg->payload, 0);
        auto samples = decode_u64_le(msg->payload, 8);
        auto elapsed_us = decode_u64_le(msg->payload, 16);
        BOOST_REQUIRE(inside.has_value());
        BOOST_REQUIRE(samples.has_value());
        BOOST_REQUIRE(elapsed_us.has_value());

        run.totals.inside += *inside;
        run.totals.samples += *samples;
        run.worker_elapsed_us.push_back(*elapsed_us);
        run.worker_samples.push_back(*samples);
    }
    const auto t_collect_end = Clock::now();

    const auto t_join_start = Clock::now();
    actor_system.shutdown();
    const auto t_join_end = Clock::now();
    const auto t_total_end = Clock::now();

    run.spawn_ms = std::chrono::duration_cast<DurationMs>(t_spawn_end - t_spawn_start);
    run.dispatch_ms = std::chrono::duration_cast<DurationMs>(t_dispatch_end - t_dispatch_start);
    run.collect_ms = std::chrono::duration_cast<DurationMs>(t_collect_end - t_collect_start);
    run.join_ms = std::chrono::duration_cast<DurationMs>(t_join_end - t_join_start);
    run.total_ms = std::chrono::duration_cast<DurationMs>(t_total_end - t_total_start);
    return run;
}

[[nodiscard]] ActorPiRun run_actor_pi_uniform(
    std::size_t workers,
    std::uint64_t total_samples,
    ActorSystem::SchedulerMode mode,
    DurationMs per_message_timeout) {
    return run_actor_pi_for_shards(
        build_shards(total_samples, workers, 0),
        mode,
        per_message_timeout);
}

[[nodiscard]] std::string etai_binary_path() {
    return ETA_ETAI_PATH;
}

[[nodiscard]] std::optional<fs::path> find_repo_root() {
    auto dir = fs::current_path();
    for (;;) {
        const auto marker = dir / "demo" / "blackjack" / "blackjack" / "src" / "blackjack.eta";
        std::error_code ec;
        if (fs::exists(marker, ec) && fs::is_regular_file(marker, ec)) {
            return dir;
        }
        if (dir == dir.root_path()) break;
        dir = dir.parent_path();
    }
    return std::nullopt;
}

[[nodiscard]] std::string join_module_path(std::vector<std::string> parts) {
#ifdef _WIN32
    constexpr char sep = ';';
#else
    constexpr char sep = ':';
#endif
    std::string out;
    for (const auto& part : parts) {
        if (part.empty()) continue;
        if (!out.empty()) out.push_back(sep);
        out += part;
    }
    return out;
}

[[nodiscard]] std::string make_blackjack_module_path(const fs::path& repo_root) {
    std::vector<std::string> parts;

    const auto blackjack_src = repo_root / "demo" / "blackjack" / "blackjack" / "src";
    std::error_code ec;
    if (fs::exists(blackjack_src, ec) && fs::is_directory(blackjack_src, ec)) {
        parts.push_back(blackjack_src.string());
    }

    fs::path stdlib_path = fs::path(ETA_STDLIB_DIR);
    if (stdlib_path.empty() || !fs::exists(stdlib_path, ec)) {
        stdlib_path = repo_root / "stdlib";
    }
    if (fs::exists(stdlib_path, ec) && fs::is_directory(stdlib_path, ec)) {
        parts.push_back(stdlib_path.string());
    }

    const auto packages = repo_root / "packages";
    if (fs::exists(packages, ec) && fs::is_directory(packages, ec)) {
        parts.push_back(packages.string());
    }

    return join_module_path(std::move(parts));
}

struct TempFileGuard {
    fs::path path;

    explicit TempFileGuard(fs::path p)
        : path(std::move(p))
    {}

    ~TempFileGuard() {
        std::error_code ec;
        fs::remove(path, ec);
    }

    TempFileGuard(const TempFileGuard&) = delete;
    TempFileGuard& operator=(const TempFileGuard&) = delete;
};

[[nodiscard]] fs::path unique_temp_eta_path() {
    static std::atomic<std::uint64_t> sequence{0};
    const auto now_ns = static_cast<std::uint64_t>(
        std::chrono::high_resolution_clock::now().time_since_epoch().count());
    const auto seq = sequence.fetch_add(1, std::memory_order_relaxed);
    return fs::temp_directory_path()
        / ("eta_blackjack_crash_probe_" + std::to_string(now_ns) + "_"
            + std::to_string(seq) + ".eta");
}

[[nodiscard]] bool write_blackjack_probe_module(
    const fs::path& script_path,
    std::size_t rounds,
    std::size_t workers) {
    std::ofstream out(script_path, std::ios::out | std::ios::binary);
    if (!out) return false;
    out << "(module blackjack.crash.probe\n"
           "  (import blackjack)\n"
           "  (begin\n"
           "    (define cfg (make-mc-shoe 27))\n"
           "    (define _profile (simulate-profiled policy-hit-below-17 cfg "
        << rounds << " " << workers << "))\n"
           "    0))\n";
    return static_cast<bool>(out);
}

[[nodiscard]] int run_etai_script(
    const fs::path& etai_path,
    const std::string& module_path,
    const fs::path& script_path) {
#ifdef _WIN32
    std::vector<std::wstring> args;
    args.reserve(5);
    args.push_back(etai_path.wstring());
    args.emplace_back(L"--path");
    args.push_back(fs::path(module_path).wstring());
    args.push_back(script_path.wstring());

    std::vector<const wchar_t*> argv;
    argv.reserve(args.size() + 1);
    for (const auto& arg : args) argv.push_back(arg.c_str());
    argv.push_back(nullptr);

    errno = 0;
    const int rc = _wspawnvp(_P_WAIT, args.front().c_str(), argv.data());
    if (rc == -1) {
        return -errno;
    }
    return rc;
#else
    auto shell_quote = [](const std::string& s) {
        std::string quoted = "'";
        for (char c : s) {
            if (c == '\'') quoted += "'\\''";
            else quoted.push_back(c);
        }
        quoted.push_back('\'');
        return quoted;
    };
    const std::string cmd =
        shell_quote(etai_path.string())
        + " --path " + shell_quote(module_path)
        + " " + shell_quote(script_path.string());
    return std::system(cmd.c_str());
#endif
}

} // namespace

BOOST_AUTO_TEST_SUITE(actor_monte_carlo_perf_tests)

BOOST_AUTO_TEST_CASE(actor_monte_carlo_pi_actor_vs_baseline_small_correctness) {
    const std::uint64_t samples = 100000;
    const std::size_t workers = 8;

    const auto t_baseline_start = Clock::now();
    const auto baseline = run_pi_shard(0x0123456789ABCDEFULL, samples);
    const auto t_baseline_end = Clock::now();
    const auto baseline_ms =
        std::chrono::duration_cast<DurationMs>(t_baseline_end - t_baseline_start);

    auto actor_run = run_actor_pi_uniform(
        workers,
        samples,
        ActorSystem::SchedulerMode::Pool,
        std::chrono::seconds(30));

    const double pi_baseline = estimate_pi(baseline);
    const double pi_actor = estimate_pi(actor_run.totals);
    constexpr double kTolerance = 0.03;

    BOOST_TEST(std::abs(pi_baseline - 3.141592653589793) < kTolerance);
    BOOST_TEST(std::abs(pi_actor - 3.141592653589793) < kTolerance);
    BOOST_TEST(actor_run.totals.samples == samples);

    BOOST_TEST_MESSAGE(
        "pi-mc-small baseline-ms=" << baseline_ms.count()
        << " actor-ms=" << actor_run.total_ms.count()
        << " actor-workers=" << actor_run.spawned_workers
        << " spawn-ms=" << actor_run.spawn_ms.count()
        << " dispatch-ms=" << actor_run.dispatch_ms.count()
        << " collect-ms=" << actor_run.collect_ms.count()
        << " join-ms=" << actor_run.join_ms.count()
        << " pi-baseline=" << pi_baseline
        << " pi-actor=" << pi_actor);
}

BOOST_AUTO_TEST_CASE(actor_monte_carlo_pi_breakdown_opt_in) {
    if (!parse_bool_env_flag("ETA_ACTOR_PI_BENCH_ENABLE", false)) {
        BOOST_TEST_MESSAGE(
            "ETA_ACTOR_PI_BENCH_ENABLE not set; skipping opt-in pi Monte Carlo actor benchmark");
        return;
    }

    const std::uint64_t samples = static_cast<std::uint64_t>(parse_size_t_env(
        "ETA_ACTOR_PI_BENCH_SAMPLES",
        500000u));
    const std::size_t workers_requested = parse_size_t_env("ETA_ACTOR_PI_BENCH_WORKERS", 16u);
    const std::uint64_t max_shard_size = static_cast<std::uint64_t>(parse_size_t_env(
        "ETA_ACTOR_PI_BENCH_MAX_SHARD",
        500u));
    const auto timeout_ms = parse_size_t_env("ETA_ACTOR_PI_BENCH_TIMEOUT_MS", 300000u);
    const auto mode = parse_scheduler_mode_env();

    BOOST_REQUIRE_MESSAGE(workers_requested > 0, "workers must be > 0");

    const auto t_baseline_start = Clock::now();
    const auto baseline = run_pi_shard(0x0123456789ABCDEFULL, samples);
    const auto t_baseline_end = Clock::now();
    const auto baseline_ms =
        std::chrono::duration_cast<DurationMs>(t_baseline_end - t_baseline_start);

    auto actor_compact = run_actor_pi_uniform(
        workers_requested,
        samples,
        mode,
        DurationMs(static_cast<std::int64_t>(timeout_ms)));
    auto actor_sharded = run_actor_pi_for_shards(
        build_shards(samples, workers_requested, max_shard_size),
        mode,
        DurationMs(static_cast<std::int64_t>(timeout_ms)));

    const double pi_baseline = estimate_pi(baseline);
    const double pi_actor_compact = estimate_pi(actor_compact.totals);
    const double pi_actor_sharded = estimate_pi(actor_sharded.totals);
    const auto max_worker_us_it = std::max_element(
        actor_sharded.worker_elapsed_us.begin(),
        actor_sharded.worker_elapsed_us.end());
    const std::uint64_t max_worker_us = (max_worker_us_it == actor_sharded.worker_elapsed_us.end())
        ? 0ULL
        : *max_worker_us_it;
    const auto max_worker_ms =
        static_cast<std::uint64_t>(std::chrono::duration_cast<DurationMs>(
                                       DurationUs(max_worker_us))
                                       .count());
    const auto total_us = static_cast<std::int64_t>(actor_sharded.total_ms.count()) * 1000LL;
    const auto overhead_us_estimate =
        (total_us > static_cast<std::int64_t>(max_worker_us))
        ? (total_us - static_cast<std::int64_t>(max_worker_us))
        : 0LL;
    const auto overhead_ms_estimate =
        (actor_sharded.total_ms.count() > static_cast<std::int64_t>(max_worker_ms))
        ? (actor_sharded.total_ms.count() - static_cast<std::int64_t>(max_worker_ms))
        : 0;
    const auto baseline_us =
        std::chrono::duration_cast<DurationUs>(t_baseline_end - t_baseline_start).count();
    const double speedup_compact = actor_compact.total_ms.count() > 0
        ? (static_cast<double>(baseline_us) / 1000.0)
            / static_cast<double>(actor_compact.total_ms.count())
        : 0.0;
    const double speedup_sharded = actor_sharded.total_ms.count() > 0
        ? (static_cast<double>(baseline_us) / 1000.0)
            / static_cast<double>(actor_sharded.total_ms.count())
        : 0.0;

    BOOST_TEST(actor_compact.totals.samples == samples);
    BOOST_TEST(actor_sharded.totals.samples == samples);
    BOOST_TEST(std::abs(pi_baseline - 3.141592653589793) < 0.02);
    BOOST_TEST(std::abs(pi_actor_compact - 3.141592653589793) < 0.02);
    BOOST_TEST(std::abs(pi_actor_sharded - 3.141592653589793) < 0.02);

    BOOST_TEST_MESSAGE(
        "pi-mc config: samples=" << samples
        << " workers-requested=" << workers_requested
        << " scheduler=" << scheduler_mode_name(mode)
        << " max-shard=" << max_shard_size);
    BOOST_TEST_MESSAGE(
        "pi-mc baseline-us=" << baseline_us
        << " baseline-ms=" << baseline_ms.count()
        << " baseline-pi=" << pi_baseline);
    BOOST_TEST_MESSAGE(
        "pi-mc compact-workers actor-total-ms=" << actor_compact.total_ms.count()
        << " actor-workers=" << actor_compact.spawned_workers
        << " speedup-baseline-over-actor=" << speedup_compact);
    BOOST_TEST_MESSAGE(
        "pi-mc compact-breakdown-ms: spawn=" << actor_compact.spawn_ms.count()
        << " dispatch=" << actor_compact.dispatch_ms.count()
        << " collect=" << actor_compact.collect_ms.count()
        << " join=" << actor_compact.join_ms.count());
    BOOST_TEST_MESSAGE(
        "pi-mc sharded-workers actor-total-ms=" << actor_sharded.total_ms.count()
        << " actor-workers=" << actor_sharded.spawned_workers
        << " speedup-baseline-over-actor=" << speedup_sharded);
    BOOST_TEST_MESSAGE(
        "pi-mc sharded-breakdown-ms: spawn=" << actor_sharded.spawn_ms.count()
        << " dispatch=" << actor_sharded.dispatch_ms.count()
        << " collect=" << actor_sharded.collect_ms.count()
        << " join=" << actor_sharded.join_ms.count());
    BOOST_TEST_MESSAGE(
        "pi-mc actor-worker-max-ms=" << max_worker_ms
        << " actor-worker-max-us=" << max_worker_us
        << " actor-overhead-estimate-us=" << overhead_us_estimate
        << " actor-overhead-estimate-ms=" << overhead_ms_estimate);
}

BOOST_AUTO_TEST_CASE(actor_blackjack_profiled_crash_probe_opt_in) {
    if (!parse_bool_env_flag("ETA_BLACKJACK_CRASH_PROBE_ENABLE", false)) {
        BOOST_TEST_MESSAGE(
            "ETA_BLACKJACK_CRASH_PROBE_ENABLE not set; skipping opt-in blackjack crash probe");
        return;
    }

    const auto etai_raw = etai_binary_path();
    BOOST_REQUIRE_MESSAGE(!etai_raw.empty(), "ETA_ETAI_PATH is empty");
    if (etai_raw.find("$<") != std::string::npos) {
        BOOST_TEST_MESSAGE("ETA_ETAI_PATH is generator expression-like; skipping crash probe");
        return;
    }

    fs::path etai_path(etai_raw);
    std::error_code ec;
    if (!etai_path.is_absolute()) etai_path = fs::absolute(etai_path, ec);
    BOOST_REQUIRE_MESSAGE(
        fs::exists(etai_path, ec) && fs::is_regular_file(etai_path, ec),
        "etai binary not found at " + etai_path.string());

    const auto repo_root = find_repo_root();
    BOOST_REQUIRE_MESSAGE(repo_root.has_value(), "could not locate repo root for blackjack probe");

    const auto module_path = make_blackjack_module_path(*repo_root);
    BOOST_REQUIRE_MESSAGE(!module_path.empty(), "blackjack probe module path is empty");

    auto run_case = [&](std::size_t rounds, std::size_t workers) {
        const auto script_path = unique_temp_eta_path();
        TempFileGuard guard(script_path);
        BOOST_REQUIRE_MESSAGE(
            write_blackjack_probe_module(script_path, rounds, workers),
            "failed to write crash probe script: " + script_path.string());
        return run_etai_script(etai_path, module_path, script_path);
    };

    const int rc_small = run_case(240, 24);
    const int rc_mid = run_case(1000, 2);
    const int rc_repro = run_case(2000, 2);

#ifdef _WIN32
    constexpr int kWindowsStackOverflowExit = static_cast<int>(0xC00000FDu);
    BOOST_TEST_MESSAGE(
        "blackjack crash probe exits: n=240,w=24 -> " << rc_small
        << "; n=1000,w=2 -> " << rc_mid
        << "; n=2000,w=2 -> " << rc_repro
        << "; stack-overflow-exit=" << kWindowsStackOverflowExit);
    BOOST_REQUIRE_MESSAGE(
        rc_mid != kWindowsStackOverflowExit && rc_repro != kWindowsStackOverflowExit,
        "blackjack crash probe hit Windows stack-overflow exit (0xC00000FD)");
#else
    BOOST_TEST_MESSAGE(
        "blackjack crash probe exits: n=240,w=24 -> " << rc_small
        << "; n=1000,w=2 -> " << rc_mid
        << "; n=2000,w=2 -> " << rc_repro);
#endif

    BOOST_REQUIRE_MESSAGE(rc_small == 0, "small blackjack profiled probe should succeed");
    BOOST_REQUIRE_MESSAGE(rc_mid == 0, "mid-size blackjack profiled probe should succeed");
    BOOST_REQUIRE_MESSAGE(rc_repro == 0, "repro blackjack profiled probe should succeed");
}

BOOST_AUTO_TEST_SUITE_END()
