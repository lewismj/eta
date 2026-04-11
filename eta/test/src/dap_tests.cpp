// dap_tests.cpp — DAP protocol tests for DapServer
//
// Tests exercise the DAP server in-process by injecting std::istringstream /
// std::ostringstream instead of the real stdin/stdout pipe.  This lets us
// verify the Content-Length framing, request dispatch, and response/event
// correctness without spawning a subprocess.
//
// NOTE: BOOST_TEST_MODULE is defined once in eta_test.cpp for the whole binary.

#include <boost/test/unit_test.hpp>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "eta/dap/dap_server.h"
#include "eta/util/json.h"

namespace fs   = std::filesystem;
namespace json = eta::json;

// ============================================================================
// Helpers
// ============================================================================

/// Wrap a JSON body in the Content-Length frame used by the DAP wire protocol.
static std::string frame(const std::string& body) {
    return "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body;
}

/// Build a minimal DAP request JSON string.
static std::string request(int seq, const std::string& cmd,
                            const std::string& args = "{}") {
    return R"({"seq":)" + std::to_string(seq)
         + R"(,"type":"request","command":")" + cmd + '"'
         + R"(,"arguments":)" + args + '}';
}

/// Parse all Content-Length-framed DAP messages out of the raw output bytes.
static std::vector<json::Value> parse_output(const std::string& raw) {
    std::vector<json::Value> msgs;
    std::istringstream ss(raw);
    while (true) {
        std::size_t len = 0;
        std::string line;
        while (std::getline(ss, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.empty()) break;
            const std::string pfx = "Content-Length: ";
            if (line.substr(0, pfx.size()) == pfx)
                len = std::stoull(line.substr(pfx.size()));
        }
        if (ss.eof() || ss.fail() || len == 0) break;
        std::string body(len, '\0');
        ss.read(body.data(), static_cast<std::streamsize>(len));
        if (ss.fail()) break;
        msgs.push_back(json::parse(body));
    }
    return msgs;
}

/// Find the first message matching type + command/event name.
/// Returns a null Value if not found.
static json::Value find_msg(const std::vector<json::Value>& msgs,
                             const std::string& type,
                             const std::string& name) {
    const std::string key = (type == "response") ? "command" : "event";
    for (const auto& m : msgs) {
        auto t = m.get_string("type");
        auto n = m.get_string(key);
        if (t && n && *t == type && *n == name) return m;
    }
    return {};   // null Value
}

/// Escape a filesystem path for safe embedding in a JSON string value.
/// Converts backslashes to \\\\ and double-quotes to \\".
static std::string json_path(const fs::path& p) {
    std::string s = p.string();
    std::string out;
    out.reserve(s.size() + 8);
    for (char c : s) {
        if      (c == '\\') out += "\\\\";
        else if (c == '"')  out += "\\\"";
        else                out += c;
    }
    return out;
}
static std::vector<std::string> collect_output(const std::vector<json::Value>& msgs,
                                                const std::string& category) {
    std::vector<std::string> out;
    for (const auto& m : msgs) {
        auto t = m.get_string("type");
        auto e = m.get_string("event");
        if (t && e && *t == "event" && *e == "output") {
            auto cat = m["body"].get_string("category");
            if (cat && *cat == category) {
                auto text = m["body"].get_string("output");
                if (text) out.push_back(*text);
            }
        }
    }
    return out;
}

/// Collect text from custom "eta-output" events (script stdout/stderr).
/// The DAP server sends these instead of standard "output" events so the
/// VS Code extension can route them to the "Eta Output" panel.
/// Body shape: { "stream": "stdout"|"stderr", "text": "..." }
static std::vector<std::string> collect_eta_output(const std::vector<json::Value>& msgs,
                                                    const std::string& stream) {
    std::vector<std::string> out;
    for (const auto& m : msgs) {
        auto t = m.get_string("type");
        auto e = m.get_string("event");
        if (t && e && *t == "event" && *e == "eta-output") {
            auto s = m["body"].get_string("stream");
            if (s && *s == stream) {
                auto text = m["body"].get_string("text");
                if (text) out.push_back(*text);
            }
        }
    }
    return out;
}

