#include <boost/test/unit_test.hpp>

#include <array>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <sstream>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>

#include "eta/interpreter/module_path.h"
#include "eta/nng/wire_format.h"
#include "eta/runtime/actor/actor_system.h"
#include "eta/runtime/factory.h"
#include "eta/runtime/memory/heap.h"
#include "eta/runtime/memory/intern_table.h"
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

struct ActorHarness {
    eta::session::Driver driver;

    ActorHarness() : driver(make_resolver()) {}

    [[nodiscard]] std::string diagnostics_string() const {
        std::ostringstream oss;
        driver.diagnostics().print_all(oss, /*use_color=*/false, driver.file_resolver());
        return oss.str();
    }

    eta::runtime::nanbox::LispVal run_module(std::string_view source) {
        eta::runtime::nanbox::LispVal result{eta::runtime::nanbox::Nil};
        const bool ok = driver.run_source(source, &result, "result");
        BOOST_REQUIRE_MESSAGE(ok, "run_source failed:\n" + diagnostics_string());
        return result;
    }

    std::int64_t as_int(eta::runtime::nanbox::LispVal value) const {
        auto decoded = eta::runtime::nanbox::ops::decode<std::int64_t>(value);
        BOOST_REQUIRE_MESSAGE(decoded.has_value(), "expected fixnum result");
        return *decoded;
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

[[nodiscard]] std::string unique_inproc_endpoint() {
    const auto stamp =
        std::chrono::steady_clock::now().time_since_epoch().count();
    return "inproc://eta_actor_distribution_" + std::to_string(stamp);
}

template <typename Predicate>
[[nodiscard]] bool wait_until(
    Predicate&& predicate,
    std::chrono::milliseconds timeout = std::chrono::milliseconds(1500)) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return predicate();
}

std::vector<std::uint8_t> encode_fixnum_payload(
    eta::runtime::memory::heap::Heap& heap,
    eta::runtime::memory::intern::InternTable& intern_table,
    std::int64_t value) {
    auto fixnum = eta::runtime::memory::factory::make_fixnum(heap, value);
    BOOST_REQUIRE(fixnum.has_value());
    auto payload = eta::nng::serialize_binary_strict(*fixnum, heap, intern_table);
    BOOST_REQUIRE(payload.has_value());
    return std::move(*payload);
}

std::vector<std::uint8_t> encode_symbol_payload(
    eta::runtime::memory::heap::Heap& heap,
    eta::runtime::memory::intern::InternTable& intern_table,
    std::string_view symbol_name) {
    auto symbol = eta::runtime::memory::factory::make_symbol(intern_table, std::string(symbol_name));
    BOOST_REQUIRE(symbol.has_value());
    auto payload = eta::nng::serialize_binary_strict(*symbol, heap, intern_table);
    BOOST_REQUIRE(payload.has_value());
    return std::move(*payload);
}

std::vector<std::uint8_t> encode_pid_payload(
    eta::runtime::memory::heap::Heap& heap,
    eta::runtime::memory::intern::InternTable& intern_table,
    const eta::runtime::types::Pid& pid) {
    auto pid_value = eta::runtime::memory::factory::make_pid(heap, pid);
    BOOST_REQUIRE(pid_value.has_value());
    auto payload = eta::nng::serialize_binary_strict(*pid_value, heap, intern_table);
    BOOST_REQUIRE(payload.has_value());
    return std::move(*payload);
}

std::optional<std::int64_t> decode_fixnum_payload(
    std::span<const std::uint8_t> payload,
    eta::runtime::memory::heap::Heap& heap,
    eta::runtime::memory::intern::InternTable& intern_table) {
    auto decoded = eta::nng::deserialize_binary(payload, heap, intern_table);
    if (!decoded.has_value()) return std::nullopt;
    auto value = eta::runtime::nanbox::ops::decode<std::int64_t>(*decoded);
    if (!value.has_value()) return std::nullopt;
    return *value;
}

std::optional<std::string> decode_symbol_payload(
    std::span<const std::uint8_t> payload,
    eta::runtime::memory::heap::Heap& heap,
    eta::runtime::memory::intern::InternTable& intern_table) {
    auto decoded = eta::nng::deserialize_binary(payload, heap, intern_table);
    if (!decoded.has_value()) return std::nullopt;
    if (!eta::runtime::nanbox::ops::is_boxed(*decoded)
        || eta::runtime::nanbox::ops::tag(*decoded) != eta::runtime::nanbox::Tag::Symbol) {
        return std::nullopt;
    }
    auto text = intern_table.get_string(eta::runtime::nanbox::ops::payload(*decoded));
    if (!text.has_value()) return std::nullopt;
    return std::string(*text);
}

std::optional<eta::runtime::types::Pid> decode_pid_payload(
    std::span<const std::uint8_t> payload,
    eta::runtime::memory::heap::Heap& heap,
    eta::runtime::memory::intern::InternTable& intern_table) {
    auto decoded = eta::nng::deserialize_binary(payload, heap, intern_table);
    if (!decoded.has_value()) return std::nullopt;
    if (!eta::runtime::nanbox::ops::is_boxed(*decoded)
        || eta::runtime::nanbox::ops::tag(*decoded) != eta::runtime::nanbox::Tag::HeapObject) {
        return std::nullopt;
    }
    auto* pid = heap.try_get_as<
        eta::runtime::memory::heap::ObjectKind::Pid,
        eta::runtime::types::Pid>(eta::runtime::nanbox::ops::payload(*decoded));
    if (!pid) return std::nullopt;
    return *pid;
}

void establish_distribution_link(
    eta::runtime::actor::ActorSystem& server,
    eta::runtime::actor::ActorSystem& client,
    const std::string& server_name,
    const std::string& client_name,
    const std::string& cookie,
    const std::string& endpoint) {
    std::string error;
    BOOST_REQUIRE(server.configure_node(server_name, cookie, &error));
    BOOST_REQUIRE(client.configure_node(client_name, cookie, &error));
    BOOST_REQUIRE(server.node_listen(endpoint, &error));
    BOOST_REQUIRE(client.node_connect(endpoint, &error));

    const bool handshake_complete = wait_until([&]() {
        return server.connected_nodes().size() == 1u
            && client.connected_nodes().size() == 1u;
    });
    BOOST_REQUIRE(handshake_complete);
}

void run_remote_monitor_exit_case(
    bool crash,
    eta::runtime::actor::ActorSystem::ExitReason::Kind expected_reason) {
    using eta::runtime::actor::ActorSystem;

    ActorSystem server;
    ActorSystem client;

    auto server_pid = server.register_current_thread_actor();
    auto client_pid = client.register_current_thread_actor();
    BOOST_REQUIRE(server_pid.has_value());
    BOOST_REQUIRE(client_pid.has_value());

    establish_distribution_link(
        server,
        client,
        "eta@server",
        "eta@client",
        "cookie-remote-monitor",
        unique_inproc_endpoint());

    auto worker = server.spawn([&server, crash](const eta::runtime::types::Pid& pid) {
        (void)server.receive(pid, std::nullopt);
        if (crash) {
            throw std::runtime_error("remote monitor crash path");
        }
    });
    BOOST_REQUIRE(worker.has_value());

    auto monitor_ref = client.monitor(*client_pid, *worker);
    BOOST_REQUIRE(monitor_ref.has_value());

    eta::runtime::actor::ActorSystem::BinaryMessage trigger{0x01u};
    BOOST_TEST(server.send(*worker, std::move(trigger)));

    auto down = client.receive(*client_pid, std::chrono::milliseconds(5000));
    BOOST_REQUIRE(down.has_value());
    BOOST_TEST(
        static_cast<int>(down->kind)
        == static_cast<int>(ActorSystem::Message::Kind::DownSignal));
    BOOST_TEST(down->monitor_ref == *monitor_ref);
    BOOST_TEST(down->pid.node_id == worker->node_id);
    BOOST_TEST(down->pid.actor_id == worker->actor_id);
    BOOST_TEST(down->pid.incarnation == worker->incarnation);
    BOOST_TEST(
        static_cast<int>(down->reason.kind)
        == static_cast<int>(expected_reason));

    auto extra = client.receive(*client_pid, std::chrono::milliseconds(0));
    BOOST_TEST(!extra.has_value());
}

} // namespace

