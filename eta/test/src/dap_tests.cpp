/**
 *
 * Tests exercise the DAP server in-process by injecting std::istringstream /
 * std::ostringstream instead of the real stdin/stdout pipe.  This lets us
 * verify the Content-Length framing, request dispatch, and response/event
 * correctness without spawning a subprocess.
 *
 * NOTE: BOOST_TEST_MODULE is defined once in eta_test.cpp for the whole binary.
 */

#include <boost/test/unit_test.hpp>

#include <chrono>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <mutex>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "eta/dap/dap_io.h"
#include "eta/dap/dap_server.h"
#include "eta/interpreter/driver.h"
#include "eta/util/json.h"

namespace fs   = std::filesystem;
namespace json = eta::json;

/**
 * Helpers
 */

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

/**
 * Find the first message matching type + command/event name.
 * Returns a null Value if not found.
 */
static json::Value find_msg(const std::vector<json::Value>& msgs,
                             const std::string& type,
                             const std::string& name) {
    const std::string key = (type == "response") ? "command" : "event";
    for (const auto& m : msgs) {
        auto t = m.get_string("type");
        auto n = m.get_string(key);
        if (t && n && *t == type && *n == name) return m;
    }
    return {};   ///< null Value
}

/**
 * Escape a filesystem path for safe embedding in a JSON string value.
 * Converts backslashes to \\\\ and double-quotes to \\".
 */
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

/**
 * Collect text from custom "eta-output" events (script stdout/stderr).
 * The DAP server sends these instead of standard "output" events so the
 * VS Code extension can route them to the "Eta Output" panel.
 * Body shape: { "stream": "stdout"|"stderr", "text": "..." }
 */
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

/**
 * Run the server synchronously with the given framed input and return all
 * parsed output messages.
 */
static std::vector<json::Value> run_server(const std::string& input) {
    std::istringstream in(input);
    std::ostringstream out;
    eta::dap::DapServer server(in, out);
    server.run();
    return parse_output(out.str());
}

class BlockingInputStreambuf final : public std::streambuf {
public:
    void push(std::string_view text) {
        {
            std::lock_guard<std::mutex> lk(mu_);
            for (char c : text) bytes_.push_back(c);
        }
        cv_.notify_all();
    }

    void close() {
        {
            std::lock_guard<std::mutex> lk(mu_);
            closed_ = true;
        }
        cv_.notify_all();
    }

protected:
    int_type underflow() override {
        std::unique_lock<std::mutex> lk(mu_);
        cv_.wait(lk, [&]() { return closed_ || !bytes_.empty(); });
        if (bytes_.empty()) return traits_type::eof();
        current_ = bytes_.front();
        bytes_.pop_front();
        setg(&current_, &current_, &current_ + 1);
        return traits_type::to_int_type(current_);
    }

private:
    std::mutex mu_;
    std::condition_variable cv_;
    std::deque<char> bytes_;
    bool closed_{false};
    char current_{'\0'};
};

class FramedOutputStreambuf final : public std::streambuf {
public:
    json::Value wait_next_matching(std::size_t& cursor,
                                   const std::function<bool(const json::Value&)>& pred,
                                   std::chrono::milliseconds timeout) {
        std::unique_lock<std::mutex> lk(mu_);

        auto try_find = [&]() -> json::Value {
            for (std::size_t i = cursor; i < messages_.size(); ++i) {
                if (pred(messages_[i])) {
                    cursor = i + 1;
                    return messages_[i];
                }
            }
            return {};
        };

        json::Value found = try_find();
        if (!found.is_null()) return found;

        const bool ready = cv_.wait_for(lk, timeout, [&]() {
            found = try_find();
            return !found.is_null();
        });
        if (!ready) return {};
        return found;
    }

protected:
    std::streamsize xsputn(const char_type* s, std::streamsize count) override {
        if (count <= 0) return 0;
        append(s, static_cast<std::size_t>(count));
        return count;
    }

    int_type overflow(int_type ch) override {
        if (traits_type::eq_int_type(ch, traits_type::eof())) return ch;
        const char c = traits_type::to_char_type(ch);
        append(&c, 1);
        return ch;
    }

private:
    void append(const char* data, std::size_t count) {
        std::lock_guard<std::mutex> lk(mu_);
        buffer_.append(data, count);

        bool parsed_message = false;
        while (true) {
            const std::size_t header_end = buffer_.find("\r\n\r\n");
            if (header_end == std::string::npos) break;

            std::size_t content_length = 0;
            bool has_content_length = false;
            std::istringstream hs(buffer_.substr(0, header_end));
            std::string line;
            while (std::getline(hs, line)) {
                if (!line.empty() && line.back() == '\r') line.pop_back();
                const std::string pfx = "Content-Length: ";
                if (line.rfind(pfx, 0) != 0) continue;
                try {
                    content_length = std::stoull(line.substr(pfx.size()));
                    has_content_length = true;
                } catch (const std::exception&) {
                    has_content_length = false;
                }
            }

            const std::size_t body_start = header_end + 4;
            if (!has_content_length || content_length == 0) {
                buffer_.erase(0, body_start);
                continue;
            }
            if (buffer_.size() < body_start + content_length) break;

            const std::string body = buffer_.substr(body_start, content_length);
            buffer_.erase(0, body_start + content_length);

            try {
                messages_.push_back(json::parse(body));
                parsed_message = true;
            } catch (const std::exception&) {
                // Ignore malformed payloads in the test harness.
            }
        }

        if (parsed_message) cv_.notify_all();
    }

    std::mutex mu_;
    std::condition_variable cv_;
    std::string buffer_;
    std::vector<json::Value> messages_;
};

class AsyncDapHarness {
public:
    AsyncDapHarness()
        : in_(&in_buf_),
          out_(&out_buf_) {
        server_thread_ = std::thread([this]() {
            eta::dap::DapServer server(in_, out_);
            server.run();
        });
    }

    ~AsyncDapHarness() {
        close();
    }

    void send(const std::string& json_body) {
        in_buf_.push(frame(json_body));
    }

    json::Value wait_response(const std::string& command,
                              std::chrono::milliseconds timeout = std::chrono::milliseconds(5000)) {
        return out_buf_.wait_next_matching(
            cursor_,
            [&](const json::Value& msg) {
                auto t = msg.get_string("type");
                auto c = msg.get_string("command");
                return t && c && *t == "response" && *c == command;
            },
            timeout);
    }

    json::Value wait_message(const std::function<bool(const json::Value&)>& pred,
                             std::chrono::milliseconds timeout = std::chrono::milliseconds(5000)) {
        return out_buf_.wait_next_matching(cursor_, pred, timeout);
    }

    json::Value wait_event(const std::string& event_name,
                           std::chrono::milliseconds timeout = std::chrono::milliseconds(5000)) {
        return out_buf_.wait_next_matching(
            cursor_,
            [&](const json::Value& msg) {
                auto t = msg.get_string("type");
                auto e = msg.get_string("event");
                return t && e && *t == "event" && *e == event_name;
            },
            timeout);
    }