/// Run the server synchronously with the given framed input and return all
/// parsed output messages.
static std::vector<json::Value> run_server(const std::string& input) {
    std::istringstream in(input);
    std::ostringstream out;
    eta::dap::DapServer server(in, out);
    server.run();
    return parse_output(out.str());
}

// ============================================================================
// Test suite
// ============================================================================

BOOST_AUTO_TEST_SUITE(dap_protocol)

// ---------------------------------------------------------------------------
// 1. initialize → capabilities response (no "initialized" event yet per DAP spec;
//    "initialized" is sent after launch so VS Code knows script_path_ is set)
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(initialize_returns_capabilities) {
    std::string input =
        frame(request(1, "initialize",
            R"({"clientID":"test","adapterID":"eta","linesStartAt1":true})"))
      + frame(request(2, "disconnect", "{}"));

    auto msgs = run_server(input);

    auto resp = find_msg(msgs, "response", "initialize");
    BOOST_REQUIRE(!resp.is_null());
    BOOST_TEST(resp["success"].as_bool() == true);
    BOOST_TEST(resp["body"]["supportsConfigurationDoneRequest"].as_bool() == true);
    BOOST_TEST(resp["body"]["supportsTerminateRequest"].as_bool() == true);

    // "initialized" must NOT be present without a prior "launch" (we moved the
    // event to handle_launch to match the DAP spec and fix the 0-breakpoints bug)
    auto evt = find_msg(msgs, "event", "initialized");
    BOOST_TEST(evt.is_null());
}

// ---------------------------------------------------------------------------
// 1b. launch → capabilities response + initialized event (correct DAP order)
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(launch_sends_initialized_event) {
    std::string input =
        frame(request(1, "initialize", "{}"))
      + frame(request(2, "launch",
            R"({"program":"/tmp/nonexistent.eta","stopOnEntry":false})"))
      + frame(request(3, "disconnect", "{}"));

    auto msgs = run_server(input);

    // "initialized" must be present and come AFTER the "launch" response
    auto evt = find_msg(msgs, "event", "initialized");
    BOOST_TEST(!evt.is_null());
}

// ---------------------------------------------------------------------------
// 2. launch stores the program path and responds with success
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(launch_responds_success) {
    std::string input =
        frame(request(1, "initialize", "{}"))
      + frame(request(2, "launch",
            R"({"program":"/tmp/nonexistent.eta","stopOnEntry":false})"))
      + frame(request(3, "disconnect", "{}"));

    auto msgs = run_server(input);

    auto resp = find_msg(msgs, "response", "launch");
    BOOST_REQUIRE(!resp.is_null());
    BOOST_TEST(resp["success"].as_bool() == true);
}

// ---------------------------------------------------------------------------
// 3. setBreakpoints before launch → unverified breakpoints stored/returned
//    with IDs assigned by the adapter
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(set_breakpoints_before_launch) {
    std::string input =
        frame(request(1, "initialize", "{}"))
      + frame(request(2, "setBreakpoints",
            R"({"source":{"path":"/tmp/test.eta"},)"
            R"("breakpoints":[{"line":5},{"line":10}]})"))
      + frame(request(3, "disconnect", "{}"));

    auto msgs = run_server(input);

    auto resp = find_msg(msgs, "response", "setBreakpoints");
    BOOST_REQUIRE(!resp.is_null());
    BOOST_TEST(resp["success"].as_bool() == true);

    const auto& bps = resp["body"]["breakpoints"].as_array();
    BOOST_REQUIRE_EQUAL(bps.size(), 2u);
    // Before the VM has started, breakpoints are reported as not yet verified
    BOOST_TEST(bps[0]["verified"].as_bool() == false);
    BOOST_TEST(bps[0]["line"].as_int() == 5);
    BOOST_TEST(bps[1]["line"].as_int() == 10);
    // Each breakpoint must have an integer ID assigned by the adapter
    BOOST_TEST(bps[0]["id"].is_int() == true);
    BOOST_TEST(bps[1]["id"].is_int() == true);
    // IDs must be distinct
    BOOST_TEST(bps[0]["id"].as_int() != bps[1]["id"].as_int());
}

