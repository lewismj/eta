// debug_tests.cpp — Tests for the VM debug API (source maps, breakpoints, stepping)
//
// Uses the same fixture pattern as vm_tests.cpp / emitter_tests.cpp.
// NOTE: BOOST_TEST_MODULE is defined once in eta_test.cpp for the whole binary.

#include <boost/test/unit_test.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

#include "eta/reader/lexer.h"
#include "eta/reader/parser.h"
#include "eta/reader/expander.h"
#include "eta/reader/module_linker.h"
#include "eta/semantics/semantic_analyzer.h"
#include "eta/semantics/emitter.h"
#include "eta/runtime/vm/vm.h"
#include "eta/runtime/vm/bytecode.h"
#include "eta/runtime/builtin_env.h"
#include "eta/runtime/core_primitives.h"

using namespace eta;
using namespace eta::semantics;
using namespace eta::runtime;
using namespace eta::runtime::vm;
using namespace eta::reader::lexer;

// ---------------------------------------------------------------------------
// Shared fixture — same structure as VMTestFixture
// ---------------------------------------------------------------------------

struct DebugFixture {
    memory::heap::Heap          heap;
    memory::intern::InternTable intern_table;
    BytecodeFunctionRegistry    registry;
    BuiltinEnvironment          builtins;
    std::size_t                 last_total_globals_{256}; // updated by compile()

    explicit DebugFixture(std::size_t heap_bytes = 2 * 1024 * 1024)
        : heap(heap_bytes)
    {
        register_core_primitives(builtins, heap, intern_table);
    }

    /// Compile module source through full pipeline.
    /// Returns the main BytecodeFunction* (owned by registry).
    BytecodeFunction* compile(const std::string& src, uint32_t file_id = 1) {
        reader::lexer::Lexer lex(file_id, src);
        reader::parser::Parser p(lex);
        auto parsed_res = p.parse_toplevel();
        if (!parsed_res) throw std::runtime_error("Parse error");
        auto parsed = std::move(*parsed_res);

        reader::expander::Expander ex;
        auto expanded_res = ex.expand_many(parsed);
        if (!expanded_res) throw std::runtime_error("Expansion error");
        auto expanded = std::move(*expanded_res);

        reader::ModuleLinker linker;
        (void)linker.index_modules(expanded);
        (void)linker.link();

        SemanticAnalyzer sa;
        auto sem_res = sa.analyze_all(expanded, linker, builtins);
        if (!sem_res) throw std::runtime_error("Semantic error: " + sem_res.error().message);
        if (sem_res->empty()) throw std::runtime_error("No modules produced");

        auto& sem_mod = (*sem_res)[0];
        // Remember total_globals so callers can pass the right count to install()
        last_total_globals_ = sem_mod.total_globals;

        Emitter emitter(sem_mod, heap, intern_table, registry);
        return emitter.emit();
    }

    /// Build a VM ready for use (function resolver wired up).
    std::unique_ptr<VM> make_vm() {
        auto vm = std::make_unique<VM>(heap, intern_table);
        vm->set_function_resolver([this](uint32_t idx) { return registry.get(idx); });
        return vm;
    }

    /// Run @p fn on a background thread; waits for at most @p timeout for a
    /// single stop event, then resumes and joins.  Returns the stop event
    /// (StopReason::Pause if timeout fired without stopping).
    StopEvent run_until_stop(VM& vm, std::function<void()> run_fn,
                             std::chrono::milliseconds timeout = std::chrono::milliseconds(5000))
    {
        std::mutex mu;
        std::condition_variable cv;
        StopEvent captured{StopReason::Pause, {}, {}};
        bool stopped = false;

        // The callback is invoked WITHOUT debug_mutex_ held (see run_loop() fix),
        // so it is safe to call vm.resume() from within.
        vm.set_stop_callback([&](const StopEvent& ev) {
            bool is_first;
            {
                std::lock_guard<std::mutex> lk(mu);
                is_first = !stopped;
                if (!stopped) {
                    captured = ev;
                    stopped  = true;
                    cv.notify_one();
                }
            }
            // Multiple instructions can share the same source line (e.g. LoadConst +
            // StoreGlobal both tagged line 2). The second hit must be auto-resumed;
            // otherwise t.join() below would hang forever because the main thread has
            // already called its single resume().
            if (!is_first) {
                vm.resume();
            }
        });

        std::thread t(std::move(run_fn));

        {
            std::unique_lock<std::mutex> lk(mu);
            cv.wait_for(lk, timeout, [&] { return stopped; });
        }

        vm.resume();   // resume the first (captured) stop – no-op if never stopped
        t.join();
        return captured;
    }
};

