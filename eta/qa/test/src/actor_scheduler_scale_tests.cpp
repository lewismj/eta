#include <boost/test/unit_test.hpp>

#include <cerrno>
#include <chrono>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <limits>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>

#ifdef _WIN32
#include <windows.h>
#include <psapi.h>
#include <tlhelp32.h>
#pragma comment(lib, "Psapi.lib")
#elif defined(__linux__)
#include <unistd.h>
#endif

#include "eta/interpreter/module_path.h"
#include "eta/runtime/nanbox.h"
#include "eta/session/driver.h"

namespace fs = std::filesystem;

#ifndef ETA_STDLIB_DIR
#define ETA_STDLIB_DIR ""
#endif

namespace {

fs::path stdlib_dir() {
    fs::path p(ETA_STDLIB_DIR);
    if (!p.empty() && fs::is_directory(p)) return p;

    const auto cwd = fs::current_path();
    for (const auto& candidate : {
        cwd / "stdlib",
        cwd / ".." / "stdlib",
        cwd / ".." / ".." / "stdlib",
        cwd / ".." / ".." / ".." / "stdlib",
    }) {
        if (fs::is_directory(candidate)) return fs::canonical(candidate);
    }
    return {};
}

eta::interpreter::ModulePathResolver make_resolver() {
    auto stdlib = stdlib_dir();
    if (stdlib.empty()) return eta::interpreter::ModulePathResolver{};
    return eta::interpreter::ModulePathResolver({stdlib});
}

struct DriverHarness {
    eta::session::Driver driver;

    DriverHarness()
        : driver(make_resolver()) {}

    [[nodiscard]] std::string diagnostics_string() const {
        std::ostringstream oss;
        driver.diagnostics().print_all(oss, /*use_color=*/false, driver.file_resolver());
        return oss.str();
    }

    bool run_source(std::string_view source,
                    eta::runtime::nanbox::LispVal* result = nullptr,
                    const std::string& result_binding = "result") {
        return driver.run_source(source, result, result_binding);
    }

    eta::runtime::nanbox::LispVal run_module(std::string_view source) {
        eta::runtime::nanbox::LispVal result{eta::runtime::nanbox::Nil};
        const bool ok = run_source(source, &result, "result");
        BOOST_REQUIRE_MESSAGE(ok, "run_source failed:\n" + diagnostics_string());
        return result;
    }

    std::int64_t as_int(eta::runtime::nanbox::LispVal value) const {
        auto decoded = eta::runtime::nanbox::ops::decode<std::int64_t>(value);
        BOOST_REQUIRE_MESSAGE(decoded.has_value(), "expected fixnum result");
        return *decoded;
    }