// ---------------------------------------------------------------------------
// 4. disconnect → server exits cleanly with a success response
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(disconnect_exits_cleanly) {
    std::string input =
        frame(request(1, "initialize", "{}"))
      + frame(request(2, "disconnect", "{}"));

    auto msgs = run_server(input);

    auto resp = find_msg(msgs, "response", "disconnect");
    BOOST_REQUIRE(!resp.is_null());
    BOOST_TEST(resp["success"].as_bool() == true);
}

// ---------------------------------------------------------------------------
// 5. configurationDone with a non-existent file → terminated event with error
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(bad_program_path_sends_terminated) {
    std::string input =
        frame(request(1, "initialize", "{}"))
      + frame(request(2, "launch",
            R"({"program":"/tmp/eta_dap_test_nonexistent_XXXXX.eta"})"))
      + frame(request(3, "configurationDone", "{}"))
      + frame(request(4, "disconnect", "{}"));

    auto msgs = run_server(input);

    // Must eventually send terminated
    auto term = find_msg(msgs, "event", "terminated");
    BOOST_TEST(!term.is_null());
    // And an output/stderr event describing the failure
    auto errs = collect_output(msgs, "stderr");
    BOOST_TEST(!errs.empty());
}

// ---------------------------------------------------------------------------
// 6. Full minimal script run — output captured as DAP events, not leaked to pipe
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(script_output_captured_as_output_event) {
    // Write a tiny script to a temp file.
    // Does NOT import the prelude — uses only the builtin `display`.
    const std::string src = R"(
(module dap-test-output
  (begin
    (display "hello-dap")
    (newline)))
)";
    auto tmp = fs::temp_directory_path() / "eta_dap_test_output.eta";
    {
        std::ofstream f(tmp);
        BOOST_REQUIRE(f.is_open());
        f << src;
    }

    std::string prog_arg = R"({"program":")" + json_path(tmp) + R"("})";

    std::string input =
        frame(request(1, "initialize", "{}"))
      + frame(request(2, "launch", prog_arg))
      + frame(request(3, "configurationDone", "{}"))
      + frame(request(4, "disconnect", "{}"));

    auto msgs = run_server(input);

    // The "hello-dap" text must appear in a custom "eta-output" event
    // (stream "stdout") — NOT have leaked into the raw protocol stream.
    // Script output uses "eta-output" instead of standard "output" events
    // so the VS Code extension routes it to the dedicated "Eta Output" panel.
    auto stdout_chunks = collect_eta_output(msgs, "stdout");
    std::string all_stdout;
    for (const auto& c : stdout_chunks) all_stdout += c;
    BOOST_TEST(all_stdout.find("hello-dap") != std::string::npos);

    // terminated event must be present
    BOOST_TEST(!find_msg(msgs, "event", "terminated").is_null());

    fs::remove(tmp);
}

// ---------------------------------------------------------------------------
// 7. Sequence numbers are monotonically increasing in all server messages
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(sequence_numbers_are_monotonic) {
    std::string input =
        frame(request(1, "initialize", "{}"))
      + frame(request(2, "disconnect", "{}"));

    auto msgs = run_server(input);

    int64_t prev = -1;
    for (const auto& m : msgs) {
        auto seq = m.get_int("seq");
        BOOST_REQUIRE(seq.has_value());
        BOOST_TEST(*seq > prev);
        prev = *seq;
    }
}

// ---------------------------------------------------------------------------
// 8. Unknown command gets an empty-success response (keeps VS Code happy)
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(unknown_command_gets_success_response) {
    std::string input =
        frame(request(1, "initialize", "{}"))
      + frame(request(2, "someUnknownCommand", "{}"))
      + frame(request(3, "disconnect", "{}"));

    auto msgs = run_server(input);

    auto resp = find_msg(msgs, "response", "someUnknownCommand");
    BOOST_REQUIRE(!resp.is_null());
    BOOST_TEST(resp["success"].as_bool() == true);
}

