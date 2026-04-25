/**
 * @file driver_jupyter_test.cpp
 * @brief Driver regression tests for notebook-facing evaluation APIs.
 */

#include <boost/test/unit_test.hpp>

#include <atomic>
#include <chrono>
#include <filesystem>
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

BOOST_AUTO_TEST_SUITE(driver_jupyter_tests)

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

BOOST_AUTO_TEST_SUITE_END()