    void close() {
        if (closed_) return;
        /**
         * Best-effort graceful shutdown so a paused VM does not block the
         * server thread's vm_thread_.join() path.
         */
        const std::string disconnect_body =
            R"({"seq":2147483000,"type":"request","command":"disconnect","arguments":{}})";
        in_buf_.push(frame(disconnect_body));
        (void) out_buf_.wait_next_matching(
            cursor_,
            [](const json::Value& msg) {
                auto t = msg.get_string("type");
                auto c = msg.get_string("command");
                return t && c && *t == "response" && *c == "disconnect";
            },
            std::chrono::milliseconds(300));

        closed_ = true;
        in_buf_.close();
        if (server_thread_.joinable()) server_thread_.join();
    }

private:
    BlockingInputStreambuf in_buf_;
    FramedOutputStreambuf out_buf_;
    std::istream in_;
    std::ostream out_;
    std::thread server_thread_;
    std::size_t cursor_{0};
    bool closed_{false};
};

/**
 * Test suite
 */

BOOST_AUTO_TEST_SUITE(dap_protocol)

/**
 *    "initialized" is sent after launch so VS Code knows script_path_ is set)
 */
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

    /**
     * "initialized" must NOT be present without a prior "launch" (we moved the
     * event to handle_launch to match the DAP spec and fix the 0-breakpoints bug)
     */
    auto evt = find_msg(msgs, "event", "initialized");
    BOOST_TEST(evt.is_null());
}

/**
 */
BOOST_AUTO_TEST_CASE(launch_sends_initialized_event) {
    std::string input =
        frame(request(1, "initialize", "{}"))
      + frame(request(2, "launch",
            R"({"program":"/tmp/nonexistent.eta","stopOnEntry":false})"))
      + frame(request(3, "disconnect", "{}"));

    auto msgs = run_server(input);

    /// "initialized" must be present and come AFTER the "launch" response
    auto evt = find_msg(msgs, "event", "initialized");
    BOOST_TEST(!evt.is_null());
}

/**
 * 2. launch stores the program path and responds with success
 */
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

/**
 *    with IDs assigned by the adapter
 */
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
    /// Before the VM has started, breakpoints are reported as not yet verified
    BOOST_TEST(bps[0]["verified"].as_bool() == false);
    BOOST_TEST(bps[0]["line"].as_int() == 5);
    BOOST_TEST(bps[1]["line"].as_int() == 10);
    /// Each breakpoint must have an integer ID assigned by the adapter
    BOOST_TEST(bps[0]["id"].is_int() == true);
    BOOST_TEST(bps[1]["id"].is_int() == true);
    /// IDs must be distinct
    BOOST_TEST(bps[0]["id"].as_int() != bps[1]["id"].as_int());
}

/**
 */
BOOST_AUTO_TEST_CASE(disconnect_exits_cleanly) {
    std::string input =
        frame(request(1, "initialize", "{}"))
      + frame(request(2, "disconnect", "{}"));

    auto msgs = run_server(input);

    auto resp = find_msg(msgs, "response", "disconnect");
    BOOST_REQUIRE(!resp.is_null());
    BOOST_TEST(resp["success"].as_bool() == true);
}

/**
 */
BOOST_AUTO_TEST_CASE(bad_program_path_sends_terminated) {
    std::string input =
        frame(request(1, "initialize", "{}"))
      + frame(request(2, "launch",
            R"({"program":"/tmp/eta_dap_test_nonexistent_XXXXX.eta"})"))
      + frame(request(3, "configurationDone", "{}"))
      + frame(request(4, "disconnect", "{}"));

    auto msgs = run_server(input);

    /// Must eventually send terminated
    auto term = find_msg(msgs, "event", "terminated");
    BOOST_TEST(!term.is_null());
    /// And an output/stderr event describing the failure
    auto errs = collect_output(msgs, "stderr");
    BOOST_TEST(!errs.empty());
}

/**
 */
BOOST_AUTO_TEST_CASE(script_output_captured_as_output_event) {
    /// Write a tiny script to a temp file.
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

    /**
     * The "hello-dap" text must appear in a custom "eta-output" event
     * Script output uses "eta-output" instead of standard "output" events
     * so the VS Code extension routes it to the dedicated "Eta Output" panel.
     */
    auto stdout_chunks = collect_eta_output(msgs, "stdout");
    std::string all_stdout;
    for (const auto& c : stdout_chunks) all_stdout += c;
    BOOST_TEST(all_stdout.find("hello-dap") != std::string::npos);

    /// terminated event must be present
    BOOST_TEST(!find_msg(msgs, "event", "terminated").is_null());

    fs::remove(tmp);
}

/**
 * 7. Sequence numbers are monotonically increasing in all server messages
 */
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

/**
 * 8. Unknown command gets an empty-success response (keeps VS Code happy)
 */
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

/**
 */
BOOST_AUTO_TEST_CASE(launch_without_program_sends_error) {
    std::string input =
        frame(request(1, "initialize", "{}"))
      + frame(request(2, "launch", "{}"))      ///< no "program" key
      + frame(request(3, "disconnect", "{}"));

    auto msgs = run_server(input);

    auto resp = find_msg(msgs, "response", "launch");
    BOOST_REQUIRE(!resp.is_null());
    BOOST_TEST(resp["success"].as_bool() == false);
    /// Must carry an error message
    auto fmt = resp["body"]["error"].get_string("format");
    BOOST_TEST(fmt.has_value());
    BOOST_TEST(fmt->find("program") != std::string::npos);
}

/**
 */
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

/**
 * 11. setExceptionBreakpoints is acknowledged with success
 */
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

/**
 */
BOOST_AUTO_TEST_CASE(configuration_done_before_launch_is_safe) {
    std::string input =
        frame(request(1, "initialize", "{}"))
      + frame(request(2, "configurationDone", "{}"))   ///< no launch first
      + frame(request(3, "disconnect", "{}"));

    auto msgs = run_server(input);

    auto resp = find_msg(msgs, "response", "configurationDone");
    BOOST_REQUIRE(!resp.is_null());
    BOOST_TEST(resp["success"].as_bool() == true);
    /// No vm thread should have started, so no terminated event
    BOOST_TEST(find_msg(msgs, "event", "terminated").is_null());
}

/**
 * 13. threads response has correct structure
 */
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

/**
 * 14. scopes response has Module + Locals + Upvalues + Globals entries
 */
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
    /// variablesReference values must all differ
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

/**
 */
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

/**
 * 16. Sending setBreakpoints twice for the same file replaces the first set
 */