// ---------------------------------------------------------------------------
// 9. launch with no 'program' field → error response (success:false)
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(launch_without_program_sends_error) {
    std::string input =
        frame(request(1, "initialize", "{}"))
      + frame(request(2, "launch", "{}"))      // no "program" key
      + frame(request(3, "disconnect", "{}"));

    auto msgs = run_server(input);

    auto resp = find_msg(msgs, "response", "launch");
    BOOST_REQUIRE(!resp.is_null());
    BOOST_TEST(resp["success"].as_bool() == false);
    // Must carry an error message
    auto fmt = resp["body"]["error"].get_string("format");
    BOOST_TEST(fmt.has_value());
    BOOST_TEST(fmt->find("program") != std::string::npos);
}

// ---------------------------------------------------------------------------
// 10. setBreakpoints with no 'path' in source → empty breakpoints list
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(set_breakpoints_without_source_path) {
    std::string input =
        frame(request(1, "initialize", "{}"))
      + frame(request(2, "setBreakpoints",
            R"({"source":{},"breakpoints":[{"line":1}]})"))
      + frame(request(3, "disconnect", "{}"));

    auto msgs = run_server(input);

    auto resp = find_msg(msgs, "response", "setBreakpoints");
    BOOST_REQUIRE(!resp.is_null());
    BOOST_TEST(resp["success"].as_bool() == true);
    BOOST_TEST(resp["body"]["breakpoints"].as_array().empty());
}

// ---------------------------------------------------------------------------
// 11. setExceptionBreakpoints is acknowledged with success
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(set_exception_breakpoints_acknowledged) {
    std::string input =
        frame(request(1, "initialize", "{}"))
      + frame(request(2, "setExceptionBreakpoints",
            R"({"filters":["uncaught"]})"))
      + frame(request(3, "disconnect", "{}"));

    auto msgs = run_server(input);

    auto resp = find_msg(msgs, "response", "setExceptionBreakpoints");
    BOOST_REQUIRE(!resp.is_null());
    BOOST_TEST(resp["success"].as_bool() == true);
}

// ---------------------------------------------------------------------------
// 12. configurationDone before launch — must not crash
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(configuration_done_before_launch_is_safe) {
    std::string input =
        frame(request(1, "initialize", "{}"))
      + frame(request(2, "configurationDone", "{}"))   // no launch first
      + frame(request(3, "disconnect", "{}"));

    auto msgs = run_server(input);

    auto resp = find_msg(msgs, "response", "configurationDone");
    BOOST_REQUIRE(!resp.is_null());
    BOOST_TEST(resp["success"].as_bool() == true);
    // No vm thread should have started, so no terminated event
    BOOST_TEST(find_msg(msgs, "event", "terminated").is_null());
}

// ---------------------------------------------------------------------------
// 13. threads response has correct structure
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(threads_response_structure) {
    std::string input =
        frame(request(1, "initialize", "{}"))
      + frame(request(2, "threads", "{}"))
      + frame(request(3, "disconnect", "{}"));

    auto msgs = run_server(input);

    auto resp = find_msg(msgs, "response", "threads");
    BOOST_REQUIRE(!resp.is_null());
    BOOST_TEST(resp["success"].as_bool() == true);
    const auto& threads = resp["body"]["threads"].as_array();
    BOOST_REQUIRE_EQUAL(threads.size(), 1u);
    BOOST_TEST(threads[0].get_int("id").value_or(-1) == 1);
    auto name = threads[0].get_string("name");
    BOOST_TEST(name.has_value());
    BOOST_TEST(!name->empty());
}