    [[nodiscard]] std::string format_value(eta::runtime::nanbox::LispVal value) {
        return driver.format_value(value, eta::runtime::FormatMode::Write);
    }
};

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

[[nodiscard]] bool parse_bool_env_flag_with_legacy(
    const char* name,
    const char* legacy_name,
    bool default_value = false) {
    const char* value = std::getenv(name);
    if (value && value[0] != '\0') {
        return parse_bool_env_flag(name, default_value);
    }
    if (!legacy_name) return default_value;
    return parse_bool_env_flag(legacy_name, default_value);
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

[[nodiscard]] std::size_t parse_size_t_env_with_legacy(
    const char* name,
    const char* legacy_name,
    std::size_t default_value) {
    const char* value = std::getenv(name);
    if (value && value[0] != '\0') {
        return parse_size_t_env(name, default_value);
    }
    if (!legacy_name) return default_value;
    return parse_size_t_env(legacy_name, default_value);
}

[[nodiscard]] std::optional<std::uint64_t> current_resident_bytes() {
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS_EX counters{};
    counters.cb = static_cast<DWORD>(sizeof(counters));
    if (!GetProcessMemoryInfo(
            GetCurrentProcess(),
            reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters),
            counters.cb)) {
        return std::nullopt;
    }
    return static_cast<std::uint64_t>(counters.WorkingSetSize);
#elif defined(__linux__)
    std::ifstream statm("/proc/self/statm");
    std::uint64_t pages = 0;
    std::uint64_t resident_pages = 0;
    if (!(statm >> pages >> resident_pages)) return std::nullopt;
    const auto page_size = static_cast<std::uint64_t>(::sysconf(_SC_PAGESIZE));
    return resident_pages * page_size;
#else
    return std::nullopt;
#endif
}

[[nodiscard]] std::optional<std::uint64_t> current_thread_count() {
#ifdef _WIN32
    const auto pid = GetCurrentProcessId();
    const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return std::nullopt;

    THREADENTRY32 entry{};
    entry.dwSize = sizeof(entry);

    std::uint64_t count = 0;
    if (Thread32First(snapshot, &entry) != FALSE) {
        do {
            if (entry.th32OwnerProcessID == pid) {
                ++count;
            }
            entry.dwSize = sizeof(entry);
        } while (Thread32Next(snapshot, &entry) != FALSE);
    }
    CloseHandle(snapshot);
    return count;
#elif defined(__linux__)
    std::ifstream status("/proc/self/status");
    if (!status) return std::nullopt;

    std::string line;
    while (std::getline(status, line)) {
        if (!line.starts_with("Threads:")) continue;
        std::istringstream iss(line.substr(std::string("Threads:").size()));
        std::uint64_t count = 0;
        if (iss >> count) return count;
        return std::nullopt;
    }
    return std::nullopt;
#else
    return std::nullopt;
#endif
}

[[nodiscard]] std::string build_active_module_source(
    std::string_view module_name,
    std::size_t pair_count,
    std::size_t rounds,
    std::size_t control_count,
    std::size_t timeout_steps,
    std::size_t ack_slack) {
    std::ostringstream src;
    src
        << "(module " << module_name << "\n"
        << "  (import std.actor)\n"
        << "  (import std.core)\n"
        << "  (export result acks expected-acks min-acks downs exits control-target)\n"
        << "\n"
        << "  (define parent (self))\n"
        << "  (trap-exit! #t)\n"
        << "\n"
        << "  (defun await-message ()\n"
        << "    (let loop ()\n"
        << "      (let ((msg (receive-after 0)))\n"
        << "        (if msg\n"
        << "            msg\n"
        << "            (loop)))))\n"
        << "\n"
        << "  (defun pong-loop ()\n"
        << "    (let loop ()\n"
        << "      (let ((msg (await-message)))\n"
        << "        (cond\n"
        << "          ((eq? msg 'stop) 'ok)\n"
        << "          ((and (pair? msg) (eq? (car msg) 'ping))\n"
        << "           (send parent 'ack)\n"
        << "           (loop))\n"
        << "          (else (loop))))))\n"
        << "\n"
        << "  (defun ping-loop (pong)\n"
        << "    (let loop ()\n"
        << "      (let ((msg (await-message)))\n"
        << "        (cond\n"
        << "          ((eq? msg 'stop) 'ok)\n"
        << "          ((eq? msg 'run)\n"
        << "           (send pong (list 'ping (self)))\n"
        << "           (loop))\n"
        << "          (else (loop))))))\n"
        << "\n"
        << "  (defun spawn-pairs (n ping-acc pong-acc)\n"
        << "    (if (= n 0)\n"
        << "        (list ping-acc pong-acc)\n"
        << "        (let ((pong (spawn (lambda () (pong-loop)))))\n"
        << "          (let ((ping (spawn (lambda () (ping-loop pong)))))\n"
        << "            (spawn-pairs (- n 1)\n"
        << "                         (cons ping ping-acc)\n"
        << "                         (cons pong pong-acc))))))\n"
        << "\n"
        << "  (defun control-worker ()\n"
        << "    (let ((msg (await-message)))\n"
        << "      (cond\n"
        << "        ((non-pair? msg) (kill (self)))\n"
        << "        ((eq? (car msg) 'go) (kill (self)))\n"
        << "        (else (kill (self))))))\n"
        << "\n"
        << "  (defun spawn-control (n acc)\n"
        << "    (if (= n 0)\n"
        << "        acc\n"
        << "        (spawn-control (- n 1)\n"
        << "                       (cons (spawn (lambda () (control-worker))) acc))))\n"
        << "\n"
        << "  (defun arm-control (actors)\n"
        << "    (if (null? actors)\n"
        << "        0\n"
        << "        (begin\n"
        << "          (link (car actors))\n"
        << "          (monitor (car actors))\n"
        << "          (send (car actors) 'go)\n"
        << "          (+ 1 (arm-control (cdr actors))))))\n"
        << "\n"
        << "  (defun send-all (actors payload)\n"
        << "    (if (null? actors)\n"
        << "        #t\n"
        << "        (begin\n"
        << "          (send (car actors) payload)\n"
        << "          (send-all (cdr actors) payload))))\n"
        << "\n"
        << "  (defun run-rounds (n pings)\n"
        << "    (if (= n 0)\n"
        << "        #t\n"
        << "        (begin\n"
        << "          (send-all pings 'run)\n"
        << "          (run-rounds (- n 1) pings))))\n"
        << "\n"
        << "  (defun drain-counts (target-acks target-down target-exit timeout-left acks downs exits)\n"
        << "    (if (or (= timeout-left 0)\n"
        << "            (and (= acks target-acks)\n"
        << "                 (= downs target-down)\n"
        << "                 (= exits target-exit)))\n"
        << "        (list acks downs exits)\n"
        << "        (let ((msg (receive-after 1000)))\n"
        << "          (if msg\n"
        << "              (cond\n"
        << "                ((eq? msg 'ack)\n"
        << "                 (drain-counts target-acks target-down target-exit timeout-left (+ acks 1) downs exits))\n"
        << "                ((and (pair? msg) (eq? (car msg) 'DOWN))\n"
        << "                 (drain-counts target-acks target-down target-exit timeout-left acks (+ downs 1) exits))\n"
        << "                ((and (pair? msg) (eq? (car msg) 'EXIT))\n"
        << "                 (drain-counts target-acks target-down target-exit timeout-left acks downs (+ exits 1)))\n"
        << "                (else\n"
        << "                 (drain-counts target-acks target-down target-exit timeout-left acks downs exits)))\n"
        << "              (drain-counts target-acks target-down target-exit (- timeout-left 1) acks downs exits)))))\n"
        << "\n"
        << "  (define spawned (spawn-pairs " << pair_count << " '() '()))\n"
        << "  (define pings (car spawned))\n"
        << "  (define pongs (car (cdr spawned)))\n"
        << "  (define controls (spawn-control " << control_count << " '()))\n"
        << "  (define control-target (arm-control controls))\n"
        << "\n"
        << "  (run-rounds " << rounds << " pings)\n"
        << "\n"
        << "  (define expected-acks (* " << pair_count << " " << rounds << "))\n"
        << "  (define min-acks (if (> " << ack_slack << " expected-acks)\n"
        << "                        0\n"
        << "                        (- expected-acks " << ack_slack << ")))\n"
        << "  (define counts\n"
        << "    (drain-counts min-acks control-target control-target " << timeout_steps
        << " 0 0 0))\n"
        << "  (define acks (car counts))\n"
        << "  (define downs (car (cdr counts)))\n"
        << "  (define exits (car (cdr (cdr counts))))\n"
        << "\n"
        << "  (send-all pings 'stop)\n"
        << "  (send-all pongs 'stop)\n"
        << "\n"
        << "  (define result\n"
        << "    (and (>= acks min-acks)\n"
        << "         (= downs control-target)\n"
        << "         (= exits control-target)))\n"
        << ")\n";

    return src.str();
}

} // namespace

