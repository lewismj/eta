/**
 * @file driver_jupyter_test.cpp
 * @brief Driver regression tests for notebook-facing evaluation APIs.
 */

#include <boost/test/unit_test.hpp>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "eta/session/eval_display.h"
#include "eta/session/driver.h"
#include "eta/interpreter/module_path.h"

namespace fs = std::filesystem;

#ifndef ETA_STDLIB_DIR
#define ETA_STDLIB_DIR ""
#endif

static fs::path stdlib_dir() {
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

static eta::interpreter::ModulePathResolver make_resolver() {
    auto stdlib = stdlib_dir();
    if (stdlib.empty()) return eta::interpreter::ModulePathResolver{};
    return eta::interpreter::ModulePathResolver({stdlib});
}

static void require_prelude(eta::session::Driver& driver) {
    auto pr = driver.load_prelude();
    BOOST_REQUIRE_MESSAGE(pr.found, "prelude.eta was not found for driver jupyter tests");
    BOOST_REQUIRE_MESSAGE(pr.loaded, "prelude.eta failed to load for driver jupyter tests");
}

struct CurrentPathGuard {
    fs::path original;

    CurrentPathGuard()
        : original(fs::current_path()) {}

    ~CurrentPathGuard() {
        std::error_code ec;
        fs::current_path(original, ec);
    }
};

struct ScopedTempDir {
    fs::path path;

    ScopedTempDir() {
        const auto stamp =
            std::chrono::steady_clock::now().time_since_epoch().count();
        path = fs::temp_directory_path() / ("eta_jupyter_s7_" + std::to_string(stamp));
        fs::create_directories(path);
    }

    ~ScopedTempDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};

BOOST_AUTO_TEST_SUITE(driver_jupyter_tests)

BOOST_AUTO_TEST_CASE(startup_resolver_discovers_project_modules_from_lockfile) {
    ScopedTempDir temp;
    const auto project_root = temp.path / "app";
    fs::create_directories(project_root / ".eta" / "modules" / "dep-0.1.0" / "src");
    fs::create_directories(project_root / "src");

    {
        std::ofstream out(project_root / "eta.toml", std::ios::out | std::ios::binary | std::ios::trunc);
        BOOST_REQUIRE(out.is_open());
        out << "[package]\n"
            << "name = \"app\"\n"
            << "version = \"1.0.0\"\n"
            << "license = \"MIT\"\n\n"
            << "[compatibility]\n"
            << "eta = \">=0.6, <0.8\"\n\n"
            << "[dependencies]\n"
            << "dep = { path = \"../dep\" }\n";
    }
    {
        std::ofstream out(project_root / "eta.lock", std::ios::out | std::ios::binary | std::ios::trunc);
        BOOST_REQUIRE(out.is_open());
        out << "version = 1\n\n"
            << "[[package]]\n"
            << "name = \"app\"\n"
            << "version = \"1.0.0\"\n"
            << "source = \"root\"\n"
            << "dependencies = [\"dep@0.1.0\"]\n\n"
            << "[[package]]\n"
            << "name = \"dep\"\n"
            << "version = \"0.1.0\"\n"
            << "source = \"path+../dep\"\n"
            << "dependencies = []\n";
    }
    {
        std::ofstream out(project_root / ".eta" / "modules" / "dep-0.1.0" / "src" / "dep.eta",
                          std::ios::out | std::ios::binary | std::ios::trunc);
        BOOST_REQUIRE(out.is_open());
        out << "(module dep\n"
            << "  (export dep-value)\n"
            << "  (begin (define dep-value 9)))\n";
    }

    CurrentPathGuard cwd_guard;
    fs::current_path(project_root);

    auto resolver = eta::interpreter::ModulePathResolver::from_args_or_env_at(
        "", project_root);
    eta::session::Driver driver(std::move(resolver));
    require_prelude(driver);

    eta::runtime::nanbox::LispVal result{eta::runtime::nanbox::Nil};
    const bool ok = driver.run_source(R"eta(
(module app.test
  (import dep)
  (define result dep-value))
)eta", &result, "result");
    BOOST_REQUIRE(ok);

    auto decoded = eta::runtime::nanbox::ops::decode<int64_t>(result);
    BOOST_REQUIRE(decoded.has_value());
    BOOST_TEST(*decoded == 9);
}

BOOST_AUTO_TEST_CASE(is_complete_expression_unbalanced_paren_with_indent_hint) {
    eta::session::Driver driver(make_resolver());
    std::string indent;
    const bool complete = driver.is_complete_expression("(+ 1", &indent);
    BOOST_TEST(!complete);
    BOOST_TEST(indent == "  ");
}

BOOST_AUTO_TEST_CASE(is_complete_expression_balanced_input_returns_true) {
    eta::session::Driver driver(make_resolver());
    std::string indent;
    const bool complete = driver.is_complete_expression("(+ 1 2)", &indent);
    BOOST_TEST(complete);
    BOOST_TEST(indent.empty());
}

BOOST_AUTO_TEST_CASE(is_complete_expression_unterminated_string_returns_false) {
    eta::session::Driver driver(make_resolver());
    std::string indent;
    const bool complete = driver.is_complete_expression("\"unterminated", &indent);
    BOOST_TEST(!complete);
}

BOOST_AUTO_TEST_CASE(is_complete_expression_representative_inputs) {
    eta::session::Driver driver(make_resolver());

    struct Probe {
        std::string input;
        bool complete{false};
        std::string indent;
    };

    const std::vector<Probe> probes = {
        {"", true, ""},
        {"(+ 1", false, "  "},
        {"(+ 1 2)", true, ""},
        {"\"unterminated", false, ""},
        {"(display \"ok\")", true, ""},
        {"(+ 1\n  (+ 2 3)", false, "  "},
        {"#| block", false, ""},
        {"#| nested #| block |# still-open", false, ""},
        {"#| nested #| block |# done |#", true, ""},
        {"; comment only", true, ""},
        {"(+ 1 2)\n.continue", false, "  "},
        {"(+ 1 2)\n  ; trailing comment", true, ""},
    };
    BOOST_REQUIRE_EQUAL(probes.size(), 12u);

    for (const auto& probe : probes) {
        std::string indent;
        const bool complete = driver.is_complete_expression(probe.input, &indent);
        BOOST_CHECK_MESSAGE(complete == probe.complete, "input: " << probe.input);
        if (!probe.complete) {
            BOOST_CHECK_MESSAGE(indent == probe.indent, "input: " << probe.input);
        }
    }
}

BOOST_AUTO_TEST_CASE(completions_at_import_prefix_includes_std_torch) {
    eta::session::Driver driver(make_resolver());
    require_prelude(driver);

    const std::string code = "(import std.to";
    const auto completion = driver.completions_at(code, code.size());

    BOOST_TEST(completion.cursor_start < completion.cursor_end);
    BOOST_TEST(completion.cursor_end == code.size());

    bool found_std_torch = false;
    for (const auto& m : completion.matches) {
        if (m == "std.torch") {
            found_std_torch = true;
            break;
        }
    }
    BOOST_TEST(found_std_torch);
}

BOOST_AUTO_TEST_CASE(hover_at_resolves_prelude_binding_docs) {
    eta::session::Driver driver(make_resolver());
    require_prelude(driver);

    const auto markdown = driver.hover_at("defrel");
    BOOST_TEST(!markdown.empty());
    BOOST_TEST(markdown.find("defrel") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(request_interrupt_stops_runaway_evaluation_quickly) {
    using namespace std::chrono_literals;

    eta::session::Driver driver(make_resolver());
    require_prelude(driver);

    static constexpr auto kRunawaySource = R"eta(
(module driver-jupyter-interrupt
  (begin
    (define (spin i)
      (if (< i 500000000)
          (spin (+ i 1))
          i))
    (spin 0)))
)eta";

    std::atomic<bool> finished{false};
    bool ok = true;
    std::thread worker([&]() {
        ok = driver.run_source(kRunawaySource);
        finished.store(true, std::memory_order_release);
    });

    std::this_thread::sleep_for(5ms);
    const auto interrupt_start = std::chrono::steady_clock::now();
    driver.request_interrupt();

    while (!finished.load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(1ms);
    }
    worker.join();

    const auto elapsed = std::chrono::steady_clock::now() - interrupt_start;
    BOOST_TEST(!ok);
    BOOST_TEST(std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count() <= 50);
}

BOOST_AUTO_TEST_CASE(eval_to_display_tags_tensor_values) {
    eta::session::Driver driver(make_resolver());
    require_prelude(driver);

    auto display = driver.eval_to_display("(torch/zeros '(3 3))");
    BOOST_TEST(static_cast<int>(display.tag) == static_cast<int>(eta::session::DisplayTag::Tensor));
}

BOOST_AUTO_TEST_CASE(eval_to_display_tags_fact_table_values) {
    eta::session::Driver driver(make_resolver());
    require_prelude(driver);

    auto display = driver.eval_to_display("(%make-fact-table '(x y))");
    BOOST_TEST(static_cast<int>(display.tag) == static_cast<int>(eta::session::DisplayTag::FactTable));
}

BOOST_AUTO_TEST_CASE(eval_to_display_tags_jupyter_wrapper_values) {
    eta::session::Driver driver(make_resolver());
    require_prelude(driver);

    auto html = driver.eval_to_display("(vector 'jupyter-display \"text/html\" \"<b>ok</b>\")");
    BOOST_TEST(static_cast<int>(html.tag) == static_cast<int>(eta::session::DisplayTag::Html));

    auto vega = driver.eval_to_display(
        "(vector 'jupyter-display \"application/vnd.vegalite.v5+json\" \"{\\\"mark\\\":\\\"line\\\"}\")");
    BOOST_TEST(static_cast<int>(vega.tag) == static_cast<int>(eta::session::DisplayTag::VegaLite));
}

BOOST_AUTO_TEST_CASE(eval_to_display_import_std_jupyter_module) {
    eta::session::Driver driver(make_resolver());
    require_prelude(driver);

    const auto imported = driver.eval_to_display("(import std.jupyter)");
    BOOST_TEST(static_cast<int>(imported.tag) != static_cast<int>(eta::session::DisplayTag::Error));
}

BOOST_AUTO_TEST_CASE(eval_to_display_persists_imports_between_calls) {
    eta::session::Driver driver(make_resolver());
    require_prelude(driver);

    const auto first = driver.eval_to_display("(import (only std.aad grad))");
    BOOST_TEST(static_cast<int>(first.tag) != static_cast<int>(eta::session::DisplayTag::Error));

    const auto second = driver.eval_to_display(
        "(grad (lambda (x y) (+ (* x y) (sin x))) '(2 3))");
    BOOST_TEST(static_cast<int>(second.tag) != static_cast<int>(eta::session::DisplayTag::Error));
}

BOOST_AUTO_TEST_CASE(set_stream_sinks_routes_stdout_and_stderr) {
    eta::session::Driver driver(make_resolver());
    require_prelude(driver);

    std::string stdout_text;
    std::string stderr_text;
    driver.set_stream_sinks(
        [&stdout_text](std::string_view chunk) { stdout_text.append(chunk); },
        [&stderr_text](std::string_view chunk) { stderr_text.append(chunk); });

    std::string out;
    const bool ok = driver.eval_string(
        "(begin "
        "  (display \"hello\")"
        "  (newline)"
        "  (display \"boom\" (current-error-port))"
        "  (newline (current-error-port))"
        "  42)",
        out);
    BOOST_TEST(ok);
    BOOST_TEST(stdout_text.find("hello") != std::string::npos);
    BOOST_TEST(stderr_text.find("boom") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(spawn_thread_routes_stdout_to_spawning_stream_sink) {
    using namespace std::chrono_literals;

    eta::session::Driver driver(make_resolver());
    require_prelude(driver);

    std::mutex stdout_mu;
    std::string stdout_text;
    std::atomic<long long> first_marker_ms{-1};
    const auto start = std::chrono::steady_clock::now();

    driver.set_stream_sinks(
        [&](std::string_view chunk) {
            if (chunk.empty()) return;
            bool marker_seen = false;
            {
                std::lock_guard<std::mutex> lk(stdout_mu);
                stdout_text.append(chunk);
                marker_seen = stdout_text.find("routing-hi") != std::string::npos;
            }
            if (marker_seen) {
                const auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - start).count();
                long long expected = -1;
                (void)first_marker_ms.compare_exchange_strong(
                    expected, now_ms, std::memory_order_acq_rel);
            }
        },
        [](std::string_view) {});

    std::string out;
    const bool spawned = driver.eval_string(
        "(define routed-thread "
        "  (spawn-thread "
        "    (lambda () "
        "      (define (spin i) "
        "        (if (< i 1500000) "
        "            (spin (+ i 1)) "
        "            i)) "
        "      (spin 0) "
        "      (println \"routing-hi\"))))",
        out);
    if (!spawned) {
        std::ostringstream diagnostics;
        driver.diagnostics().print_all(
            diagnostics, /*use_color=*/false, driver.file_resolver());
        BOOST_FAIL("spawn-thread setup failed: " + diagnostics.str());
    }
    const auto eval_done_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start).count();

    const auto deadline = std::chrono::steady_clock::now() + 3s;
    while (first_marker_ms.load(std::memory_order_acquire) < 0 &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(5ms);
    }

    std::string join_out;
    BOOST_REQUIRE(driver.eval_string("(thread-join routed-thread)", join_out));

    const auto marker_ms = first_marker_ms.load(std::memory_order_acquire);
    BOOST_REQUIRE(marker_ms >= 0);
    BOOST_TEST(marker_ms >= eval_done_ms);
}

BOOST_AUTO_TEST_CASE(runtime_error_populates_user_error_diagnostic) {
    eta::session::Driver driver(make_resolver());
    require_prelude(driver);

    std::string out;
    const bool ok = driver.eval_string("(error \"x\")", out);
    BOOST_TEST(!ok);

    const auto& diags = driver.diagnostics().diagnostics();
    BOOST_REQUIRE(!diags.empty());
    BOOST_TEST(static_cast<int>(diags.front().code) ==
               static_cast<int>(eta::diagnostic::DiagnosticCode::UserError));
    BOOST_TEST(diags.front().message.find("x") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(clear_module_cache_allows_module_reload) {
    namespace fs = std::filesystem;

    auto stdlib = stdlib_dir();
    BOOST_REQUIRE(!stdlib.empty());

    const auto tmp = fs::temp_directory_path() / "eta_driver_reload_test";
    std::error_code ec;
    fs::create_directories(tmp, ec);
    BOOST_REQUIRE(!ec);

    const auto module_path = tmp / "jupyter_reload_test.eta";
    {
        std::ofstream out(module_path, std::ios::out | std::ios::binary | std::ios::trunc);
        out << "(module jupyter_reload_test\n"
            << "  (export reload-value)\n"
            << "  (begin\n"
            << "    (define reload-value 1)))\n";
    }

    eta::interpreter::ModulePathResolver resolver({tmp, stdlib});
    eta::session::Driver driver(std::move(resolver));
    require_prelude(driver);

    BOOST_REQUIRE(driver.run_file(module_path));
    BOOST_TEST(driver.has_module("jupyter_reload_test"));

    BOOST_TEST(driver.clear_module_cache("jupyter_reload_test"));
    BOOST_TEST(!driver.has_module("jupyter_reload_test"));

    BOOST_REQUIRE(driver.run_file(module_path));
    BOOST_TEST(driver.has_module("jupyter_reload_test"));

    fs::remove(module_path, ec);
}

BOOST_AUTO_TEST_SUITE_END()