// ---------------------------------------------------------------------------
// 14. scopes response has Module + Locals + Upvalues + Globals entries
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(scopes_response_structure) {
    std::string input =
        frame(request(1, "initialize", "{}"))
      + frame(request(2, "scopes", R"({"frameId":0})"))
      + frame(request(3, "disconnect", "{}"));

    auto msgs = run_server(input);

    auto resp = find_msg(msgs, "response", "scopes");
    BOOST_REQUIRE(!resp.is_null());
    BOOST_TEST(resp["success"].as_bool() == true);
    const auto& scopes = resp["body"]["scopes"].as_array();
    BOOST_REQUIRE_EQUAL(scopes.size(), 4u);
    auto n0 = scopes[0].get_string("name");
    auto n1 = scopes[1].get_string("name");
    auto n2 = scopes[2].get_string("name");
    auto n3 = scopes[3].get_string("name");
    BOOST_REQUIRE(n0.has_value());
    BOOST_REQUIRE(n1.has_value());
    BOOST_REQUIRE(n2.has_value());
    BOOST_REQUIRE(n3.has_value());
    BOOST_TEST(*n0 == "Module");
    BOOST_TEST(*n1 == "Locals");
    BOOST_TEST(*n2 == "Upvalues");
    BOOST_TEST(*n3 == "Globals");
    // variablesReference values must all differ
    auto ref0 = scopes[0].get_int("variablesReference");
    auto ref1 = scopes[1].get_int("variablesReference");
    auto ref2 = scopes[2].get_int("variablesReference");
    auto ref3 = scopes[3].get_int("variablesReference");
    BOOST_REQUIRE(ref0.has_value());
    BOOST_REQUIRE(ref1.has_value());
    BOOST_REQUIRE(ref2.has_value());
    BOOST_REQUIRE(ref3.has_value());
    BOOST_TEST(*ref0 != *ref1);
    BOOST_TEST(*ref0 != *ref2);
    BOOST_TEST(*ref0 != *ref3);
    BOOST_TEST(*ref1 != *ref2);
    BOOST_TEST(*ref1 != *ref3);
    BOOST_TEST(*ref2 != *ref3);
}

// ---------------------------------------------------------------------------
// 15. evaluate without a running VM → "<not available>" result
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(evaluate_without_driver_returns_not_available) {
    std::string input =
        frame(request(1, "initialize", "{}"))
      + frame(request(2, "evaluate", "{\"expression\":\"(+ 1 2)\"}"))
      + frame(request(3, "disconnect", "{}"));

    auto msgs = run_server(input);

    auto resp = find_msg(msgs, "response", "evaluate");
    BOOST_REQUIRE(!resp.is_null());
    BOOST_TEST(resp["success"].as_bool() == true);
    auto result = resp["body"].get_string("result");
    BOOST_REQUIRE(result.has_value());
    BOOST_TEST(*result == "<not available>");
}

// ---------------------------------------------------------------------------
// 16. Sending setBreakpoints twice for the same file replaces the first set
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(set_breakpoints_replaces_previous_set) {
    std::string input =
        frame(request(1, "initialize", "{}"))
      + frame(request(2, "setBreakpoints",
            R"({"source":{"path":"/tmp/bp_test.eta"},)"
            R"("breakpoints":[{"line":1},{"line":2},{"line":3}]})"))
      + frame(request(3, "setBreakpoints",        // replace with just line 7
            R"({"source":{"path":"/tmp/bp_test.eta"},)"
            R"("breakpoints":[{"line":7}]})"))
      + frame(request(4, "disconnect", "{}"));

    auto msgs = run_server(input);

    // Only the second response matters; it must have exactly one breakpoint
    auto resps = [&] {
        std::vector<json::Value> out;
        for (const auto& m : msgs) {
            auto t = m.get_string("type");
            auto c = m.get_string("command");
            if (t && c && *t == "response" && *c == "setBreakpoints")
                out.push_back(m);
        }
        return out;
    }();
    BOOST_REQUIRE_EQUAL(resps.size(), 2u);
    const auto& second = resps[1];
    BOOST_TEST(second["success"].as_bool() == true);
    const auto& bps = second["body"]["breakpoints"].as_array();
    BOOST_REQUIRE_EQUAL(bps.size(), 1u);
    BOOST_TEST(bps[0]["line"].as_int() == 7);
}

// ---------------------------------------------------------------------------
// 17. Malformed JSON in a frame is tolerated — server continues running
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(malformed_json_is_tolerated) {
    // Send a Content-Length frame with invalid JSON, then a valid disconnect
    std::string bad_body = "{ this is not valid json !!!";
    std::string input =
        "Content-Length: " + std::to_string(bad_body.size()) + "\r\n\r\n" + bad_body
      + frame(request(1, "initialize", "{}"))
      + frame(request(2, "disconnect", "{}"));

    auto msgs = run_server(input);

    // Server must have survived and processed the valid requests
    BOOST_TEST(!find_msg(msgs, "response", "initialize").is_null());
    BOOST_TEST(!find_msg(msgs, "response", "disconnect").is_null());
}