BOOST_AUTO_TEST_SUITE(actor_scheduler_scale_tests)

BOOST_AUTO_TEST_CASE(pool_idle_scale_holds_thread_and_memory_ceilings) {
    using eta::runtime::actor::ActorSystem;

    ScopedEnvVar scheduler_mode("ETA_ACTOR_SCHEDULER", "pool");
    DriverHarness harness;

    auto actor_system = harness.driver.vm().actor_system();
    BOOST_REQUIRE(actor_system);
    BOOST_TEST(
        static_cast<int>(actor_system->scheduler_mode())
        == static_cast<int>(ActorSystem::SchedulerMode::Pool));

    const bool full_scale = parse_bool_env_flag_with_legacy(
        "ETA_ACTOR_SCALE_FULL_PROFILE",
        "ETA_ACTOR_M7_FULL_SCALE",
        false);
    const auto idle_actors = parse_size_t_env(
        "ETA_ACTOR_IDLE_SCALE_ACTORS",
        full_scale ? 100000u : 4000u);
    const auto thread_ceiling = parse_size_t_env(
        "ETA_ACTOR_IDLE_THREAD_CEILING",
        full_scale ? 512u : 256u);
    const auto memory_ceiling_mb = parse_size_t_env(
        "ETA_ACTOR_IDLE_MEMORY_CEILING_MB",
        full_scale ? 4096u : 768u);

    BOOST_TEST_MESSAGE(
        "actor scheduler idle-scale profile: actors=" << idle_actors
        << ", thread_ceiling=" << thread_ceiling
        << ", memory_ceiling_mb=" << memory_ceiling_mb
        << ", full_scale=" << (full_scale ? "true" : "false"));

    const auto rss_before = current_resident_bytes();

    std::ostringstream spawn_source;
    spawn_source
        << "(module actor.scheduler.scale.idle_state\n"
        << "  (import std.actor)\n"
        << "  (export workers-count stop-workers)\n"
        << "\n"
        << "  (defun idle-worker ()\n"
        << "    (let loop ()\n"
        << "      (let ((msg (receive-after 60000)))\n"
        << "        (if (eq? msg 'stop)\n"
        << "            'ok\n"
        << "            (loop)))))\n"
        << "\n"
        << "  (defun spawn-workers (n acc)\n"
        << "    (if (= n 0)\n"
        << "        acc\n"
        << "        (spawn-workers (- n 1)\n"
        << "                       (cons (spawn (lambda () (idle-worker))) acc))))\n"
        << "\n"
        << "  (define workers (spawn-workers " << idle_actors << " '()))\n"
        << "\n"
        << "  (defun workers-count ()\n"
        << "    (length workers))\n"
        << "\n"
        << "  (defun stop-all (xs)\n"
        << "    (if (null? xs)\n"
        << "        0\n"
        << "        (begin\n"
        << "          (send (car xs) 'stop)\n"
        << "          (+ 1 (stop-all (cdr xs))))))\n"
        << "\n"
        << "  (defun stop-workers ()\n"
        << "    (stop-all workers))\n"
        << ")\n";
    BOOST_REQUIRE_MESSAGE(
        harness.run_source(spawn_source.str()),
        "idle-state module failed:\n" + harness.diagnostics_string());

    auto count_result = harness.run_module(R"eta(
(module actor.scheduler.scale.idle_count
  (import actor.scheduler.scale.idle_state)
  (define result (workers-count)))
)eta");
    const auto worker_count = harness.as_int(count_result);
    BOOST_TEST(static_cast<std::size_t>(worker_count) == idle_actors);

    const auto threads_after_spawn = current_thread_count();
    if (threads_after_spawn.has_value()) {
        BOOST_TEST(*threads_after_spawn <= thread_ceiling);
    } else {
        BOOST_TEST_MESSAGE("thread-count metric unavailable on this platform; skipping ceiling check");
    }

    const auto rss_after_spawn = current_resident_bytes();
    if (rss_before.has_value() && rss_after_spawn.has_value()) {
        const auto delta_bytes =
            (*rss_after_spawn > *rss_before) ? (*rss_after_spawn - *rss_before) : 0ULL;
        const auto ceiling_bytes = static_cast<std::uint64_t>(memory_ceiling_mb) * 1024ULL * 1024ULL;
        BOOST_TEST(delta_bytes <= ceiling_bytes);
    } else {
        BOOST_TEST_MESSAGE("resident-memory metric unavailable on this platform; skipping ceiling check");
    }

    auto stop_result = harness.run_module(R"eta(
(module actor.scheduler.scale.idle_stop
  (import actor.scheduler.scale.idle_state)
  (define result (stop-workers)))
)eta");
    const auto stopped = harness.as_int(stop_result);
    BOOST_TEST(static_cast<std::size_t>(stopped) == idle_actors);
}