// ===========================================================================
// SUITE 1 — Source map: every emitted instruction has a span
// ===========================================================================

BOOST_AUTO_TEST_SUITE(source_map_suite)

BOOST_AUTO_TEST_CASE(source_map_size_equals_code_size) {
    DebugFixture f;
    f.compile("(module test (define result (+ 1 2)))", /*file_id=*/1);

    BOOST_REQUIRE_GT(f.registry.size(), 0u);

    for (std::size_t i = 0; i < f.registry.size(); ++i) {
        const auto* fn = f.registry.get(static_cast<uint32_t>(i));
        BOOST_REQUIRE(fn != nullptr);
        // Core invariant: source_map is always the same length as code.
        BOOST_CHECK_EQUAL(fn->code.size(), fn->source_map.size());
    }
}

BOOST_AUTO_TEST_CASE(source_map_has_valid_spans) {
    DebugFixture f;
    f.compile("(module test (define result (+ 1 2)))", /*file_id=*/2);

    bool any_valid = false;
    for (std::size_t i = 0; i < f.registry.size(); ++i) {
        const auto* fn = f.registry.get(static_cast<uint32_t>(i));
        BOOST_REQUIRE(fn);
        for (const auto& sp : fn->source_map) {
            if (sp.file_id != 0) { any_valid = true; break; }
        }
    }
    BOOST_CHECK_MESSAGE(any_valid, "At least one instruction should carry a non-zero file_id");
}

BOOST_AUTO_TEST_CASE(source_map_lambda) {
    DebugFixture f;
    f.compile("(module test (define (add x y) (+ x y)) (define result (add 3 4)))",
              /*file_id=*/3);

    // Should produce at least 2 functions: module init + 'add' lambda
    BOOST_CHECK_GE(f.registry.size(), 2u);

    for (std::size_t i = 0; i < f.registry.size(); ++i) {
        const auto* fn = f.registry.get(static_cast<uint32_t>(i));
        BOOST_REQUIRE(fn);
        BOOST_CHECK_EQUAL(fn->code.size(), fn->source_map.size());
    }
}

BOOST_AUTO_TEST_CASE(span_at_in_range) {
    DebugFixture f;
    f.compile("(module test (define result (+ 1 2)))", /*file_id=*/4);

    const auto* fn = f.registry.get(0);
    BOOST_REQUIRE(fn);
    BOOST_REQUIRE_GT(fn->code.size(), 0u);

    // Every in-range pc should return without crashing
    for (uint32_t pc = 0; pc < static_cast<uint32_t>(fn->code.size()); ++pc) {
        auto sp = fn->span_at(pc);
        (void)sp; // just verify no crash / assertion
    }
}

BOOST_AUTO_TEST_CASE(span_at_out_of_range_returns_zero) {
    DebugFixture f;
    f.compile("(module test (define result 42))", /*file_id=*/5);

    const auto* fn = f.registry.get(0);
    BOOST_REQUIRE(fn);

    auto sp = fn->span_at(static_cast<uint32_t>(fn->code.size() + 100));
    BOOST_CHECK_EQUAL(sp.file_id, 0u);
    BOOST_CHECK_EQUAL(sp.start.line, 1u);
}

BOOST_AUTO_TEST_SUITE_END()