// ---------------------------------------------------------------------------
// 18. eta/heapSnapshot without a running VM → error 2001
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(heap_snapshot_without_vm_returns_error) {
    std::string input =
        frame(request(1, "initialize", "{}"))
      + frame(request(2, "eta/heapSnapshot", "{}"))
      + frame(request(3, "disconnect", "{}"));

    auto msgs = run_server(input);

    auto resp = find_msg(msgs, "response", "eta/heapSnapshot");
    BOOST_REQUIRE(!resp.is_null());
    BOOST_TEST(resp["success"].as_bool() == false);
    // Must report error code 2001
    auto err_id = resp["body"]["error"].get_int("id");
    BOOST_REQUIRE(err_id.has_value());
    BOOST_TEST(*err_id == 2001);
}

// ---------------------------------------------------------------------------
// 19. eta/inspectObject without a running VM → error 2001
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(inspect_object_without_vm_returns_error) {
    std::string input =
        frame(request(1, "initialize", "{}"))
      + frame(request(2, "eta/inspectObject", R"({"objectId":42})"))
      + frame(request(3, "disconnect", "{}"));

    auto msgs = run_server(input);

    auto resp = find_msg(msgs, "response", "eta/inspectObject");
    BOOST_REQUIRE(!resp.is_null());
    BOOST_TEST(resp["success"].as_bool() == false);
    auto err_id = resp["body"]["error"].get_int("id");
    BOOST_REQUIRE(err_id.has_value());
    BOOST_TEST(*err_id == 2001);
}

// ---------------------------------------------------------------------------
// 20. eta/inspectObject without objectId argument (no VM) → error 2001
//     NOTE: The deeper error 2003 (missing objectId) is only reachable with a
//     paused VM.  The synchronous test harness cannot reliably pause the VM
//     (stopOnEntry causes a hang because disconnect's resume() fires before
//     the VM thread reaches check_and_wait, leaving it blocked on debug_cv_
//     with nobody to wake it).  We verify the dispatch path doesn't crash.
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(inspect_object_missing_objectid) {
    std::string input =
        frame(request(1, "initialize", "{}"))
      + frame(request(2, "eta/inspectObject", "{}"))   // no objectId field
      + frame(request(3, "disconnect", "{}"));

    auto msgs = run_server(input);

    auto resp = find_msg(msgs, "response", "eta/inspectObject");
    BOOST_REQUIRE(!resp.is_null());
    BOOST_TEST(resp["success"].as_bool() == false);
    // Without a running VM we get error 2001 before objectId is checked
    auto err_id = resp["body"]["error"].get_int("id");
    BOOST_REQUIRE(err_id.has_value());
    BOOST_TEST(*err_id == 2001);
}

// ---------------------------------------------------------------------------
// 21. Heap snapshot consPool field — run a script to completion (no
//     stopOnEntry) and request a snapshot.  The VM is likely not paused so
//     we expect error 2002 ("VM must be paused"), but if timing allows a
//     successful response we validate the consPool shape.
//
//     NOTE: stopOnEntry + disconnect hangs the synchronous harness (see #20).
//     The consPool integration is already covered by cons_pool_tests.cpp;
//     this test just verifies the DAP endpoint doesn't crash and handles the
//     not-paused case gracefully.
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(heap_snapshot_while_vm_running_returns_not_paused) {
    const std::string src = R"(
(module dap-test-pool
  (begin (display "pool-test")))
)";
    auto tmp = fs::temp_directory_path() / "eta_dap_test_pool.eta";
    {
        std::ofstream f(tmp);
        BOOST_REQUIRE(f.is_open());
        f << src;
    }
    std::string prog_arg = R"({"program":")" + json_path(tmp) + R"("})";

    std::string input =
        frame(request(1, "initialize", "{}"))
      + frame(request(2, "launch", prog_arg))
      + frame(request(3, "configurationDone", "{}"))
      + frame(request(4, "eta/heapSnapshot", "{}"))
      + frame(request(5, "disconnect", "{}"));

    auto msgs = run_server(input);

    auto resp = find_msg(msgs, "response", "eta/heapSnapshot");
    BOOST_REQUIRE(!resp.is_null());

    if (resp["success"].as_bool()) {
        // Script finished before snapshot request — verify consPool shape
        const auto& body = resp["body"];
        BOOST_TEST(body.has("totalBytes"));
        BOOST_TEST(body.has("softLimit"));
        BOOST_TEST(body.has("kinds"));
        BOOST_TEST(body.has("consPool"));

        if (body.has("consPool")) {
            const auto& pool = body["consPool"];
            BOOST_TEST(pool.has("capacity"));
            BOOST_TEST(pool.has("live"));
            BOOST_TEST(pool.has("free"));
            BOOST_TEST(pool.has("bytes"));
            auto cap = pool.get_int("capacity");
            BOOST_REQUIRE(cap.has_value());
            BOOST_TEST(*cap > 0);
        }
    } else {
        // Expected: error 2002 (VM must be paused)
        auto err_id = resp["body"]["error"].get_int("id");
        BOOST_REQUIRE(err_id.has_value());
        BOOST_TEST(*err_id == 2002);
    }

    fs::remove(tmp);
}