BOOST_AUTO_TEST_CASE(set_breakpoints_replaces_previous_set) {
    std::string input =
        frame(request(1, "initialize", "{}"))
      + frame(request(2, "setBreakpoints",
            R"({"source":{"path":"/tmp/bp_test.eta"},)"
            R"("breakpoints":[{"line":1},{"line":2},{"line":3}]})"))
      + frame(request(3, "setBreakpoints",        ///< replace with just line 7
            R"({"source":{"path":"/tmp/bp_test.eta"},)"
            R"("breakpoints":[{"line":7}]})"))
      + frame(request(4, "disconnect", "{}"));

    auto msgs = run_server(input);

    /// Only the second response matters; it must have exactly one breakpoint
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

/**
 */
BOOST_AUTO_TEST_CASE(malformed_json_is_tolerated) {
    /// Send a Content-Length frame with invalid JSON, then a valid disconnect
    std::string bad_body = "{ this is not valid json !!!";
    std::string input =
        "Content-Length: " + std::to_string(bad_body.size()) + "\r\n\r\n" + bad_body
      + frame(request(1, "initialize", "{}"))
      + frame(request(2, "disconnect", "{}"));

    auto msgs = run_server(input);

    /// Server must have survived and processed the valid requests
    BOOST_TEST(!find_msg(msgs, "response", "initialize").is_null());
    BOOST_TEST(!find_msg(msgs, "response", "disconnect").is_null());
}

/**
 */
BOOST_AUTO_TEST_CASE(heap_snapshot_without_vm_returns_error) {
    std::string input =
        frame(request(1, "initialize", "{}"))
      + frame(request(2, "eta/heapSnapshot", "{}"))
      + frame(request(3, "disconnect", "{}"));

    auto msgs = run_server(input);

    auto resp = find_msg(msgs, "response", "eta/heapSnapshot");
    BOOST_REQUIRE(!resp.is_null());
    BOOST_TEST(resp["success"].as_bool() == false);
    /// Must report error code 2001
    auto err_id = resp["body"]["error"].get_int("id");
    BOOST_REQUIRE(err_id.has_value());
    BOOST_TEST(*err_id == 2001);
}

/**
 */
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

/**
 *     NOTE: The deeper error 2003 (missing objectId) is only reachable with a
 *     paused VM.  The synchronous test harness cannot reliably pause the VM
 *     (stopOnEntry causes a hang because disconnect's resume() fires before
 *     the VM thread reaches check_and_wait, leaving it blocked on debug_cv_
 *     with nobody to wake it).  We verify the dispatch path doesn't crash.
 */
BOOST_AUTO_TEST_CASE(inspect_object_missing_objectid) {
    std::string input =
        frame(request(1, "initialize", "{}"))
      + frame(request(2, "eta/inspectObject", "{}"))   ///< no objectId field
      + frame(request(3, "disconnect", "{}"));

    auto msgs = run_server(input);

    auto resp = find_msg(msgs, "response", "eta/inspectObject");
    BOOST_REQUIRE(!resp.is_null());
    BOOST_TEST(resp["success"].as_bool() == false);
    /// Without a running VM we get error 2001 before objectId is checked
    auto err_id = resp["body"]["error"].get_int("id");
    BOOST_REQUIRE(err_id.has_value());
    BOOST_TEST(*err_id == 2001);
}

/**
 *     stopOnEntry) and request a snapshot.  The VM is likely not paused so
 *     we expect error 2002 ("VM must be paused"), but if timing allows a
 *     successful response we validate the consPool shape.
 *
 *     NOTE: stopOnEntry + disconnect hangs the synchronous harness (see #20).
 *     The consPool integration is already covered by cons_pool_tests.cpp;
 *     this test just verifies the DAP endpoint doesn't crash and handles the
 *     not-paused case gracefully.
 */
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
        /// Expected: error 2002 (VM must be paused)
        auto err_id = resp["body"]["error"].get_int("id");
        BOOST_REQUIRE(err_id.has_value());
        BOOST_TEST(*err_id == 2002);
    }

    fs::remove(tmp);
}

/**
 */
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

/**
 *     unknown-command handler.  supportsTerminateRequest is advertised so
 *     VS Code sends this; without the handler the adapter would not unblock
 *     the paused VM.
 */
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

/**
 *     when the user presses Tab in the Debug Console.  Without the handler
 *     it would fall through to unknown-command.
 */
BOOST_AUTO_TEST_CASE(completions_request_handled) {
    std::string input =
        frame(request(1, "initialize", "{}"))
      + frame(request(2, "completions", R"({"text":"de","column":3,"frameId":0})"))
      + frame(request(3, "disconnect", "{}"));

    auto msgs = run_server(input);

    auto resp = find_msg(msgs, "response", "completions");
    BOOST_REQUIRE(!resp.is_null());
    BOOST_TEST(resp["success"].as_bool() == true);
    BOOST_TEST(resp["body"]["targets"].is_array());
}

/**
 * 25. initialize advertises supportsCompletionsRequest (Bug 4 fix)
 */
BOOST_AUTO_TEST_CASE(initialize_advertises_completions_support) {
    std::string input =
        frame(request(1, "initialize", "{}"))
      + frame(request(2, "disconnect", "{}"));

    auto msgs = run_server(input);

    auto resp = find_msg(msgs, "response", "initialize");
    BOOST_REQUIRE(!resp.is_null());
    BOOST_TEST(resp["body"]["supportsCompletionsRequest"].as_bool() == true);
}

/**
 * 26. initialize advertises supportsBreakpointLocationsRequest
 */
BOOST_AUTO_TEST_CASE(initialize_advertises_breakpoint_locations_support) {
    std::string input =
        frame(request(1, "initialize", "{}"))
      + frame(request(2, "disconnect", "{}"));

    auto msgs = run_server(input);

    auto resp = find_msg(msgs, "response", "initialize");
    BOOST_REQUIRE(!resp.is_null());
    BOOST_TEST(resp["body"]["supportsBreakpointLocationsRequest"].as_bool() == true);
}

/**
 * 27. initialize advertises supportsFunctionBreakpoints
 */
BOOST_AUTO_TEST_CASE(initialize_advertises_function_breakpoints_support) {
    std::string input =
        frame(request(1, "initialize", "{}"))
      + frame(request(2, "disconnect", "{}"));

    auto msgs = run_server(input);

    auto resp = find_msg(msgs, "response", "initialize");
    BOOST_REQUIRE(!resp.is_null());
    BOOST_TEST(resp["body"]["supportsFunctionBreakpoints"].as_bool() == true);
}

/**
 * 28. initialize advertises conditional/hit/log breakpoint support
 */
BOOST_AUTO_TEST_CASE(initialize_advertises_conditional_hit_log_breakpoint_support) {
    std::string input =
        frame(request(1, "initialize", "{}"))
      + frame(request(2, "disconnect", "{}"));

    auto msgs = run_server(input);

    auto resp = find_msg(msgs, "response", "initialize");
    BOOST_REQUIRE(!resp.is_null());
    BOOST_TEST(resp["body"]["supportsConditionalBreakpoints"].as_bool() == true);
    BOOST_TEST(resp["body"]["supportsHitConditionalBreakpoints"].as_bool() == true);
    BOOST_TEST(resp["body"]["supportsLogPoints"].as_bool() == true);
}

/**
 * 29. initialize advertises setVariable support
 */
BOOST_AUTO_TEST_CASE(initialize_advertises_set_variable_support) {
    std::string input =
        frame(request(1, "initialize", "{}"))
      + frame(request(2, "disconnect", "{}"));

    auto msgs = run_server(input);

    auto resp = find_msg(msgs, "response", "initialize");
    BOOST_REQUIRE(!resp.is_null());
    BOOST_TEST(resp["body"]["supportsSetVariable"].as_bool() == true);
}

/**
 * 30. initialize advertises restart support
 */
BOOST_AUTO_TEST_CASE(initialize_advertises_restart_support) {
    std::string input =
        frame(request(1, "initialize", "{}"))
      + frame(request(2, "disconnect", "{}"));

    auto msgs = run_server(input);

    auto resp = find_msg(msgs, "response", "initialize");
    BOOST_REQUIRE(!resp.is_null());
    BOOST_TEST(resp["body"]["supportsRestartRequest"].as_bool() == true);
}