BOOST_AUTO_TEST_CASE(pool_active_ping_pong_with_monitor_and_link_traffic) {
    ScopedEnvVar scheduler_mode("ETA_ACTOR_SCHEDULER", "pool");
    DriverHarness harness;

    const bool full_scale = parse_bool_env_flag_with_legacy(
        "ETA_ACTOR_SCALE_FULL_PROFILE",
        "ETA_ACTOR_M7_FULL_SCALE",
        false);
    const auto pair_count = parse_size_t_env(
        "ETA_ACTOR_ACTIVE_SCALE_PAIRS",
        full_scale ? 5000u : 128u);
    const auto rounds = parse_size_t_env(
        "ETA_ACTOR_ACTIVE_SCALE_ROUNDS",
        full_scale ? 2u : 4u);
    const auto control_count = parse_size_t_env(
        "ETA_ACTOR_ACTIVE_SCALE_CONTROL_ACTORS",
        full_scale ? 512u : 32u);
    const auto timeout_steps = parse_size_t_env(
        "ETA_ACTOR_ACTIVE_SCALE_TIMEOUT_STEPS",
        full_scale ? 180u : 60u);
    const auto ack_slack = parse_size_t_env(
        "ETA_ACTOR_ACTIVE_SCALE_ACK_SLACK",
        full_scale ? 0u : 2u);

    BOOST_TEST_MESSAGE(
        "actor scheduler active-scale profile: pairs=" << pair_count
        << ", rounds=" << rounds
        << ", control_actors=" << control_count
        << ", timeout_steps=" << timeout_steps
        << ", ack_slack=" << ack_slack
        << ", full_scale=" << (full_scale ? "true" : "false"));

    const auto source = build_active_module_source(
        "actor.scheduler.scale.active_case",
        pair_count,
        rounds,
        control_count,
        timeout_steps,
        ack_slack);
    auto result = harness.run_module(source);
    if (result != eta::runtime::nanbox::True) {
        eta::runtime::nanbox::LispVal debug_counts{eta::runtime::nanbox::Nil};
        const bool debug_ok = harness.run_source(R"eta(
(module actor.scheduler.scale.active_case_debug
  (import actor.scheduler.scale.active_case)
  (define counts
    (list
      result
      acks expected-acks min-acks
      downs control-target
      exits control-target)))
)eta",
            &debug_counts,
            "counts");
        BOOST_REQUIRE_MESSAGE(
            debug_ok,
            "active-case debug module failed:\n" + harness.diagnostics_string());
        BOOST_TEST_MESSAGE(
            "active-scale counters: " << harness.format_value(debug_counts));
    }
    BOOST_TEST(result == eta::runtime::nanbox::True);
}