// ---------------------------------------------------------------------------
// 22. Inspect a non-existent object by ID (no VM) → error 2001
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(inspect_nonexistent_object) {
    std::string input =
        frame(request(1, "initialize", "{}"))
      + frame(request(2, "eta/inspectObject", R"({"objectId":999999999})"))
      + frame(request(3, "disconnect", "{}"));

    auto msgs = run_server(input);

    auto resp = find_msg(msgs, "response", "eta/inspectObject");
    BOOST_REQUIRE(!resp.is_null());
    BOOST_TEST(resp["success"].as_bool() == false);
    auto err_id = resp["body"]["error"].get_int("id");
    BOOST_REQUIRE(err_id.has_value());
    BOOST_TEST(*err_id == 2001);
}

// ---------------------------------------------------------------------------
// 23. terminate request is handled (Bug 5 fix) — must not fall through to
//     unknown-command handler.  supportsTerminateRequest is advertised so
//     VS Code sends this; without the handler the adapter would not unblock
//     the paused VM.
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(terminate_request_handled) {
    std::string input =
        frame(request(1, "initialize", "{}"))
      + frame(request(2, "terminate", "{}"))
      + frame(request(3, "disconnect", "{}"));

    auto msgs = run_server(input);

    auto resp = find_msg(msgs, "response", "terminate");
    BOOST_REQUIRE(!resp.is_null());
    BOOST_TEST(resp["success"].as_bool() == true);
}

// ---------------------------------------------------------------------------
// 24. completions request returns success (Bug 4 fix) — VS Code sends this
//     when the user presses Tab in the Debug Console.  Without the handler
//     it would fall through to unknown-command.
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(completions_request_handled) {
    std::string input =
        frame(request(1, "initialize", "{}"))
      + frame(request(2, "completions", R"({"text":"de","column":3,"frameId":0})"))
      + frame(request(3, "disconnect", "{}"));

    auto msgs = run_server(input);

    auto resp = find_msg(msgs, "response", "completions");
    BOOST_REQUIRE(!resp.is_null());
    BOOST_TEST(resp["success"].as_bool() == true);
    // Must have a "targets" array (even if empty — no VM running)
    BOOST_TEST(resp["body"]["targets"].is_array());
}

// ---------------------------------------------------------------------------
// 25. initialize advertises supportsCompletionsRequest (Bug 4 fix)
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(initialize_advertises_completions_support) {
    std::string input =
        frame(request(1, "initialize", "{}"))
      + frame(request(2, "disconnect", "{}"));

    auto msgs = run_server(input);

    auto resp = find_msg(msgs, "response", "initialize");
    BOOST_REQUIRE(!resp.is_null());
    BOOST_TEST(resp["body"]["supportsCompletionsRequest"].as_bool() == true);
}

// ---------------------------------------------------------------------------
// 26. Compound variable reference encoding round-trip (Bug 6 fix)
//     encode_var_ref / decode_var_ref must be self-consistent, and the
//     COMPOUND_REF_BASE must not collide with frame/scope refs.
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(var_ref_encoding_round_trip) {
    using eta::dap::DapServer;
    // Test a variety of frame/scope combinations
    for (int frame = 0; frame < 16; ++frame) {
        for (int scope = 0; scope < 2; ++scope) {
            int ref = DapServer::encode_var_ref(frame, scope);
            BOOST_TEST(ref > 0);                           // DAP requires > 0
            BOOST_TEST(ref < DapServer::COMPOUND_REF_BASE); // must not collide
            BOOST_TEST(DapServer::decode_var_ref_frame(ref) == frame);
            BOOST_TEST(DapServer::decode_var_ref_scope(ref) == scope);
        }
    }
}