/**
 * 31. initialize advertises standard disassemble support
 */
BOOST_AUTO_TEST_CASE(initialize_advertises_disassemble_support) {
    std::string input =
        frame(request(1, "initialize", "{}"))
      + frame(request(2, "disconnect", "{}"));

    auto msgs = run_server(input);

    auto resp = find_msg(msgs, "response", "initialize");
    BOOST_REQUIRE(!resp.is_null());
    BOOST_TEST(resp["body"]["supportsDisassembleRequest"].as_bool() == true);
}

/**
 * 32. initialize advertises cancel support
 */
BOOST_AUTO_TEST_CASE(initialize_advertises_cancel_support) {
    std::string input =
        frame(request(1, "initialize", "{}"))
      + frame(request(2, "disconnect", "{}"));

    auto msgs = run_server(input);

    auto resp = find_msg(msgs, "response", "initialize");
    BOOST_REQUIRE(!resp.is_null());
    BOOST_TEST(resp["body"]["supportsCancelRequest"].as_bool() == true);
}

/**
 * 33. initialize advertises terminateThreads support
 */
BOOST_AUTO_TEST_CASE(initialize_advertises_terminate_threads_support) {
    std::string input =
        frame(request(1, "initialize", "{}"))
      + frame(request(2, "disconnect", "{}"));

    auto msgs = run_server(input);

    auto resp = find_msg(msgs, "response", "initialize");
    BOOST_REQUIRE(!resp.is_null());
    BOOST_TEST(resp["body"]["supportsTerminateThreadsRequest"].as_bool() == true);
}

/**
 * 34. initialize advertises stepping granularity support
 */
BOOST_AUTO_TEST_CASE(initialize_advertises_stepping_granularity_support) {
    std::string input =
        frame(request(1, "initialize", "{}"))
      + frame(request(2, "disconnect", "{}"));

    auto msgs = run_server(input);

    auto resp = find_msg(msgs, "response", "initialize");
    BOOST_REQUIRE(!resp.is_null());
    BOOST_TEST(resp["body"]["supportsSteppingGranularity"].as_bool() == true);
}

/**
 * 35. terminateThreads request is accepted
 */
BOOST_AUTO_TEST_CASE(terminate_threads_request_is_acknowledged) {
    std::string input =
        frame(request(1, "initialize", "{}"))
      + frame(request(2, "terminateThreads", R"({"threadIds":[2,3]})"))
      + frame(request(3, "disconnect", "{}"));

    auto msgs = run_server(input);

    auto resp = find_msg(msgs, "response", "terminateThreads");
    BOOST_REQUIRE(!resp.is_null());
    BOOST_TEST(resp["success"].as_bool() == true);
}

/**
 * 36. next with instruction granularity is accepted
 */
BOOST_AUTO_TEST_CASE(next_instruction_granularity_without_vm_is_safe) {
    std::string input =
        frame(request(1, "initialize", "{}"))
      + frame(request(2, "next", R"({"threadId":1,"granularity":"instruction"})"))
      + frame(request(3, "disconnect", "{}"));

    auto msgs = run_server(input);

    auto resp = find_msg(msgs, "response", "next");
    BOOST_REQUIRE(!resp.is_null());
    BOOST_TEST(resp["success"].as_bool() == true);
}

/**
 * 37. disassemble without a running VM returns a structured error
 */
BOOST_AUTO_TEST_CASE(disassemble_without_vm_returns_error) {
    std::string input =
        frame(request(1, "initialize", "{}"))
      + frame(request(2, "disassemble",
            R"({"memoryReference":"0","instructionOffset":0,"instructionCount":16})"))
      + frame(request(3, "disconnect", "{}"));

    auto msgs = run_server(input);

    auto resp = find_msg(msgs, "response", "disassemble");
    BOOST_REQUIRE(!resp.is_null());
    BOOST_TEST(resp["success"].as_bool() == false);
    auto err_id = resp["body"]["error"].get_int("id");
    BOOST_REQUIRE(err_id.has_value());
    BOOST_TEST(*err_id == 2001);
}

/**
 * 38. restart before launch returns an error response
 */
BOOST_AUTO_TEST_CASE(restart_before_launch_returns_error) {
    std::string input =
        frame(request(1, "initialize", "{}"))
      + frame(request(2, "restart", "{}"))
      + frame(request(3, "disconnect", "{}"));

    auto msgs = run_server(input);

    auto resp = find_msg(msgs, "response", "restart");
    BOOST_REQUIRE(!resp.is_null());
    BOOST_TEST(resp["success"].as_bool() == false);
}

/**
 * 39. cancel request is acknowledged
 */
BOOST_AUTO_TEST_CASE(cancel_request_is_acknowledged) {
    std::string input =
        frame(request(1, "initialize", "{}"))
      + frame(request(2, "cancel", R"({"requestId":99})"))
      + frame(request(3, "disconnect", "{}"));

    auto msgs = run_server(input);

    auto resp = find_msg(msgs, "response", "cancel");
    BOOST_REQUIRE(!resp.is_null());
    BOOST_TEST(resp["success"].as_bool() == true);
}

/**
 * 40. pre-cancelled heap snapshot request returns cancellation error
 */
BOOST_AUTO_TEST_CASE(pre_cancelled_heap_snapshot_returns_cancelled_error) {
    std::string input =
        frame(request(1, "initialize", "{}"))
      + frame(request(2, "cancel", R"({"requestId":3})"))
      + frame(request(3, "eta/heapSnapshot", "{}"))
      + frame(request(4, "disconnect", "{}"));

    auto msgs = run_server(input);

    auto resp = find_msg(msgs, "response", "eta/heapSnapshot");
    BOOST_REQUIRE(!resp.is_null());
    BOOST_TEST(resp["success"].as_bool() == false);
    auto err_id = resp["body"]["error"].get_int("id");
    BOOST_REQUIRE(err_id.has_value());
    BOOST_TEST(*err_id == 2020);
}

/**
 * 41. setBreakpoints accepts condition/hitCondition/logMessage before launch
 */
BOOST_AUTO_TEST_CASE(set_breakpoints_with_condition_hit_and_log_before_launch) {
    std::string input =
        frame(request(1, "initialize", "{}"))
      + frame(request(2, "setBreakpoints",
            R"({"source":{"path":"/tmp/test.eta"},)"
            R"("breakpoints":[)"
            R"({"line":5,"condition":"counter","hitCondition":"3"},)"
            R"({"line":9,"logMessage":"counter={counter}"})"
            R"(]})"))
      + frame(request(3, "disconnect", "{}"));

    auto msgs = run_server(input);

    auto resp = find_msg(msgs, "response", "setBreakpoints");
    BOOST_REQUIRE(!resp.is_null());
    BOOST_TEST(resp["success"].as_bool() == true);
    const auto& bps = resp["body"]["breakpoints"].as_array();
    BOOST_REQUIRE_EQUAL(bps.size(), 2u);
    BOOST_TEST(bps[0]["line"].as_int() == 5);
    BOOST_TEST(bps[1]["line"].as_int() == 9);
    BOOST_TEST(bps[0]["verified"].as_bool() == false);
    BOOST_TEST(bps[1]["verified"].as_bool() == false);
}