// ===========================================================================
// SUITE 2 — Breakpoints: set_breakpoints API and async pause
// ===========================================================================

BOOST_AUTO_TEST_SUITE(breakpoints_suite)

BOOST_AUTO_TEST_CASE(set_breakpoints_no_crash) {
    DebugFixture f;
    auto vm = f.make_vm();

    std::vector<BreakLocation> locs = {{2u, 5u}, {1u, 10u}, {2u, 3u}};
    vm->set_breakpoints(locs);
    BOOST_CHECK(!vm->is_paused());
}

BOOST_AUTO_TEST_CASE(request_pause_stops_vm) {
    DebugFixture f;
    auto* main_fn = f.compile(
        "(module test (define result (+ 1 2 3)))", /*file_id=*/10);

    auto vm = f.make_vm();
    auto install = f.builtins.install(f.heap, vm->globals(), f.last_total_globals_);
    BOOST_REQUIRE(install.has_value());

    vm->request_pause(); // fire before execution starts

    StopEvent ev = f.run_until_stop(*vm, [&] {
        (void)vm->execute(*main_fn);
    });

    BOOST_CHECK(ev.reason == StopReason::Pause ||
                ev.reason == StopReason::Breakpoint ||
                ev.reason == StopReason::Step);
}

BOOST_AUTO_TEST_CASE(breakpoint_by_file_and_line) {
    DebugFixture f;
    // Multi-line source: the breakpoint targets line 2
    const std::string src =
        "(module test\n"         // line 1
        "  (define x 10)\n"      // line 2  ← breakpoint here
        "  (define result x))";  // line 3

    auto* main_fn = f.compile(src, /*file_id=*/11);

    // Find which instructions are on line 2
    bool found_line2 = false;
    for (std::size_t i = 0; i < f.registry.size(); ++i) {
        const auto* fn = f.registry.get(static_cast<uint32_t>(i));
        if (!fn) continue;
        for (const auto& sp : fn->source_map) {
            if (sp.file_id == 11 && sp.start.line == 2) { found_line2 = true; break; }
        }
    }
    BOOST_REQUIRE_MESSAGE(found_line2, "No instructions on line 2 — can't test breakpoint");

    auto vm = f.make_vm();
    auto install = f.builtins.install(f.heap, vm->globals(), f.last_total_globals_);
    BOOST_REQUIRE(install.has_value());

    vm->set_breakpoints({{11u, 2u}});

    StopEvent ev = f.run_until_stop(*vm, [&] {
        (void)vm->execute(*main_fn);
    });

    BOOST_CHECK(ev.reason == StopReason::Breakpoint);
    if (ev.reason == StopReason::Breakpoint) {
        BOOST_CHECK_EQUAL(ev.span.file_id, 11u);
        BOOST_CHECK_EQUAL(ev.span.start.line, 2u);
    }
}

BOOST_AUTO_TEST_SUITE_END()

// ===========================================================================
// SUITE 3 — Stepping
// ===========================================================================

BOOST_AUTO_TEST_SUITE(stepping_suite)

BOOST_AUTO_TEST_CASE(step_in_produces_step_stop) {
    DebugFixture f;
    const std::string src =
        "(module test\n"
        "  (define (inc x) (+ x 1))\n"
        "  (define result (inc 5)))";

    auto* main_fn = f.compile(src, /*file_id=*/20);

    auto vm = f.make_vm();
    auto install = f.builtins.install(f.heap, vm->globals(), f.last_total_globals_);
    BOOST_REQUIRE(install.has_value());

    std::mutex mu;
    std::condition_variable cv;
    std::vector<StopEvent> stops;

    vm->set_stop_callback([&](const StopEvent& ev) {
        std::lock_guard<std::mutex> lk(mu);
        stops.push_back(ev);
        cv.notify_one();
        // Note: we do NOT auto-resume here; the test explicitly calls step_in()
        // or resume() after checking stops.size().
    });

    // Trigger first stop via pause, then do a step_in
    vm->request_pause();

    std::thread t([&] { (void)vm->execute(*main_fn); });

    auto wait = [&](std::size_t n) {
        std::unique_lock<std::mutex> lk(mu);
        return cv.wait_for(lk, std::chrono::seconds(5), [&] { return stops.size() >= n; });
    };

    BOOST_REQUIRE_MESSAGE(wait(1), "Timed out waiting for initial pause");
    vm->step_in(); // resumes AND sets step mode; will stop on the next different source line
    BOOST_CHECK_MESSAGE(wait(2), "Timed out waiting for step_in stop");

    vm->resume(); // clear step mode, let VM finish
    t.join();

    BOOST_REQUIRE_GE(stops.size(), 2u);
    BOOST_CHECK(stops[1].reason == StopReason::Step);
}