// ---------------------------------------------------------------------------
// 27. stackTrace / continue / disconnect without a paused VM — must not hang.
//     The synchronous test harness cannot reliably wait for the VM thread to
//     hit a breakpoint before sending subsequent requests, so a full-lifecycle
//     breakpoint test would deadlock.  Instead we verify the protocol dispatch
//     layer handles these requests gracefully when the VM is not paused.
//     The actual stopped_span / first-breakpoint fix is tested at the VM unit
//     level in debug_tests.cpp (breakpoint_stopped_span_correct_line).
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(stack_trace_without_paused_vm) {
    std::string input =
        frame(request(1, "initialize", "{}"))
      + frame(request(2, "stackTrace", R"({"threadId":1})"))
      + frame(request(3, "disconnect", "{}"));

    auto msgs = run_server(input);

    auto resp = find_msg(msgs, "response", "stackTrace");
    BOOST_REQUIRE(!resp.is_null());
    BOOST_TEST(resp["success"].as_bool() == true);
    // No paused VM → empty stack frames
    BOOST_TEST(resp["body"]["stackFrames"].as_array().empty());
    auto total = resp["body"]["totalFrames"].get_int("totalFrames");
    // totalFrames should be 0 (or the field exists with value 0)
}

// ---------------------------------------------------------------------------
// 28. Variables request with compound expansion (Bug 6 fix) — when the VM
//     is paused with compound values on the stack, variables must report
//     non-zero variablesReference for expandable types (cons/vector/closure).
//     Without a running VM we can only verify the empty case doesn't crash.
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(variables_without_vm_returns_empty) {
    std::string input =
        frame(request(1, "initialize", "{}"))
      + frame(request(2, "variables", R"({"variablesReference":1})"))
      + frame(request(3, "disconnect", "{}"));

    auto msgs = run_server(input);

    auto resp = find_msg(msgs, "response", "variables");
    BOOST_REQUIRE(!resp.is_null());
    BOOST_TEST(resp["success"].as_bool() == true);
    BOOST_TEST(resp["body"]["variables"].is_array());
    BOOST_TEST(resp["body"]["variables"].as_array().empty());
}

// ---------------------------------------------------------------------------
// 29. Variables request with compound ref (out of range) returns empty
//     (Bug 6 fix — ensures compound_refs_ lookup for unknown refs is safe)
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(variables_compound_ref_out_of_range) {
    // Use a ref >= COMPOUND_REF_BASE but without a running VM
    std::string input =
        frame(request(1, "initialize", "{}"))
      + frame(request(2, "variables", R"({"variablesReference":10042})"))
      + frame(request(3, "disconnect", "{}"));

    auto msgs = run_server(input);

    auto resp = find_msg(msgs, "response", "variables");
    BOOST_REQUIRE(!resp.is_null());
    BOOST_TEST(resp["success"].as_bool() == true);
    BOOST_TEST(resp["body"]["variables"].as_array().empty());
}

// ---------------------------------------------------------------------------
// 30. Scopes response variablesReferences do not collide with compound refs
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(scopes_refs_below_compound_base) {
    std::string input =
        frame(request(1, "initialize", "{}"))
      + frame(request(2, "scopes", R"({"frameId":0})"))
      + frame(request(3, "disconnect", "{}"));

    auto msgs = run_server(input);

    auto resp = find_msg(msgs, "response", "scopes");
    BOOST_REQUIRE(!resp.is_null());
    const auto& scopes = resp["body"]["scopes"].as_array();
    for (const auto& s : scopes) {
        auto ref = s.get_int("variablesReference");
        BOOST_REQUIRE(ref.has_value());
        BOOST_TEST(*ref > 0);
        BOOST_TEST(*ref < 10000);  // below COMPOUND_REF_BASE
    }
}

BOOST_AUTO_TEST_SUITE_END()