/**
 * 42. setVariable without a running/paused VM returns a structured error
 */
BOOST_AUTO_TEST_CASE(set_variable_without_vm_returns_error) {
    std::string input =
        frame(request(1, "initialize", "{}"))
      + frame(request(2, "setVariable",
            R"({"variablesReference":1,"name":"x","value":"42"})"))
      + frame(request(3, "disconnect", "{}"));

    auto msgs = run_server(input);

    auto resp = find_msg(msgs, "response", "setVariable");
    BOOST_REQUIRE(!resp.is_null());
    BOOST_TEST(resp["success"].as_bool() == false);
    auto err_id = resp["body"]["error"].get_int("id");
    BOOST_REQUIRE(err_id.has_value());
    BOOST_TEST(*err_id == 2001);
}

/**
 * 43. setFunctionBreakpoints is accepted before launch
 */
BOOST_AUTO_TEST_CASE(set_function_breakpoints_before_launch) {
    std::string input =
        frame(request(1, "initialize", "{}"))
      + frame(request(2, "setFunctionBreakpoints",
            R"({"breakpoints":[{"name":"main"},{"name":"std.io.println"}]})"))
      + frame(request(3, "disconnect", "{}"));

    auto msgs = run_server(input);

    auto resp = find_msg(msgs, "response", "setFunctionBreakpoints");
    BOOST_REQUIRE(!resp.is_null());
    BOOST_TEST(resp["success"].as_bool() == true);

    const auto& bps = resp["body"]["breakpoints"].as_array();
    BOOST_REQUIRE_EQUAL(bps.size(), 2u);
    BOOST_TEST(bps[0]["id"].is_int());
    BOOST_TEST(bps[1]["id"].is_int());
    BOOST_TEST(bps[0]["verified"].as_bool() == false);
    BOOST_TEST(bps[1]["verified"].as_bool() == false);
}

/**
 * 29. breakpointLocations without an active VM returns an empty list
 */
BOOST_AUTO_TEST_CASE(breakpoint_locations_without_driver_returns_empty) {
    std::string input =
        frame(request(1, "initialize", "{}"))
      + frame(request(2, "breakpointLocations",
            R"({"source":{"path":"/tmp/example.eta"},"line":1,"endLine":200})"))
      + frame(request(3, "disconnect", "{}"));

    auto msgs = run_server(input);

    auto resp = find_msg(msgs, "response", "breakpointLocations");
    BOOST_REQUIRE(!resp.is_null());
    BOOST_TEST(resp["success"].as_bool() == true);
    BOOST_TEST(resp["body"]["breakpoints"].is_array());
    BOOST_TEST(resp["body"]["breakpoints"].as_array().empty());
}

/**
 * 30. Driver valid_lines_for collects executable lines from source maps
 */
BOOST_AUTO_TEST_CASE(driver_valid_lines_for_collects_source_lines) {
    const std::string src =
        "(module dap-valid-lines\n"
        "  (defun add1 (x)\n"
        "    (+ x 1))\n"
        "  (defun main ()\n"
        "    (add1 41)))\n";

    auto tmp = fs::temp_directory_path() / "eta_dap_valid_lines_test.eta";
    {
        std::ofstream f(tmp, std::ios::binary);
        BOOST_REQUIRE(f.is_open());
        f << src;
    }

    auto resolver = eta::interpreter::ModulePathResolver::from_args_or_env("");
    resolver.add_dir(tmp.parent_path());
    eta::interpreter::Driver driver(std::move(resolver));

    BOOST_REQUIRE(driver.run_file(tmp));

    const auto file_id = driver.file_id_for_path(tmp.string());
    BOOST_REQUIRE(file_id != 0);

    const auto lines = driver.valid_lines_for(file_id);
    BOOST_TEST(!lines.empty());
    const bool has_expected_line =
        lines.find(3u) != lines.end() || lines.find(5u) != lines.end();
    BOOST_TEST(has_expected_line);
    for (auto line : lines) {
        BOOST_TEST(line >= 1u);
        BOOST_TEST(line <= 5u);
    }

    fs::remove(tmp);
}

/**
 * 31. Compound variable reference encoding round-trip (Bug 6 fix)
 *     encode_var_ref / decode_var_ref must be self-consistent, and the
 *     COMPOUND_REF_BASE must not collide with frame/scope refs.
 */
BOOST_AUTO_TEST_CASE(var_ref_encoding_round_trip) {
    using eta::dap::DapServer;
    /// Test a variety of frame/scope combinations
    for (int frame = 0; frame < 16; ++frame) {
        for (int scope = 0; scope < 2; ++scope) {
            int ref = DapServer::encode_var_ref(frame, scope);
            BOOST_TEST(ref > 0);                           ///< DAP requires > 0
            BOOST_TEST(ref < DapServer::COMPOUND_REF_BASE); ///< must not collide
            BOOST_TEST(DapServer::decode_var_ref_frame(ref) == frame);
            BOOST_TEST(DapServer::decode_var_ref_scope(ref) == scope);
        }
    }
}

/**
 *     The synchronous test harness cannot reliably wait for the VM thread to
 *     hit a breakpoint before sending subsequent requests, so a full-lifecycle
 *     breakpoint test would deadlock.  Instead we verify the protocol dispatch
 *     layer handles these requests gracefully when the VM is not paused.
 *     The actual stopped_span / first-breakpoint fix is tested at the VM unit
 *     level in debug_tests.cpp (breakpoint_stopped_span_correct_line).
 */
BOOST_AUTO_TEST_CASE(stack_trace_without_paused_vm) {
    std::string input =
        frame(request(1, "initialize", "{}"))
      + frame(request(2, "stackTrace", R"({"threadId":1})"))
      + frame(request(3, "disconnect", "{}"));

    auto msgs = run_server(input);

    auto resp = find_msg(msgs, "response", "stackTrace");
    BOOST_REQUIRE(!resp.is_null());
    BOOST_TEST(resp["success"].as_bool() == true);
    BOOST_TEST(resp["body"]["stackFrames"].as_array().empty());
    auto total = resp["body"]["totalFrames"].get_int("totalFrames");
    /// totalFrames should be 0 (or the field exists with value 0)
}

/**
 *     is paused with compound values on the stack, variables must report
 *     non-zero variablesReference for expandable types (cons/vector/closure).
 *     Without a running VM we can only verify the empty case doesn't crash.
 */
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

/**
 * 29. Variables request with compound ref (out of range) returns empty
 */
BOOST_AUTO_TEST_CASE(variables_compound_ref_out_of_range) {
    /// Use a ref >= COMPOUND_REF_BASE but without a running VM
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

/**
 * 30. Scopes response variablesReferences do not collide with compound refs
 */
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
        BOOST_TEST(*ref < 10000);  ///< below COMPOUND_REF_BASE
    }
}

/**
 * 44. Async harness supports pause/continue interaction with stopOnEntry
 */