BOOST_AUTO_TEST_CASE(pool_soak_opt_in_non_pr_lane) {
    ScopedEnvVar scheduler_mode("ETA_ACTOR_SCHEDULER", "pool");

    const auto soak_seconds = parse_size_t_env_with_legacy(
        "ETA_ACTOR_SCALE_SOAK_SECONDS",
        "ETA_ACTOR_M7_SOAK_SECONDS",
        0u);
    if (soak_seconds == 0u) {
        BOOST_TEST_MESSAGE(
            "ETA_ACTOR_SCALE_SOAK_SECONDS not set; skipping optional soak test");
        return;
    }

    const auto pair_count = parse_size_t_env_with_legacy(
        "ETA_ACTOR_SCALE_SOAK_PAIRS",
        "ETA_ACTOR_M7_SOAK_PAIRS",
        128u);
    const auto rounds = parse_size_t_env_with_legacy(
        "ETA_ACTOR_SCALE_SOAK_ROUNDS",
        "ETA_ACTOR_M7_SOAK_ROUNDS",
        2u);
    const auto control_count = parse_size_t_env_with_legacy(
        "ETA_ACTOR_SCALE_SOAK_CONTROL_ACTORS",
        "ETA_ACTOR_M7_SOAK_CONTROL_ACTORS",
        16u);
    const auto timeout_steps = parse_size_t_env_with_legacy(
        "ETA_ACTOR_SCALE_SOAK_TIMEOUT_STEPS",
        "ETA_ACTOR_M7_SOAK_TIMEOUT_STEPS",
        60u);

    DriverHarness harness;
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(soak_seconds);

    std::size_t iterations = 0;
    while (std::chrono::steady_clock::now() < deadline) {
        std::ostringstream module_name;
        module_name << "actor.scheduler.scale.soak_case_" << iterations;

        const auto source = build_active_module_source(
            module_name.str(),
            pair_count,
            rounds,
            control_count,
            timeout_steps,
            0u);
        auto result = harness.run_module(source);
        BOOST_REQUIRE(result == eta::runtime::nanbox::True);
        ++iterations;
    }

    BOOST_TEST(iterations > 0u);
}

BOOST_AUTO_TEST_SUITE_END()