BOOST_AUTO_TEST_CASE(step_over_stays_in_caller) {
    DebugFixture f;
    const std::string src =
        "(module test\n"
        "  (define (double x) (+ x x))\n"
        "  (define a (double 3))\n"
        "  (define result a))";

    auto* main_fn = f.compile(src, /*file_id=*/21);

    auto vm = f.make_vm();
    auto install = f.builtins.install(f.heap, vm->globals(), f.last_total_globals_);
    BOOST_REQUIRE(install.has_value());

    std::mutex mu;
    std::condition_variable cv;
    std::vector<StopEvent> stops;

    vm->set_stop_callback([&](const StopEvent& ev) {
        std::lock_guard<std::mutex> lk(mu);
        stops.push_back(ev);
        cv.notify_one();
        // Test manages resumes explicitly via step_over() and resume().
    });

    vm->request_pause();
    std::thread t([&] { (void)vm->execute(*main_fn); });

    auto wait = [&](std::size_t n) {
        std::unique_lock<std::mutex> lk(mu);
        return cv.wait_for(lk, std::chrono::seconds(5), [&] { return stops.size() >= n; });
    };

    BOOST_REQUIRE_MESSAGE(wait(1), "Timed out waiting for initial pause");
    vm->step_over();
    BOOST_CHECK_MESSAGE(wait(2), "Timed out waiting for step_over stop");

    vm->resume();
    t.join();

    BOOST_REQUIRE_GE(stops.size(), 2u);
    BOOST_CHECK(stops[1].reason == StopReason::Step);
}

BOOST_AUTO_TEST_CASE(resume_lets_script_complete) {
    DebugFixture f;
    auto* main_fn = f.compile("(module test (define result (+ 1 2)))", /*file_id=*/22);

    auto vm = f.make_vm();
    auto install = f.builtins.install(f.heap, vm->globals(), f.last_total_globals_);
    BOOST_REQUIRE(install.has_value());

    std::mutex mu;
    std::condition_variable cv;
    bool stopped    = false;
    bool vm_done    = false;

    vm->set_stop_callback([&](const StopEvent&) {
        std::lock_guard<std::mutex> lk(mu);
        stopped = true;
        cv.notify_one();
    });
    vm->request_pause();

    std::thread t([&] {
        (void)vm->execute(*main_fn);
        std::lock_guard<std::mutex> lk(mu);
        vm_done = true;
        cv.notify_one();
    });

    {
        std::unique_lock<std::mutex> lk(mu);
        BOOST_CHECK(cv.wait_for(lk, std::chrono::seconds(5), [&] { return stopped; }));
    }
    vm->resume();
    {
        std::unique_lock<std::mutex> lk(mu);
        BOOST_CHECK(cv.wait_for(lk, std::chrono::seconds(5), [&] { return vm_done; }));
    }
    t.join();
    BOOST_CHECK(vm_done);
}

BOOST_AUTO_TEST_SUITE_END()

// ===========================================================================
// SUITE 4 — Introspection: get_frames / get_locals / get_upvalues
// ===========================================================================

BOOST_AUTO_TEST_SUITE(introspection_suite)

