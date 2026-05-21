#include <boost/test/unit_test.hpp>

#include <cstdint>
#include <filesystem>
#include <sstream>
#include <string>
#include <string_view>

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

BOOST_AUTO_TEST_SUITE_END()