BOOST_AUTO_TEST_CASE(async_harness_stop_on_entry_continue_round_trip) {
    const auto tmp = fs::temp_directory_path() / "eta_dap_async_harness_test.eta";
    struct TempFileCleanup {
        fs::path path;
        ~TempFileCleanup() {
            std::error_code ec;
            fs::remove(path, ec);
        }
    } cleanup{tmp};

    {
        std::ofstream f(tmp, std::ios::binary);
        BOOST_REQUIRE(f.is_open());
        f << "(module dap-async-harness\n"
             "  (defun main ()\n"
             "    (let ((x 41))\n"
             "      (+ x 1))))\n";
    }

    AsyncDapHarness harness;

    harness.send(request(1, "initialize", "{}"));
    auto init_resp = harness.wait_response("initialize");
    BOOST_REQUIRE(!init_resp.is_null());
    BOOST_TEST(init_resp["success"].as_bool() == true);

    const std::string launch_args =
        std::string(R"({"program":")") + json_path(tmp) + R"(","stopOnEntry":true})";
    harness.send(request(2, "launch", launch_args));
    auto launch_resp = harness.wait_response("launch");
    BOOST_REQUIRE(!launch_resp.is_null());
    BOOST_TEST(launch_resp["success"].as_bool() == true);

    auto initialized_evt = harness.wait_event("initialized");
    BOOST_REQUIRE(!initialized_evt.is_null());

    harness.send(request(3, "configurationDone", "{}"));
    auto config_done_resp = harness.wait_response("configurationDone");
    BOOST_REQUIRE(!config_done_resp.is_null());
    BOOST_TEST(config_done_resp["success"].as_bool() == true);

    auto stopped_evt = harness.wait_event("stopped");
    BOOST_REQUIRE(!stopped_evt.is_null());
    BOOST_TEST(stopped_evt["body"]["threadId"].as_int() == 1);

    harness.send(request(4, "continue", R"({"threadId":1})"));
    auto continue_resp = harness.wait_response("continue");
    BOOST_REQUIRE(!continue_resp.is_null());
    BOOST_TEST(continue_resp["success"].as_bool() == true);

    auto terminated_evt = harness.wait_event("terminated", std::chrono::milliseconds(10000));
    BOOST_REQUIRE(!terminated_evt.is_null());

    harness.send(request(5, "disconnect", "{}"));
    auto disconnect_resp = harness.wait_response("disconnect");
    BOOST_REQUIRE(!disconnect_resp.is_null());
    BOOST_TEST(disconnect_resp["success"].as_bool() == true);
}

/**
 * 45. Async conditional breakpoint flow pauses exactly on the truthy hit
 */
BOOST_AUTO_TEST_CASE(async_conditional_breakpoint_pauses_on_truthy_hit) {
    const auto tmp = fs::temp_directory_path() / "eta_dap_async_conditional_bp_test.eta";
    struct TempFileCleanup {
        fs::path path;
        ~TempFileCleanup() {
            std::error_code ec;
            fs::remove(path, ec);
        }
    } cleanup{tmp};

    {
        std::ofstream f(tmp, std::ios::binary);
        BOOST_REQUIRE(f.is_open());
        f << "(module dap-async-conditional-bp\n"
             "  (defun bp-target (flag n)\n"
             "    (if flag\n"
             "        (+ n 1)\n"
             "        (+ n 2)))\n"
             "  (defun main ()\n"
             "    (bp-target #f 1)\n"
             "    (bp-target #t 2)\n"
             "    0))\n";
    }

    int bp_line = 3;
    {
        auto resolver = eta::interpreter::ModulePathResolver::from_args_or_env("");
        resolver.add_dir(tmp.parent_path());
        eta::interpreter::Driver driver(std::move(resolver));
        BOOST_REQUIRE(driver.run_file(tmp));
        const auto file_id = driver.file_id_for_path(tmp.string());
        BOOST_REQUIRE(file_id != 0);
        const auto valid_lines = driver.valid_lines_for(file_id);
        if (valid_lines.find(3u) != valid_lines.end()) {
            bp_line = 3;
        } else if (valid_lines.find(4u) != valid_lines.end()) {
            bp_line = 4;
        } else {
            BOOST_FAIL("expected executable line for conditional breakpoint at line 3 or 4");
        }
    }

    AsyncDapHarness harness;

    harness.send(request(1, "initialize", "{}"));
    auto init_resp = harness.wait_response("initialize");
    BOOST_REQUIRE(!init_resp.is_null());
    BOOST_TEST(init_resp["success"].as_bool() == true);

    const std::string launch_args =
        std::string(R"({"program":")") + json_path(tmp) + R"(","stopOnEntry":false})";
    harness.send(request(2, "launch", launch_args));
    auto launch_resp = harness.wait_response("launch");
    BOOST_REQUIRE(!launch_resp.is_null());
    BOOST_TEST(launch_resp["success"].as_bool() == true);
    BOOST_REQUIRE(!harness.wait_event("initialized").is_null());

    const std::string set_bp_args =
        std::string(R"({"source":{"path":")") + json_path(tmp)
      + R"("},"breakpoints":[{"line":)" + std::to_string(bp_line)
      + R"(,"condition":"flag"}]})";
    harness.send(request(3, "setBreakpoints", set_bp_args));
    auto set_bp_resp = harness.wait_response("setBreakpoints");
    BOOST_REQUIRE(!set_bp_resp.is_null());
    BOOST_TEST(set_bp_resp["success"].as_bool() == true);
    BOOST_REQUIRE_EQUAL(set_bp_resp["body"]["breakpoints"].as_array().size(), 1u);

    harness.send(request(4, "configurationDone", "{}"));
    auto config_done_resp = harness.wait_response("configurationDone");
    BOOST_REQUIRE(!config_done_resp.is_null());
    BOOST_TEST(config_done_resp["success"].as_bool() == true);

    auto stopped_evt = harness.wait_event("stopped", std::chrono::milliseconds(10000));
    BOOST_REQUIRE(!stopped_evt.is_null());
    BOOST_TEST(stopped_evt["body"].get_string("reason").value_or("") == "breakpoint");

    harness.send(request(5, "stackTrace", R"({"threadId":1})"));
    auto stack_resp = harness.wait_response("stackTrace");
    BOOST_REQUIRE(!stack_resp.is_null());
    BOOST_TEST(stack_resp["success"].as_bool() == true);
    const auto& frames = stack_resp["body"]["stackFrames"].as_array();
    BOOST_REQUIRE(!frames.empty());
    bool frame_in_script = false;
    for (const auto& frame : frames) {
        auto path = frame["source"].get_string("path");
        if (!path) continue;
        if (*path == tmp.string()) {
            frame_in_script = true;
            break;
        }
    }
    BOOST_TEST(frame_in_script);

    int seq = 6;
    harness.send(request(seq++, "continue", R"({"threadId":1})"));
    auto continue_resp = harness.wait_response("continue");
    BOOST_REQUIRE(!continue_resp.is_null());
    BOOST_TEST(continue_resp["success"].as_bool() == true);

    bool terminated = false;
    for (int attempts = 0; attempts < 6 && !terminated; ++attempts) {
        auto evt = harness.wait_message([](const json::Value& m) {
            auto t = m.get_string("type");
            if (!t || *t != "event") return false;
            auto e = m.get_string("event");
            return e && (*e == "stopped" || *e == "terminated");
        }, std::chrono::milliseconds(10000));
        BOOST_REQUIRE(!evt.is_null());

        auto name = evt.get_string("event");
        BOOST_REQUIRE(name.has_value());
        if (*name == "terminated") {
            terminated = true;
            break;
        }

        harness.send(request(seq++, "continue", R"({"threadId":1})"));
        auto more_continue = harness.wait_response("continue");
        BOOST_REQUIRE(!more_continue.is_null());
        BOOST_TEST(more_continue["success"].as_bool() == true);
    }
    BOOST_TEST(terminated);

    harness.send(request(seq++, "disconnect", "{}"));
    auto disconnect_resp = harness.wait_response("disconnect");
    BOOST_REQUIRE(!disconnect_resp.is_null());
    BOOST_TEST(disconnect_resp["success"].as_bool() == true);
}

/**
 * 46. Async logpoint flow emits output and does not pause execution
 */
BOOST_AUTO_TEST_CASE(async_logpoint_emits_output_without_stopping) {
    const auto tmp = fs::temp_directory_path() / "eta_dap_async_logpoint_test.eta";
    struct TempFileCleanup {
        fs::path path;
        ~TempFileCleanup() {
            std::error_code ec;
            fs::remove(path, ec);
        }
    } cleanup{tmp};

    {
        std::ofstream f(tmp, std::ios::binary);
        BOOST_REQUIRE(f.is_open());
        f << "(module dap-async-logpoint\n"
             "  (defun log-loop (i)\n"
             "    (if (< i 3)\n"
             "        (log-loop (+ i 1))\n"
             "        i))\n"
             "  (defun main ()\n"
             "    (log-loop 0)\n"
             "    0))\n";
    }

    AsyncDapHarness harness;

    harness.send(request(1, "initialize", "{}"));
    auto init_resp = harness.wait_response("initialize");
    BOOST_REQUIRE(!init_resp.is_null());
    BOOST_TEST(init_resp["success"].as_bool() == true);

    const std::string launch_args =
        std::string(R"({"program":")") + json_path(tmp) + R"(","stopOnEntry":false})";
    harness.send(request(2, "launch", launch_args));
    auto launch_resp = harness.wait_response("launch");
    BOOST_REQUIRE(!launch_resp.is_null());
    BOOST_TEST(launch_resp["success"].as_bool() == true);
    BOOST_REQUIRE(!harness.wait_event("initialized").is_null());

    const std::string set_bp_args =
        std::string(R"({"source":{"path":")") + json_path(tmp)
      + R"("},"breakpoints":[{"line":3,"logMessage":"loop i={i}"}]})";
    harness.send(request(3, "setBreakpoints", set_bp_args));
    auto set_bp_resp = harness.wait_response("setBreakpoints");
    BOOST_REQUIRE(!set_bp_resp.is_null());
    BOOST_TEST(set_bp_resp["success"].as_bool() == true);
    BOOST_REQUIRE_EQUAL(set_bp_resp["body"]["breakpoints"].as_array().size(), 1u);

    harness.send(request(4, "configurationDone", "{}"));
    auto config_done_resp = harness.wait_response("configurationDone");
    BOOST_REQUIRE(!config_done_resp.is_null());
    BOOST_TEST(config_done_resp["success"].as_bool() == true);

    int log_hits = 0;
    bool terminated = false;
    for (int i = 0; i < 64 && !terminated; ++i) {
        auto msg = harness.wait_message([](const json::Value& m) {
            auto t = m.get_string("type");
            if (!t || *t != "event") return false;
            auto e = m.get_string("event");
            if (!e) return false;
            return *e == "output" || *e == "stopped" || *e == "terminated";
        }, std::chrono::milliseconds(10000));
        BOOST_REQUIRE(!msg.is_null());

        auto evt = msg.get_string("event");
        BOOST_REQUIRE(evt.has_value());
        if (*evt == "stopped") {
            BOOST_FAIL("logpoint breakpoint unexpectedly paused execution");
        } else if (*evt == "output") {
            auto text = msg["body"].get_string("output");
            if (text && text->find("loop i=") != std::string::npos) {
                ++log_hits;
            }
        } else if (*evt == "terminated") {
            terminated = true;
        }
    }

    BOOST_TEST(terminated);
    BOOST_TEST(log_hits >= 3);

    harness.send(request(5, "disconnect", "{}"));
    auto disconnect_resp = harness.wait_response("disconnect");
    BOOST_REQUIRE(!disconnect_resp.is_null());
    BOOST_TEST(disconnect_resp["success"].as_bool() == true);
}