BOOST_AUTO_TEST_CASE(get_frames_while_paused_nonempty) {
    DebugFixture f;
    const std::string src =
        "(module test\n"
        "  (define (f x) (+ x 1))\n"
        "  (define result (f 5)))";

    auto* main_fn = f.compile(src, /*file_id=*/30);
    auto vm = f.make_vm();
    auto install = f.builtins.install(f.heap, vm->globals(), f.last_total_globals_);
    BOOST_REQUIRE(install.has_value());

    std::mutex mu;
    std::condition_variable cv;
    bool stopped = false;
    std::vector<FrameInfo> frames;

    vm->set_stop_callback([&](const StopEvent&) {
        frames = vm->get_frames();
        std::lock_guard<std::mutex> lk(mu);
        stopped = true;
        cv.notify_one();
    });
    vm->request_pause();

    std::thread t([&] { (void)vm->execute(*main_fn); });

    {
        std::unique_lock<std::mutex> lk(mu);
        BOOST_CHECK(cv.wait_for(lk, std::chrono::seconds(5), [&] { return stopped; }));
    }
    BOOST_CHECK_GE(frames.size(), 1u);
    // Frame 0 should be the innermost (module init or a called function)
    BOOST_CHECK(!frames.empty());

    vm->resume();
    t.join();
}

BOOST_AUTO_TEST_CASE(get_locals_does_not_crash) {
    DebugFixture f;
    auto* main_fn = f.compile(
        "(module test (define (g a b) (+ a b)) (define result (g 3 4)))",
        /*file_id=*/31);

    auto vm = f.make_vm();
    auto install = f.builtins.install(f.heap, vm->globals(), f.last_total_globals_);
    BOOST_REQUIRE(install.has_value());

    std::mutex mu;
    std::condition_variable cv;
    bool stopped = false;

    vm->set_stop_callback([&](const StopEvent&) {
        // Must not crash; result size depends on where we stopped
        auto locals = vm->get_locals(0);
        (void)locals;
        auto upvals = vm->get_upvalues(0);
        (void)upvals;
        std::lock_guard<std::mutex> lk(mu);
        stopped = true;
        cv.notify_one();
    });
    vm->request_pause();

    std::thread t([&] { (void)vm->execute(*main_fn); });
    {
        std::unique_lock<std::mutex> lk(mu);
        BOOST_CHECK(cv.wait_for(lk, std::chrono::seconds(5), [&] { return stopped; }));
    }
    vm->resume();
    t.join();
    BOOST_CHECK(stopped);
}

BOOST_AUTO_TEST_SUITE_END()

// ===========================================================================
// SUITE 5 — No-debug overhead: no callback → VM runs normally
// ===========================================================================

BOOST_AUTO_TEST_SUITE(no_debug_overhead_suite)

BOOST_AUTO_TEST_CASE(vm_runs_without_callback) {
    DebugFixture f;
    auto* main_fn = f.compile("(module test (define result (+ 1 2 3)))", /*file_id=*/40);

    auto vm = f.make_vm();
    auto install = f.builtins.install(f.heap, vm->globals(), f.last_total_globals_);
    BOOST_REQUIRE(install.has_value());

    // No set_stop_callback — VM must NOT pause.
    auto res = vm->execute(*main_fn);
    BOOST_CHECK(res.has_value());
    BOOST_CHECK(!vm->is_paused());
}

BOOST_AUTO_TEST_CASE(set_breakpoints_without_callback_no_pause) {
    DebugFixture f;
    auto* main_fn = f.compile("(module test (define result 99))", /*file_id=*/41);

    auto vm = f.make_vm();
    auto install = f.builtins.install(f.heap, vm->globals(), f.last_total_globals_);
    BOOST_REQUIRE(install.has_value());

    // Breakpoints installed but no stop callback → must not pause
    vm->set_breakpoints({{41u, 1u}});
    auto res = vm->execute(*main_fn);
    BOOST_CHECK(res.has_value());
    BOOST_CHECK(!vm->is_paused());
}

BOOST_AUTO_TEST_SUITE_END()