BOOST_AUTO_TEST_SUITE(actor_runtime_tests)

BOOST_AUTO_TEST_CASE(actor_self_is_pid_and_mailbox_starts_empty) {
    ActorHarness harness;
    auto result = harness.run_module(R"eta(
(module actor.runtime.self
  (import std.actor)
  (define result (and (pid? (self))
                      (= (mailbox-length) 0))))
)eta");
    BOOST_TEST(result == eta::runtime::nanbox::True);
}

BOOST_AUTO_TEST_CASE(actor_alive_reports_pid_lifecycle) {
    ActorHarness harness;
    auto result = harness.run_module(R"eta(
(module actor.runtime.alive
  (import std.actor)
  (define child (spawn (lambda () (receive-after 10000))))
  (define ref (monitor child))
  (define before (alive? child))
  (define stopped (exit child 'shutdown))
  (define down (receive-after 1000))
  (define after (alive? child))
  (define result
    (and before
         stopped
         (pair? down)
         (eq? (car down) 'DOWN)
         (= (car (cdr down)) ref)
         (eq? (car (cdr (cdr down))) 'process)
         (pid? (car (cdr (cdr (cdr down)))))
         (eq? (car (cdr (cdr (cdr (cdr down))))) 'shutdown)
         (not after))))
)eta");
    BOOST_TEST(result == eta::runtime::nanbox::True);
}

BOOST_AUTO_TEST_CASE(actor_send_receive_fifo_and_mailbox_length) {
    ActorHarness harness;
    auto result = harness.run_module(R"eta(
(module actor.runtime.fifo
  (import std.actor)
  (define me (self))
  (send me 10)
  (send me 20)
  (define before (mailbox-length))
  (define a (receive-after 0))
  (define mid (mailbox-length))
  (define b (receive-after 0))
  (define after (mailbox-length))
  (define result (and (= before 2)
                      (= a 10)
                      (= mid 1)
                      (= b 20)
                      (= after 0))))
)eta");
    BOOST_TEST(result == eta::runtime::nanbox::True);
}

BOOST_AUTO_TEST_CASE(actor_spawn_send_and_receive_round_trip) {
    ActorHarness harness;
    auto result = harness.run_module(R"eta(
(module actor.runtime.spawn
  (import std.actor)
  (define me (self))
  (define child
    (spawn
      (lambda ()
        (display "child-self-pid? ")
        (display (pid? (self)))
        (newline)
        (let* ((msg (receive-after 1000))
               (target (if msg (car (cdr msg)) #f))
               (sent (if target (send target (list 'pong (self))) #f)))
          (display "child-msg ")
          (display msg)
          (newline)
          (display "child-target-pid? ")
          (display (and target (pid? target)))
          (newline)
          (display "child-send ")
          (display sent)
          (newline)))))
  (define sent (send child (list 'ping me)))
  (display "parent-send ")
  (display sent)
  (newline)
  (define reply (receive-after 1000))
  (display "parent-reply ")
  (display reply)
  (newline)
  (define result
    (and (pid? child)
         sent
         (pair? reply)
         (eq? (car reply) 'pong)
         (pid? (car (cdr reply))))))
)eta");
    BOOST_TEST(result == eta::runtime::nanbox::True);
}

BOOST_AUTO_TEST_CASE(actor_receive_timeout_returns_false) {
    ActorHarness harness;
    auto result = harness.run_module(R"eta(
(module actor.runtime.timeout
  (import std.actor)
  (define result (receive-after 5)))
)eta");
    BOOST_TEST(result == eta::runtime::nanbox::False);
}

BOOST_AUTO_TEST_CASE(actor_process_info_reports_reductions_and_state) {
    ActorHarness harness;
    auto result = harness.run_module(R"eta(
(module actor.runtime.process_info
  (import std.actor)

  (defun spin (n acc)
    (if (= n 0)
        acc
        (spin (- n 1) (+ acc 1))))

  (define me (self))
  (define before (process-info me 'reductions))
  (define work (spin 64 0))
  (define after (process-info me 'reductions))
  (define info (process-info me))
  (define reductions-entry (assq 'reductions info))
  (define state-entry (assq 'state info))
  (define reason-entry (assq 'last-yield-reason info))
  (define queue-entry (assq 'message-queue-len info))
  (define result
    (and (= work 64)
         (number? before)
         (number? after)
         (> after before)
         (pair? reductions-entry)
         (>= (cdr reductions-entry) after)
         (pair? state-entry)
         (eq? (cdr state-entry) 'running)
         (pair? reason-entry)
         (eq? (cdr reason-entry) 'none)
         (pair? queue-entry)
         (= (cdr queue-entry) 0))))
)eta");
    BOOST_TEST(result == eta::runtime::nanbox::True);
}

BOOST_AUTO_TEST_CASE(actor_semantics_parity_under_tiny_reduction_budgets) {
    constexpr std::array<int, 3> kBudgets{1, 5, 20};
    constexpr std::array<std::string_view, 2> kSchedulerModes{
        "thread-per-actor",
        "pool-shadow"};

    for (const auto mode : kSchedulerModes) {
        for (const auto budget : kBudgets) {
            ScopedEnvVar scheduler_mode("ETA_ACTOR_SCHEDULER", std::string(mode));
            ScopedEnvVar reduction_budget(
                "ETA_ACTOR_REDUCTION_BUDGET",
                std::to_string(budget));

            ActorHarness harness;
            auto result = harness.run_module(R"eta(
(module actor.runtime.tiny_budget.parity
  (import std.actor)

  (define self-pid (self))
  (send self-pid (list 'a 1))
  (send self-pid (list 'b 2))
  (send self-pid (list 'a 3))

  (define picked (receive-match (match-list 'b 1) 0))
  (define left-1 (receive-after 0))
  (define left-2 (receive-after 0))
  (define left-3 (receive-after 0))

  (trap-exit! #t)
  (define child (spawn (lambda () (receive-after 10000))))
  (link child)
  (define ref (monitor child))
  (exit child 'boom)

  (define down-msg (receive-match (match-list 'DOWN 4) 1000))
  (define exit-msg (receive-match (match-list 'EXIT 2) 1000))

  (define result
    (and (pair? picked)
         (eq? (car picked) 'b)
         (= (car (cdr picked)) 2)
         (pair? left-1)
         (eq? (car left-1) 'a)
         (= (car (cdr left-1)) 1)
         (pair? left-2)
         (eq? (car left-2) 'a)
         (= (car (cdr left-2)) 3)
         (not left-3)
         (pair? down-msg)
         (eq? (car down-msg) 'DOWN)
         (= (car (cdr down-msg)) ref)
         (eq? (car (cdr (cdr down-msg))) 'process)
         (pid? (car (cdr (cdr (cdr down-msg)))))
         (eq? (car (cdr (cdr (cdr (cdr down-msg))))) 'boom)
         (pair? exit-msg)
         (eq? (car exit-msg) 'EXIT)
         (pid? (car (cdr exit-msg)))
         (eq? (car (cdr (cdr exit-msg))) 'boom)
         (not (receive-after 0)))))
)eta");

            BOOST_TEST_CONTEXT(
                "ETA_ACTOR_SCHEDULER=" << mode
                << ", ETA_ACTOR_REDUCTION_BUDGET=" << budget) {
                BOOST_TEST(result == eta::runtime::nanbox::True);
            }
        }
    }
}

BOOST_AUTO_TEST_CASE(actor_spawned_closure_resumes_across_budget_slices) {
    ScopedEnvVar scheduler_mode("ETA_ACTOR_SCHEDULER", "pool");
    ScopedEnvVar reduction_budget("ETA_ACTOR_REDUCTION_BUDGET", "1000000");

    ActorHarness harness;
    auto result = harness.run_module(R"eta(
(module actor.runtime.spawn.resume.budget
  (import std.actor)

  (defun make-list (n)
    (let loop ((i 0) (acc '()))
      (if (= i n)
          acc
          (loop (+ i 1) (cons i acc)))))

  (defun heavy-step ()
    (let* ((xs (make-list 5000))
           (ys (reverse xs)))
      (length ys)))

  (defun run-steps (count)
    (let loop ((i 0) (acc 0))
      (if (= i count)
          acc
          (loop (+ i 1) (+ acc (heavy-step))))))

  (define me (self))
  (spawn (lambda () (send me (run-steps 16))))
  (define result (receive-after 5000)))
)eta");

    BOOST_REQUIRE_MESSAGE(
        result != eta::runtime::nanbox::False,
        "timed out waiting for spawned actor result under budget slicing");
    BOOST_TEST(harness.as_int(result) == 80000);
}

BOOST_AUTO_TEST_CASE(actor_scheduler_env_thread_per_actor_is_honored) {
    using eta::runtime::actor::ActorSystem;

    ScopedEnvVar scheduler_mode("ETA_ACTOR_SCHEDULER", "thread-per-actor");
    ActorHarness harness;
    auto actor_system = harness.driver.vm().actor_system();
    BOOST_REQUIRE(actor_system);

    BOOST_TEST(
        static_cast<int>(actor_system->scheduler_mode())
        == static_cast<int>(ActorSystem::SchedulerMode::ThreadPerActor));
}

BOOST_AUTO_TEST_CASE(actor_selective_receive_preserves_unmatched_order) {
    ActorHarness harness;
    auto result = harness.run_module(R"eta(
(module actor.runtime.selective
  (import std.actor)
  (define me (self))
  (send me (list 'a 1))
  (send me (list 'b 2))
  (send me (list 'a 3))
  (define matched (receive-match (match-list 'b 1) 0))
  (define left-1 (receive-after 0))
  (define left-2 (receive-after 0))
  (define left-3 (receive-after 0))
  (define result
    (and (pair? matched)
         (eq? (car matched) 'b)
         (= (car (cdr matched)) 2)
         (pair? left-1)
         (eq? (car left-1) 'a)
         (= (car (cdr left-1)) 1)
         (pair? left-2)
         (eq? (car left-2) 'a)
         (= (car (cdr left-2)) 3)
         (not left-3))))
)eta");
    BOOST_TEST(result == eta::runtime::nanbox::True);
}

BOOST_AUTO_TEST_CASE(actor_receive_match_clause_order_prefers_first_match) {
    ActorHarness harness;
    auto result = harness.run_module(R"eta(
(module actor.runtime.clause.order
  (import std.actor)
  (define me (self))
  (send me 'tick)
  (define selected
    (receive-match
      (list
        (match-case (match-predicate (lambda (msg) #t))
                    (lambda (msg) 'first))
        (match-case (match-symbol 'tick)
                    (lambda (msg) 'second)))
      0
      (lambda () 'timeout)))
  (define result (eq? selected 'first)))
)eta");
    BOOST_TEST(result == eta::runtime::nanbox::True);
}

BOOST_AUTO_TEST_CASE(actor_link_exit_turns_into_exit_message_when_trapping) {
    ActorHarness harness;
    auto result = harness.run_module(R"eta(
(module actor.runtime.link.trap
  (import std.actor)
  (trap-exit! #t)
  (define child (spawn (lambda () (receive-after 10000))))
  (define linked (link child))
  (define exited (exit child 'shutdown))
  (define msg (receive-after 1000))
  (trap-exit! #f)
  (define result
    (and linked
         exited
         (pair? msg)
         (eq? (car msg) 'EXIT)
         (pid? (car (cdr msg)))
         (eq? (car (cdr (cdr msg))) 'shutdown))))
)eta");
    BOOST_TEST(result == eta::runtime::nanbox::True);
}

BOOST_AUTO_TEST_CASE(actor_monitor_down_delivered_exactly_once) {
    ActorHarness harness;
    auto result = harness.run_module(R"eta(
(module actor.runtime.monitor.down
  (import std.actor)
  (define child (spawn (lambda () (receive-after 10000))))
  (define ref (monitor child))
  (define exited (exit child 'shutdown))
  (define msg (receive-after 1000))
  (define next (receive-after 0))
  (define result
    (and exited
         (pair? msg)
         (eq? (car msg) 'DOWN)
         (= (car (cdr msg)) ref)
         (eq? (car (cdr (cdr msg))) 'process)
         (pid? (car (cdr (cdr (cdr msg)))))
         (eq? (car (cdr (cdr (cdr (cdr msg))))) 'shutdown)
         (not next))))
)eta");
    BOOST_TEST(result == eta::runtime::nanbox::True);
}

BOOST_AUTO_TEST_CASE(actor_demonitor_flush_removes_queued_down) {
    ActorHarness harness;
    auto result = harness.run_module(R"eta(
(module actor.runtime.demonitor.flush
  (import std.actor)
  (define child (spawn (lambda () (receive-after 10000))))
  (define ref (monitor child))
  (define exited (exit child 'shutdown))
  (define queued (mailbox-length))
  (demonitor ref #t)
  (define msg (receive-after 0))
  (define result
    (and exited
         (> queued 0)
         (not msg))))
)eta");
    BOOST_TEST(result == eta::runtime::nanbox::True);
}

BOOST_AUTO_TEST_CASE(actor_registry_round_trip_and_send_by_name) {
    ActorHarness harness;
    auto result = harness.run_module(R"eta(
(module actor.runtime.registry
  (import std.actor)
  (define me (self))
  (define registered-ok (register 'main me))
  (define found (whereis 'main))
  (define sent (send 'main 77))
  (define got (receive-after 100))
  (define names (registered))
  (define unregistered-ok (unregister 'main))
  (define gone (whereis 'main))
  (define result
    (and registered-ok
         (pid? found)
         (member 'main names)
         (= sent 77)
         (= got 77)
         unregistered-ok
         (not gone))))
)eta");
    BOOST_TEST(result == eta::runtime::nanbox::True);
}

BOOST_AUTO_TEST_CASE(actor_registry_rejects_name_collision_and_clears_dead_pid) {
    ActorHarness harness;
    auto result = harness.run_module(R"eta(
(module actor.runtime.registry.collision
  (import std.actor)
  (define child-a (spawn (lambda () (receive-after 10000))))
  (define child-b (spawn (lambda () (receive-after 10000))))
  (define first (register 'worker child-a))
  (define second (register 'worker child-b))
  (define killed-a (exit child-a 'shutdown))
  (define gone (whereis 'worker))
  (define killed-b (exit child-b 'shutdown))
  (define result
    (and first
         (not second)
         killed-a
         (not gone)
         killed-b)))
)eta");
    BOOST_TEST(result == eta::runtime::nanbox::True);
}

BOOST_AUTO_TEST_CASE(actor_supervisor_one_for_one_restarts_only_failed_child) {
    ActorHarness harness;
    auto result = harness.run_module(R"eta(
(module actor.runtime.supervisor.one_for_one
  (import std.actor std.actor.supervisor)

  (defun started? (msg)
    (and (pair? msg)
         (eq? (car msg) 'started)
         (pair? (cdr msg))
         (pair? (cdr (cdr msg)))))

  (defun started-id (msg)
    (car (cdr msg)))

  (defun started-pid (msg)
    (car (cdr (cdr msg))))

  (defun pid-for (id a b c)
    (cond
      ((and (started? a) (eq? (started-id a) id)) (started-pid a))
      ((and (started? b) (eq? (started-id b) id)) (started-pid b))
      ((and (started? c) (eq? (started-id c) id)) (started-pid c))
      (else #f)))

  (define parent (self))
  (define specs
    (list
      (make-child-spec 'a (lambda () (send parent (list 'started 'a (self))) (receive-after 5000)))
      (make-child-spec 'b (lambda () (send parent (list 'started 'b (self))) (receive-after 5000)))
      (make-child-spec 'c (lambda () (send parent (list 'started 'c (self))) (receive-after 5000)))))

  (define supervisor
    (one-for-one specs 'max-restarts 10 'max-seconds 5))

  (define m1 (receive-after 1000))
  (define m2 (receive-after 1000))
  (define m3 (receive-after 1000))

  (define pid-a (pid-for 'a m1 m2 m3))
  (define pid-b (pid-for 'b m1 m2 m3))
  (define pid-c (pid-for 'c m1 m2 m3))

  (define crashed (and pid-b (exit pid-b 'shutdown)))
  (define restarted (receive-after 2000))
  (define extra (receive-after 250))
  (define restarted-pid (if (started? restarted) (started-pid restarted) #f))
  (define _cleanup-supervisor (kill supervisor))
  (define _cleanup-a (and pid-a (exit pid-a 'shutdown)))
  (define _cleanup-b (and restarted-pid (exit restarted-pid 'shutdown)))
  (define _cleanup-c (and pid-c (exit pid-c 'shutdown)))

  (define result
    (and (pid? supervisor)
         (started? m1)
         (started? m2)
         (started? m3)
         (pid? pid-a)
         (pid? pid-b)
         (pid? pid-c)
         crashed
         (started? restarted)
         (eq? (started-id restarted) 'b)
         (pid? (started-pid restarted))
         (not (equal? (started-pid restarted) pid-b))
         (not extra))))
)eta");
    BOOST_TEST(result == eta::runtime::nanbox::True);
}

BOOST_AUTO_TEST_CASE(actor_supervisor_one_for_all_restarts_entire_group) {
    ActorHarness harness;
    auto result = harness.run_module(R"eta(
(module actor.runtime.supervisor.one_for_all
  (import std.actor std.actor.supervisor)

  (defun started? (msg)
    (and (pair? msg)
         (eq? (car msg) 'started)
         (pair? (cdr msg))
         (pair? (cdr (cdr msg)))))

  (defun started-id (msg)
    (car (cdr msg)))

  (defun started-pid (msg)
    (car (cdr (cdr msg))))

  (defun pid-for (id a b c)
    (cond
      ((and (started? a) (eq? (started-id a) id)) (started-pid a))
      ((and (started? b) (eq? (started-id b) id)) (started-pid b))
      ((and (started? c) (eq? (started-id c) id)) (started-pid c))
      (else #f)))

  (defun member-id? (id a b c)
    (or (and (started? a) (eq? (started-id a) id))
        (and (started? b) (eq? (started-id b) id))
        (and (started? c) (eq? (started-id c) id))))

  (define parent (self))
  (define specs
    (list
      (make-child-spec 'a (lambda () (send parent (list 'started 'a (self))) (receive-after 5000)))
      (make-child-spec 'b (lambda () (send parent (list 'started 'b (self))) (receive-after 5000)))
      (make-child-spec 'c (lambda () (send parent (list 'started 'c (self))) (receive-after 5000)))))

  (define supervisor
    (one-for-all specs 'max-restarts 10 'max-seconds 5))

  (define i1 (receive-after 1000))
  (define i2 (receive-after 1000))
  (define i3 (receive-after 1000))
  (define pid-a (pid-for 'a i1 i2 i3))
  (define pid-b (pid-for 'b i1 i2 i3))
  (define pid-c (pid-for 'c i1 i2 i3))

  (define crashed (and pid-b (exit pid-b 'shutdown)))

  (define r1 (receive-after 2000))
  (define r2 (receive-after 2000))
  (define r3 (receive-after 2000))
  (define r4 (receive-after 250))

  (define rpid-a (pid-for 'a r1 r2 r3))
  (define rpid-b (pid-for 'b r1 r2 r3))
  (define rpid-c (pid-for 'c r1 r2 r3))
  (define _cleanup-supervisor (kill supervisor))
  (define _cleanup-a (and rpid-a (exit rpid-a 'shutdown)))
  (define _cleanup-b (and rpid-b (exit rpid-b 'shutdown)))
  (define _cleanup-c (and rpid-c (exit rpid-c 'shutdown)))

  (define result
    (and (pid? supervisor)
         crashed
         (member-id? 'a r1 r2 r3)
         (member-id? 'b r1 r2 r3)
         (member-id? 'c r1 r2 r3)
         (pid? rpid-a)
         (pid? rpid-b)
         (pid? rpid-c)
         (not (equal? rpid-a pid-a))
         (not (equal? rpid-b pid-b))
         (not (equal? rpid-c pid-c))
         (not r4))))
)eta");
    BOOST_TEST(result == eta::runtime::nanbox::True);
}

BOOST_AUTO_TEST_CASE(actor_supervisor_rest_for_one_restarts_suffix_only) {
    ActorHarness harness;
    auto result = harness.run_module(R"eta(
(module actor.runtime.supervisor.rest_for_one
  (import std.actor std.actor.supervisor)

  (defun started? (msg)
    (and (pair? msg)
         (eq? (car msg) 'started)
         (pair? (cdr msg))
         (pair? (cdr (cdr msg)))))

  (defun started-id (msg)
    (car (cdr msg)))

  (defun started-pid (msg)
    (car (cdr (cdr msg))))

  (defun pid-for (id a b c)
    (cond
      ((and (started? a) (eq? (started-id a) id)) (started-pid a))
      ((and (started? b) (eq? (started-id b) id)) (started-pid b))
      ((and (started? c) (eq? (started-id c) id)) (started-pid c))
      (else #f)))

  (define parent (self))
  (define specs
    (list
      (make-child-spec 'a (lambda () (send parent (list 'started 'a (self))) (receive-after 5000)))
      (make-child-spec 'b (lambda () (send parent (list 'started 'b (self))) (receive-after 5000)))
      (make-child-spec 'c (lambda () (send parent (list 'started 'c (self))) (receive-after 5000)))))

  (define supervisor
    (rest-for-one specs 'max-restarts 10 'max-seconds 5))

  (define i1 (receive-after 1000))
  (define i2 (receive-after 1000))
  (define i3 (receive-after 1000))
  (define pid-a (pid-for 'a i1 i2 i3))
  (define pid-b (pid-for 'b i1 i2 i3))
  (define pid-c (pid-for 'c i1 i2 i3))

  (define crashed (and pid-b (exit pid-b 'shutdown)))

  (define r1 (receive-after 2000))
  (define r2 (receive-after 2000))
  (define r3 (receive-after 250))

  (define rpid-b (pid-for 'b r1 r2 #f))
  (define rpid-c (pid-for 'c r1 r2 #f))
  (define _cleanup-supervisor (kill supervisor))
  (define _cleanup-a (and pid-a (exit pid-a 'shutdown)))
  (define _cleanup-b (and rpid-b (exit rpid-b 'shutdown)))
  (define _cleanup-c (and rpid-c (exit rpid-c 'shutdown)))

  (define result
    (and (pid? supervisor)
         (pid? pid-a)
         crashed
         (pid? rpid-b)
         (pid? rpid-c)
         (not (equal? rpid-b pid-b))
         (not (equal? rpid-c pid-c))
         (not r3))))
)eta");
    BOOST_TEST(result == eta::runtime::nanbox::True);
}

BOOST_AUTO_TEST_CASE(actor_supervisor_restart_intensity_exits_with_shutdown_reason) {
    ActorHarness harness;
    auto result = harness.run_module(R"eta(
(module actor.runtime.supervisor.restart_intensity
  (import std.actor std.actor.supervisor)

  (defun started? (msg)
    (and (pair? msg)
         (eq? (car msg) 'started)
         (pair? (cdr msg))
         (pair? (cdr (cdr msg)))))

  (defun down? (msg)
    (and (pair? msg)
         (eq? (car msg) 'DOWN)
         (pair? (cdr msg))
         (pair? (cdr (cdr msg)))
         (pair? (cdr (cdr (cdr msg))))
         (pair? (cdr (cdr (cdr (cdr msg)))))))

  (define parent (self))
  (define spec
    (make-child-spec 'worker
                     (lambda ()
                       (send parent (list 'started 'worker (self)))
                       (receive-after 5000))))

  (define supervisor
    (one-for-one (list spec) 'max-restarts 1 'max-seconds 5))
  (define sup-ref (monitor supervisor))

  (define s1 (receive-after 1000))
  (define pid-1 (if (started? s1) (car (cdr (cdr s1))) #f))
  (define k1 (and pid-1 (exit pid-1 'shutdown)))

  (define s2 (receive-after 2000))
  (define pid-2 (if (started? s2) (car (cdr (cdr s2))) #f))
  (define k2 (and pid-2 (exit pid-2 'shutdown)))

  (define down (receive-after 2000))
  (define maybe-third (receive-after 250))
  (define reason
    (if (down? down)
        (car (cdr (cdr (cdr (cdr down)))))
        #f))

  (define result
    (and (pid? supervisor)
         sup-ref
         (started? s1)
         (started? s2)
         k1
         k2
         (down? down)
         (= (car (cdr down)) sup-ref)
         (pair? reason)
         (eq? (car reason) 'shutdown)
         (eq? (car (cdr reason)) 'restart-intensity-exceeded)
         (not maybe-third))))
)eta");
    BOOST_TEST(result == eta::runtime::nanbox::True);
}

BOOST_AUTO_TEST_CASE(actor_distribution_transport_handshake_and_remote_send_round_trip) {
    using eta::runtime::actor::ActorSystem;
    using eta::runtime::memory::heap::Heap;
    using eta::runtime::memory::intern::InternTable;

    ActorSystem server;
    ActorSystem client;

    auto server_pid = server.register_current_thread_actor();
    auto client_pid = client.register_current_thread_actor();
    BOOST_REQUIRE(server_pid.has_value());
    BOOST_REQUIRE(client_pid.has_value());

    std::string error;
    BOOST_REQUIRE(server.configure_node("eta@server", "cookie-123", &error));
    BOOST_REQUIRE(client.configure_node("eta@client", "cookie-123", &error));

    const auto endpoint = unique_inproc_endpoint();
    BOOST_REQUIRE(server.node_listen(endpoint, &error));
    BOOST_REQUIRE(client.node_connect(endpoint, &error));

    const bool handshake_complete = wait_until([&]() {
        return server.connected_nodes().size() == 1u
            && client.connected_nodes().size() == 1u;
    });
    BOOST_REQUIRE(handshake_complete);

    Heap send_heap(4u * 1024u * 1024u);
    InternTable send_intern;
    auto to_server = encode_fixnum_payload(send_heap, send_intern, 42);

    const auto first_status = client.send_checked(*server_pid, std::move(to_server));
    BOOST_TEST(
        static_cast<int>(first_status)
        == static_cast<int>(ActorSystem::SendStatus::Delivered));

    auto received_server = server.receive(*server_pid, std::chrono::milliseconds(1000));
    BOOST_REQUIRE(received_server.has_value());
    BOOST_TEST(
        static_cast<int>(received_server->kind)
        == static_cast<int>(ActorSystem::Message::Kind::Payload));

    Heap recv_heap(4u * 1024u * 1024u);
    InternTable recv_intern;
    auto decoded_server = decode_fixnum_payload(
        std::span<const std::uint8_t>(received_server->payload),
        recv_heap,
        recv_intern);
    BOOST_REQUIRE(decoded_server.has_value());
    BOOST_TEST(*decoded_server == 42);

    auto pid_payload = encode_pid_payload(send_heap, send_intern, *client_pid);
    const auto second_status = client.send_checked(*server_pid, std::move(pid_payload));
    BOOST_TEST(
        static_cast<int>(second_status)
        == static_cast<int>(ActorSystem::SendStatus::Delivered));

    auto pid_message = server.receive(*server_pid, std::chrono::milliseconds(1000));
    BOOST_REQUIRE(pid_message.has_value());
    auto reply_target = decode_pid_payload(
        std::span<const std::uint8_t>(pid_message->payload),
        recv_heap,
        recv_intern);
    BOOST_REQUIRE(reply_target.has_value());
    BOOST_TEST(reply_target->node_id == client_pid->node_id);
    BOOST_TEST(reply_target->actor_id == client_pid->actor_id);
    BOOST_TEST(reply_target->incarnation == client_pid->incarnation);

    auto ack_payload = encode_symbol_payload(send_heap, send_intern, "pong");
    BOOST_TEST(server.bind_current_thread_pid(*server_pid));
    const auto reply_status = server.send_checked(*reply_target, std::move(ack_payload));
    BOOST_TEST(
        static_cast<int>(reply_status)
        == static_cast<int>(ActorSystem::SendStatus::Delivered));
    server.unbind_current_thread_pid();

    auto received_client = client.receive(*client_pid, std::chrono::milliseconds(1000));
    BOOST_REQUIRE(received_client.has_value());
    auto decoded_client = decode_symbol_payload(
        std::span<const std::uint8_t>(received_client->payload),
        recv_heap,
        recv_intern);
    BOOST_REQUIRE(decoded_client.has_value());
    BOOST_TEST(*decoded_client == "pong");
}

BOOST_AUTO_TEST_CASE(actor_distribution_transport_rejects_bad_cookie) {
    using eta::runtime::actor::ActorSystem;

    ActorSystem server;
    ActorSystem client;

    auto server_pid = server.register_current_thread_actor();
    auto client_pid = client.register_current_thread_actor();
    BOOST_REQUIRE(server_pid.has_value());
    BOOST_REQUIRE(client_pid.has_value());

    std::string error;
    BOOST_REQUIRE(server.configure_node("eta@server", "cookie-good", &error));
    BOOST_REQUIRE(client.configure_node("eta@client", "cookie-bad", &error));

    const auto endpoint = unique_inproc_endpoint();
    BOOST_REQUIRE(server.node_listen(endpoint, &error));
    const bool connected = client.node_connect(endpoint, &error);
    BOOST_TEST(!connected);

    const bool no_nodes = wait_until([&]() {
        return server.connected_nodes().empty() && client.connected_nodes().empty();
    });
    BOOST_TEST(no_nodes);
}

BOOST_AUTO_TEST_CASE(actor_distribution_node_monitor_reports_nodeup_and_disconnect_nodedown_once) {
    using eta::runtime::actor::ActorSystem;

    ActorSystem server;
    ActorSystem client;

    auto server_pid = server.register_current_thread_actor();
    auto client_pid = client.register_current_thread_actor();
    BOOST_REQUIRE(server_pid.has_value());
    BOOST_REQUIRE(client_pid.has_value());

    auto node_ref = server.monitor_node(*server_pid, "eta@client");
    BOOST_REQUIRE(node_ref.has_value());

    establish_distribution_link(
        server,
        client,
        "eta@server",
        "eta@client",
        "cookie-node-monitor",
        unique_inproc_endpoint());

    auto node_up = server.receive(*server_pid, std::chrono::milliseconds(1000));
    BOOST_REQUIRE(node_up.has_value());
    BOOST_TEST(
        static_cast<int>(node_up->kind)
        == static_cast<int>(ActorSystem::Message::Kind::NodeUp));
    BOOST_TEST(node_up->monitor_ref == *node_ref);
    BOOST_TEST(node_up->node_name == "eta@client");
    BOOST_TEST(node_up->node_id != 0u);

    auto no_extra_up = server.receive(*server_pid, std::chrono::milliseconds(0));
    BOOST_TEST(!no_extra_up.has_value());

    BOOST_TEST(server.disconnect_node("eta@client"));
    auto node_down = server.receive(*server_pid, std::chrono::milliseconds(1500));
    BOOST_REQUIRE(node_down.has_value());
    BOOST_TEST(
        static_cast<int>(node_down->kind)
        == static_cast<int>(ActorSystem::Message::Kind::NodeDown));
    BOOST_TEST(node_down->monitor_ref == *node_ref);
    BOOST_TEST(node_down->node_name == "eta@client");
    BOOST_TEST(
        static_cast<int>(node_down->reason.kind)
        == static_cast<int>(ActorSystem::ExitReason::Kind::NoConnection));

    auto no_extra_down = server.receive(*server_pid, std::chrono::milliseconds(0));
    BOOST_TEST(!no_extra_down.has_value());
}

BOOST_AUTO_TEST_CASE(actor_distribution_node_monitor_reports_bad_cookie_reconnect_attempt) {
    using eta::runtime::actor::ActorSystem;

    ActorSystem server;
    ActorSystem bad_client;

    auto server_pid = server.register_current_thread_actor();
    auto bad_client_pid = bad_client.register_current_thread_actor();
    BOOST_REQUIRE(server_pid.has_value());
    BOOST_REQUIRE(bad_client_pid.has_value());

    auto node_ref = server.monitor_node(*server_pid, "eta@client");
    BOOST_REQUIRE(node_ref.has_value());

    std::string error;
    BOOST_REQUIRE(server.configure_node("eta@server", "cookie-good", &error));
    BOOST_REQUIRE(bad_client.configure_node("eta@client", "cookie-bad", &error));

    const auto endpoint = unique_inproc_endpoint();
    BOOST_REQUIRE(server.node_listen(endpoint, &error));
    const bool connected = bad_client.node_connect(endpoint, &error);
    BOOST_TEST(!connected);

    auto node_down = server.receive(*server_pid, std::chrono::milliseconds(1500));
    BOOST_REQUIRE(node_down.has_value());
    BOOST_TEST(
        static_cast<int>(node_down->kind)
        == static_cast<int>(ActorSystem::Message::Kind::NodeDown));
    BOOST_TEST(node_down->monitor_ref == *node_ref);
    BOOST_TEST(node_down->node_name == "eta@client");
    BOOST_TEST(
        static_cast<int>(node_down->reason.kind)
        == static_cast<int>(ActorSystem::ExitReason::Kind::BadCookie));
}

BOOST_AUTO_TEST_CASE(actor_distribution_remote_monitor_down_normal_exit_exactly_once) {
    run_remote_monitor_exit_case(
        /*crash=*/false,
        eta::runtime::actor::ActorSystem::ExitReason::Kind::Normal);
}

BOOST_AUTO_TEST_CASE(actor_distribution_remote_monitor_down_error_exit_exactly_once) {
    run_remote_monitor_exit_case(
        /*crash=*/true,
        eta::runtime::actor::ActorSystem::ExitReason::Kind::Error);
}

BOOST_AUTO_TEST_CASE(actor_distribution_remote_monitor_down_on_node_loss) {
    using eta::runtime::actor::ActorSystem;

    ActorSystem server;
    ActorSystem client;

    auto server_pid = server.register_current_thread_actor();
    auto client_pid = client.register_current_thread_actor();
    BOOST_REQUIRE(server_pid.has_value());
    BOOST_REQUIRE(client_pid.has_value());

    establish_distribution_link(
        server,
        client,
        "eta@server",
        "eta@client",
        "cookie-node-loss",
        unique_inproc_endpoint());

    auto worker = server.spawn([&server](const eta::runtime::types::Pid& pid) {
        (void)server.receive(pid, std::nullopt);
    });
    BOOST_REQUIRE(worker.has_value());

    auto monitor_ref = client.monitor(*client_pid, *worker);
    BOOST_REQUIRE(monitor_ref.has_value());

    BOOST_TEST(client.disconnect_node("eta@server"));

    auto down = client.receive(*client_pid, std::chrono::milliseconds(5000));
    BOOST_REQUIRE(down.has_value());
    BOOST_TEST(
        static_cast<int>(down->kind)
        == static_cast<int>(ActorSystem::Message::Kind::DownSignal));
    BOOST_TEST(down->monitor_ref == *monitor_ref);
    BOOST_TEST(down->pid.node_id == worker->node_id);
    BOOST_TEST(down->pid.actor_id == worker->actor_id);
    BOOST_TEST(down->pid.incarnation == worker->incarnation);
    BOOST_TEST(
        static_cast<int>(down->reason.kind)
        == static_cast<int>(ActorSystem::ExitReason::Kind::NoConnection));

    auto extra = client.receive(*client_pid, std::chrono::milliseconds(0));
    BOOST_TEST(!extra.has_value());
}

BOOST_AUTO_TEST_CASE(actor_distribution_remote_demonitor_flush_suppresses_stale_down) {
    using eta::runtime::actor::ActorSystem;

    ActorSystem server;
    ActorSystem client;

    auto server_pid = server.register_current_thread_actor();
    auto client_pid = client.register_current_thread_actor();
    BOOST_REQUIRE(server_pid.has_value());
    BOOST_REQUIRE(client_pid.has_value());

    establish_distribution_link(
        server,
        client,
        "eta@server",
        "eta@client",
        "cookie-demonitor",
        unique_inproc_endpoint());

    auto worker = server.spawn([&server](const eta::runtime::types::Pid& pid) {
        (void)server.receive(pid, std::nullopt);
    });
    BOOST_REQUIRE(worker.has_value());

    auto monitor_ref = client.monitor(*client_pid, *worker);
    BOOST_REQUIRE(monitor_ref.has_value());

    BOOST_TEST(client.demonitor(*client_pid, *monitor_ref, true));
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    eta::runtime::actor::ActorSystem::BinaryMessage trigger{0x01u};
    BOOST_TEST(server.send(*worker, std::move(trigger)));

    auto down = client.receive(*client_pid, std::chrono::milliseconds(400));
    BOOST_TEST(!down.has_value());
}

BOOST_AUTO_TEST_SUITE_END()