/**
 * 47. Async function breakpoint flow pauses at function entry
 */
BOOST_AUTO_TEST_CASE(async_function_breakpoint_pauses_on_function_entry) {
    const auto tmp = fs::temp_directory_path() / "eta_dap_async_function_bp_test.eta";
    struct TempFileCleanup {
        fs::path path;
        ~TempFileCleanup() {
            std::error_code ec;
            fs::remove(path, ec);
        }
    } cleanup{tmp};

    {
        std::ofstream f(tmp, std::ios::binary);
        BOOST_REQUIRE(f.is_open());
        f << "(module dap-async-function-bp\n"
             "  (defun func-bp-target-async (x)\n"
             "    (+ x 1))\n"
             "  (defun main ()\n"
             "    (let ((v 41))\n"
             "      (func-bp-target-async v))\n"
             "    0))\n";
    }

    int setup_bp_line = 5;
    {
        auto resolver = eta::interpreter::ModulePathResolver::from_args_or_env("");
        resolver.add_dir(tmp.parent_path());
        eta::interpreter::Driver driver(std::move(resolver));
        BOOST_REQUIRE(driver.run_file(tmp));
        const auto file_id = driver.file_id_for_path(tmp.string());
        BOOST_REQUIRE(file_id != 0);
        const auto valid_lines = driver.valid_lines_for(file_id);
        if (valid_lines.find(5u) != valid_lines.end()) {
            setup_bp_line = 5;
        } else if (valid_lines.find(6u) != valid_lines.end()) {
            setup_bp_line = 6;
        } else {
            BOOST_FAIL("expected executable setup breakpoint line at line 5 or 6");
        }
    }

    AsyncDapHarness harness;

    harness.send(request(1, "initialize", "{}"));
    auto init_resp = harness.wait_response("initialize");
    BOOST_REQUIRE(!init_resp.is_null());
    BOOST_TEST(init_resp["success"].as_bool() == true);

    const std::string launch_args =
        std::string(R"({"program":")") + json_path(tmp) + R"(","stopOnEntry":false})";
    harness.send(request(2, "launch", launch_args));
    auto launch_resp = harness.wait_response("launch");
    BOOST_REQUIRE(!launch_resp.is_null());
    BOOST_TEST(launch_resp["success"].as_bool() == true);
    BOOST_REQUIRE(!harness.wait_event("initialized").is_null());

    const std::string set_line_bp_args =
        std::string(R"({"source":{"path":")") + json_path(tmp)
      + R"("},"breakpoints":[{"line":)" + std::to_string(setup_bp_line) + R"(}]})";
    harness.send(request(3, "setBreakpoints", set_line_bp_args));
    auto set_line_bp_resp = harness.wait_response("setBreakpoints");
    BOOST_REQUIRE(!set_line_bp_resp.is_null());
    BOOST_TEST(set_line_bp_resp["success"].as_bool() == true);

    harness.send(request(4, "configurationDone", "{}"));
    auto config_done_resp = harness.wait_response("configurationDone");
    BOOST_REQUIRE(!config_done_resp.is_null());
    BOOST_TEST(config_done_resp["success"].as_bool() == true);

    auto setup_stop_evt = harness.wait_event("stopped", std::chrono::milliseconds(10000));
    BOOST_REQUIRE(!setup_stop_evt.is_null());

    harness.send(request(5, "setFunctionBreakpoints",
        R"({"breakpoints":[{"name":"func-bp-target-async"}]})"));
    auto set_bp_resp = harness.wait_response("setFunctionBreakpoints");
    BOOST_REQUIRE(!set_bp_resp.is_null());
    BOOST_TEST(set_bp_resp["success"].as_bool() == true);
    BOOST_REQUIRE_EQUAL(set_bp_resp["body"]["breakpoints"].as_array().size(), 1u);
    /**
     * Capture the line where the function breakpoint was actually resolved.
     * Function names emitted by the compiler include a synthesised `_lambda<N>`
     * suffix, so verifying the stop happened "in func-bp-target-async" is done
     * by matching the stack frame's source file + resolved line, rather than
     * by name.
     */
    const auto& fn_bps = set_bp_resp["body"]["breakpoints"].as_array();
    BOOST_REQUIRE(fn_bps[0].is_object());
    BOOST_REQUIRE(fn_bps[0]["verified"].is_bool());
    BOOST_TEST(fn_bps[0]["verified"].as_bool() == true);
    auto fn_bp_line_opt = fn_bps[0].get_int("line");
    BOOST_REQUIRE(fn_bp_line_opt.has_value());
    const int fn_bp_line = static_cast<int>(*fn_bp_line_opt);

    const std::string clear_line_bp_args =
        std::string(R"({"source":{"path":")") + json_path(tmp)
      + R"("},"breakpoints":[]})";
    harness.send(request(6, "setBreakpoints", clear_line_bp_args));
    auto clear_line_bp_resp = harness.wait_response("setBreakpoints");
    BOOST_REQUIRE(!clear_line_bp_resp.is_null());
    BOOST_TEST(clear_line_bp_resp["success"].as_bool() == true);

    harness.send(request(7, "continue", R"({"threadId":1})"));
    auto continue_to_func_resp = harness.wait_response("continue");
    BOOST_REQUIRE(!continue_to_func_resp.is_null());
    BOOST_TEST(continue_to_func_resp["success"].as_bool() == true);

    auto stopped_evt = harness.wait_event("stopped", std::chrono::milliseconds(10000));
    BOOST_REQUIRE(!stopped_evt.is_null());
    BOOST_TEST(stopped_evt["body"].get_string("reason").value_or("") == "breakpoint");

    harness.send(request(8, "stackTrace", R"({"threadId":1})"));
    auto stack_resp = harness.wait_response("stackTrace");
    BOOST_REQUIRE(!stack_resp.is_null());
    BOOST_TEST(stack_resp["success"].as_bool() == true);
    const auto& frames = stack_resp["body"]["stackFrames"].as_array();
    BOOST_REQUIRE(!frames.empty());
    /**
     * The innermost frame should be inside the body of func-bp-target-async,
     * which is identified here by (source path == tmp) AND (line == resolved bp line).
     */
    bool has_target_frame = false;
    auto top_path = frames.front()["source"].get_string("path");
    auto top_line = frames.front().get_int("line");
    if (top_path && top_line && *top_path == tmp.string()
        && static_cast<int>(*top_line) == fn_bp_line) {
        has_target_frame = true;
    }
    BOOST_TEST(has_target_frame);

    /**
     * The function breakpoint may match several consecutive instructions on
     * the same source line. Loop continuing across any further `stopped`
     * events until we observe `terminated`.
     */
    int seq = 9;
    bool terminated = false;
    harness.send(request(seq++, "continue", R"({"threadId":1})"));
    auto continue_resp = harness.wait_response("continue");
    BOOST_REQUIRE(!continue_resp.is_null());
    BOOST_TEST(continue_resp["success"].as_bool() == true);
    for (int attempts = 0; attempts < 16 && !terminated; ++attempts) {
        auto evt = harness.wait_message([](const json::Value& m) {
            auto t = m.get_string("type");
            if (!t || *t != "event") return false;
            auto e = m.get_string("event");
            return e && (*e == "stopped" || *e == "terminated");
        }, std::chrono::milliseconds(10000));
        BOOST_REQUIRE(!evt.is_null());

        auto name = evt.get_string("event");
        BOOST_REQUIRE(name.has_value());
        if (*name == "terminated") {
            terminated = true;
            break;
        }

        harness.send(request(seq++, "continue", R"({"threadId":1})"));
        auto more_continue = harness.wait_response("continue");
        BOOST_REQUIRE(!more_continue.is_null());
        BOOST_TEST(more_continue["success"].as_bool() == true);
    }
    BOOST_TEST(terminated);

    harness.send(request(seq++, "disconnect", "{}"));
    auto disconnect_resp = harness.wait_response("disconnect");
    BOOST_REQUIRE(!disconnect_resp.is_null());
    BOOST_TEST(disconnect_resp["success"].as_bool() == true);
}

BOOST_AUTO_TEST_SUITE_END()


BOOST_AUTO_TEST_SUITE(dap_framing_robustness)

/// Malformed Content-Length must not throw; returns nullopt.
BOOST_AUTO_TEST_CASE(malformed_content_length_returns_nullopt) {
    std::istringstream in("Content-Length: not-a-number\r\n\r\n{}");
    BOOST_CHECK_NO_THROW({
        auto result = eta::dap::read_message(in);
        BOOST_CHECK(!result.has_value());
    });
}

/// Empty Content-Length value must not throw.
BOOST_AUTO_TEST_CASE(empty_content_length_returns_nullopt) {
    std::istringstream in("Content-Length: \r\n\r\n{}");
    BOOST_CHECK_NO_THROW({
        auto result = eta::dap::read_message(in);
        BOOST_CHECK(!result.has_value());
    });
}

/// Over-limit Content-Length must not allocate / crash; returns nullopt.
BOOST_AUTO_TEST_CASE(overlimit_content_length_returns_nullopt) {
    std::istringstream in("Content-Length: 4294967295\r\n\r\n");
    BOOST_CHECK_NO_THROW({
        auto result = eta::dap::read_message(in);
        BOOST_CHECK(!result.has_value());
    });
}

/// Zero Content-Length skips and returns nullopt (existing behaviour).
BOOST_AUTO_TEST_CASE(zero_content_length_returns_nullopt) {
    std::istringstream in("Content-Length: 0\r\n\r\n");
    auto result = eta::dap::read_message(in);
    BOOST_CHECK(!result.has_value());
}

/// A valid message is still read correctly after a skipped zero-length one.
BOOST_AUTO_TEST_CASE(valid_message_after_skipped_zero_length) {
    const std::string body = R"({"seq":1,"type":"request","command":"initialize","arguments":{}})";
    std::istringstream in(
        "Content-Length: 0\r\n\r\n"          ///< skipped
      + frame(body));
    auto result = eta::dap::read_message(in);
    BOOST_REQUIRE(result.has_value());
    BOOST_CHECK_EQUAL(*result, body);
}

BOOST_AUTO_TEST_SUITE_END() ///< dap_framing_robustness


