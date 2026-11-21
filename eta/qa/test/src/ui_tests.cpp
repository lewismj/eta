/**
 * @file ui_tests.cpp
 * @brief Tests for the std.ui native layer (packages/stdlib/native/ui).
 *
 *  - FrameWriter: Eta value -> protocol frame (JSON + Float64 buffers)
 *  - framing helpers, app-id sanitising, MIME types
 *  - `%ui-*` primitives registered in a BuiltinEnvironment
 *  - a live UiServer: static files over HTTP, WebSocket frames both ways,
 *    token rejection, retained snapshot replay and the rerun takeover
 */

#include <boost/test/unit_test.hpp>

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <nng/nng.h>
#include <nng/supplemental/http/http.h>

#include <eta/runtime/builtin_env.h>
#include <eta/runtime/core_primitives.h>
#include <eta/runtime/factory.h>
#include <eta/runtime/memory/heap.h>
#include <eta/runtime/memory/intern_table.h>
#include <eta/runtime/nanbox.h>
#include <eta/runtime/string_view.h>
#include <eta/ui/ui_primitives.h>
#include <eta/ui/ui_server.h>
#include <eta/util/json.h>

#include "ui_test_support.h"

using namespace eta::runtime;
using namespace eta::runtime::nanbox;
using namespace eta::runtime::memory::heap;
using namespace eta::runtime::memory::intern;
using namespace eta::runtime::memory::factory;
using namespace eta::runtime::error;
namespace fs = std::filesystem;
namespace ui = eta::ui;
using namespace eta_ui_test;

namespace {

struct Values {
    Heap heap{1ull << 22};
    InternTable intern;
    BuiltinEnvironment env;

    Values() { register_core_primitives(env, heap, intern, nullptr); }

    LispVal fix(std::int64_t n) {
        auto v = make_fixnum(heap, n);
        BOOST_REQUIRE(v.has_value());
        return *v;
    }
    LispVal flo(double d) {
        auto v = make_flonum(d);
        BOOST_REQUIRE(v.has_value());
        return *v;
    }
    LispVal str(const std::string& s) {
        auto v = make_string(heap, intern, s);
        BOOST_REQUIRE(v.has_value());
        return *v;
    }
    LispVal sym(const std::string& s) {
        auto v = make_symbol(intern, s);
        BOOST_REQUIRE(v.has_value());
        return *v;
    }
    LispVal vec(std::vector<LispVal> xs) {
        auto v = make_vector(heap, std::move(xs));
        BOOST_REQUIRE(v.has_value());
        return *v;
    }
    LispVal list(const std::vector<LispVal>& xs) {
        LispVal out = Nil;
        for (auto it = xs.rbegin(); it != xs.rend(); ++it) {
            auto c = make_cons(heap, *it, out);
            BOOST_REQUIRE(c.has_value());
            out = *c;
        }
        return out;
    }
    LispVal call(const std::string& name, const std::vector<LispVal>& args) {
        auto idx = env.lookup(name);
        BOOST_REQUIRE_MESSAGE(idx.has_value(), "missing builtin " + name);
        auto r = env.specs()[*idx].func(args);
        BOOST_REQUIRE_MESSAGE(r.has_value(), "call failed: " + name);
        return *r;
    }
};

DecodedFrame write_frame(Values& v, LispVal value) {
    ui::detail::FrameWriter w(v.heap, v.intern);
    auto ok = w.write(value);
    BOOST_REQUIRE_MESSAGE(ok.has_value(), "FrameWriter::write failed");
    return decode(*w.finish());
}

// ---- environment helpers for the live-server tests -----------------------

} // namespace

// ===========================================================================

BOOST_AUTO_TEST_SUITE(ui_frame_writer_tests)

BOOST_AUTO_TEST_CASE(scalars_strings_and_symbols) {
    Values v;
    auto f = write_frame(v, v.list({v.fix(42), v.flo(1.5), True, False, v.str("a\"b\n"), v.sym("sym")}));
    const auto& arr = f.body().as_array();
    BOOST_REQUIRE_EQUAL(arr.size(), 6u);
    BOOST_TEST(arr[0].as_int() == 42);
    BOOST_TEST(arr[1].as_double() == 1.5);
    BOOST_TEST(arr[2].as_bool() == true);
    BOOST_TEST(arr[3].as_bool() == false);
    BOOST_TEST(arr[4].as_string() == "a\"b\n");
    BOOST_TEST(arr[5].as_string() == "sym");
    BOOST_TEST(f.header["buffers"].as_array().empty());
}

BOOST_AUTO_TEST_CASE(empty_list_is_empty_array_and_nonfinite_is_null) {
    Values v;
    auto f = write_frame(v, v.vec({Nil, v.flo(std::numeric_limits<double>::infinity())}));
    const auto& arr = f.body().as_array();
    BOOST_TEST(arr[0].is_array());
    BOOST_TEST(arr[0].as_array().empty());
    BOOST_TEST(arr[1].is_null());
}

BOOST_AUTO_TEST_CASE(hash_maps_become_objects) {
    Values v;
    auto map = v.call("hash-map", {v.str("type"), v.str("render"), v.sym("rev"), v.fix(3)});
    auto f = write_frame(v, map);
    BOOST_TEST(f.body()["type"].as_string() == "render");
    BOOST_TEST(f.body()["rev"].as_int() == 3);
}

BOOST_AUTO_TEST_CASE(short_numeric_vectors_stay_json) {
    Values v;
    std::vector<LispVal> xs;
    for (int i = 0; i < 15; ++i) xs.push_back(v.flo(i * 0.5));
    auto f = write_frame(v, v.vec(xs));
    BOOST_TEST(f.body().is_array());
    BOOST_TEST(f.body().as_array().size() == 15u);
    BOOST_TEST(f.header["buffers"].as_array().empty());
}

BOOST_AUTO_TEST_CASE(long_numeric_vectors_become_aligned_f64_buffers) {
    Values v;
    std::vector<LispVal> xs, ys;
    for (int i = 0; i < 40; ++i) xs.push_back(v.flo(i * 0.25));
    for (int i = 0; i < 17; ++i) ys.push_back(v.fix(i * 3));  // fixnums widen to f64
    auto map = v.call("hash-map", {v.str("xs"), v.vec(xs), v.str("ys"), v.vec(ys)});
    auto f = write_frame(v, map);

    // Header is padded so the buffer section starts 8-byte aligned.
    BOOST_TEST((8 + f.json_len) % 8 == 0u);
    const auto& bufs = f.header["buffers"].as_array();
    BOOST_REQUIRE_EQUAL(bufs.size(), 2u);
    for (const auto& b : bufs) {
        BOOST_TEST(b["offset"].as_int() % 8 == 0);
        BOOST_TEST(b["dtype"].as_string() == "f64");
    }

    const auto xi = f.body()["xs"]["$buf"].as_int();
    const auto yi = f.body()["ys"]["$buf"].as_int();
    const auto xb = f.buffer(static_cast<std::size_t>(xi));
    const auto yb = f.buffer(static_cast<std::size_t>(yi));
    BOOST_REQUIRE_EQUAL(xb.size(), 40u);
    BOOST_REQUIRE_EQUAL(yb.size(), 17u);
    BOOST_TEST(xb[39] == 9.75);
    BOOST_TEST(yb[16] == 48.0);
}

BOOST_AUTO_TEST_CASE(mixed_vectors_are_not_buffered) {
    Values v;
    std::vector<LispVal> xs;
    for (int i = 0; i < 20; ++i) xs.push_back(v.flo(i));
    xs[7] = v.str("seven");
    auto f = write_frame(v, v.vec(xs));
    BOOST_TEST(f.body().is_array());
    BOOST_TEST(f.body().as_array()[7].as_string() == "seven");
}

BOOST_AUTO_TEST_CASE(unsupported_values_are_rejected) {
    Values v;
    ui::detail::FrameWriter w(v.heap, v.intern);
    BOOST_TEST(!w.write(ops::box(Tag::Char, 'x')).has_value());
}

BOOST_AUTO_TEST_SUITE_END()

// ===========================================================================

BOOST_AUTO_TEST_SUITE(ui_helper_tests)

BOOST_AUTO_TEST_CASE(u32_round_trip_little_endian) {
    std::string s;
    ui::put_u32_le(s, 0x01020304u);
    BOOST_REQUIRE_EQUAL(s.size(), 4u);
    BOOST_TEST(static_cast<unsigned char>(s[0]) == 0x04u);
    BOOST_TEST(ui::get_u32_le(s.data()) == 0x01020304u);
}

BOOST_AUTO_TEST_CASE(make_frame_layout) {
    auto f = ui::make_frame("{\"a\":1}", std::string(16, '\x01'));
    const auto total = ui::get_u32_le(f->data());
    const auto json_len = ui::get_u32_le(f->data() + 4);
    BOOST_TEST(total + 4u == f->size());
    BOOST_TEST((8 + json_len) % 8 == 0u);
    BOOST_TEST(f->substr(8 + json_len).size() == 16u);
}

BOOST_AUTO_TEST_CASE(json_quote_escapes_control_characters) {
    BOOST_TEST(ui::json_quote("a\tb") == "\"a\\tb\"");
    BOOST_TEST(ui::json_quote(std::string("\x01", 1)) == "\"\\u0001\"");
}

BOOST_AUTO_TEST_CASE(app_ids_are_file_safe) {
    BOOST_TEST(ui::sanitize_app_id("eta.cookbook/frontier") == "eta.cookbook_frontier");
    BOOST_TEST(ui::sanitize_app_id("Portfolio explorer") == "Portfolio_explorer");
    BOOST_TEST(ui::sanitize_app_id("") == "app");
}

BOOST_AUTO_TEST_CASE(mime_types) {
    BOOST_TEST(ui::mime_type_for("index.html").starts_with("text/html"));
    BOOST_TEST(ui::mime_type_for("a/b.js").starts_with("text/javascript"));
    BOOST_TEST(ui::mime_type_for("f.woff2") == "font/woff2");
    BOOST_TEST(ui::mime_type_for("x.unknown") == "application/octet-stream");
}

BOOST_AUTO_TEST_CASE(primitives_are_registered_in_seed_order) {
    Values v;
    BuiltinEnvironment env;
    ui::register_ui_primitives(env, v.heap, v.intern);
    const std::vector<std::string> expected{
        "%ui-start", "%ui-send", "%ui-recv", "%ui-open", "%ui-stop", "%ui-client-count", "%ui-window"};
    BOOST_REQUIRE_EQUAL(env.specs().size(), expected.size());
    for (std::size_t i = 0; i < expected.size(); ++i) {
        BOOST_TEST(env.specs()[i].name == expected[i]);
    }
}

BOOST_AUTO_TEST_CASE(open_rejects_non_local_urls) {
    Values v;
    BuiltinEnvironment env;
    ui::register_ui_primitives(env, v.heap, v.intern);
    auto idx = env.lookup("%ui-open");
    BOOST_REQUIRE(idx.has_value());
    const std::vector<LispVal> args{v.str("https://example.com/")};
    auto r = env.specs()[*idx].func(args);
    BOOST_TEST(!r.has_value());
}

BOOST_AUTO_TEST_CASE(window_reports_unavailable_without_window_support) {
    Values v;
    BuiltinEnvironment env;
    ui::register_ui_primitives(env, v.heap, v.intern);
    auto idx = env.lookup("%ui-window");
    BOOST_REQUIRE(idx.has_value());
    // eta_core_test does not link the native window (ui_window.cpp), so the
    // hook is empty and std.ui falls back to the browser.
    const auto saved = ui::window_hooks();
    ui::window_hooks() = {};
    const std::vector<LispVal> ok_args{v.str("http://127.0.0.1:1234/?t=x"), v.str("App"), v.fix(800), v.fix(600)};
    auto r = env.specs()[*idx].func(ok_args);
    BOOST_REQUIRE(r.has_value());
    BOOST_TEST(*r == False);

    // With a hook installed the primitive delegates to it.
    static std::string seen_url;
    static int seen_w = 0;
    ui::window_hooks().open = [](const std::string& url, const std::string&, int w, int) {
        seen_url = url;
        seen_w = w;
        return true;
    };
    auto r2 = env.specs()[*idx].func(ok_args);
    BOOST_REQUIRE(r2.has_value());
    BOOST_TEST(*r2 == True);
    BOOST_TEST(seen_url == "http://127.0.0.1:1234/?t=x");
    BOOST_TEST(seen_w == 800);

    const std::vector<LispVal> bad_args{v.str("https://example.com"), v.str("App"), v.fix(800), v.fix(600)};
    BOOST_TEST(!env.specs()[*idx].func(bad_args).has_value());
    ui::window_hooks() = saved;
}

BOOST_AUTO_TEST_SUITE_END()

// ===========================================================================

BOOST_AUTO_TEST_SUITE(ui_server_tests)

BOOST_AUTO_TEST_CASE(serves_files_and_speaks_the_protocol) {
    TempUiHome home;
    auto& server = ui::UiServer::instance();
    server.stop();

    auto url = server.start("eta.tests.ui", home.web);
    BOOST_REQUIRE_MESSAGE(url.has_value(), (url ? std::string{} : url.error()));
    const auto parsed = parse_url(*url);
    BOOST_TEST(parsed.token.size() == 32u);
    BOOST_TEST(fs::exists(home.root / "config" / "eta" / "ui" / "eta.tests.ui.url"));

    // Static files, SPA fallback and 404s.
    int status = 0;
    auto index = http_get(parsed.port, "/?t=" + parsed.token, &status);
    BOOST_REQUIRE(index.has_value());
    BOOST_TEST(status == 200);
    BOOST_TEST(index->find("eta-ui-test") != std::string::npos);
    auto js = http_get(parsed.port, "/assets/app.js", &status);
    BOOST_REQUIRE(js.has_value());
    BOOST_TEST(status == 200);
    http_get(parsed.port, "/assets/missing.js", &status);
    BOOST_TEST(status == 404);
    http_get(parsed.port, "/some/route", &status);
    BOOST_TEST(status == 200);  // falls back to index.html

    // Retained snapshot before any client connects.
    Values v;
    ui::detail::FrameWriter w(v.heap, v.intern);
    BOOST_REQUIRE(w.write(v.call("hash-map", {v.str("type"), v.str("render"), v.str("rev"), v.fix(1)})).has_value());
    BOOST_TEST(server.broadcast(w.finish(), true) == 0u);

    // Wrong token: closed without delivering anything.
    {
        WsClient bad(parsed.port, "?t=deadbeef");
        if (bad.connected()) BOOST_TEST(!bad.read_frame().has_value());
    }

    WsClient client(parsed.port, "?t=" + parsed.token);
    BOOST_REQUIRE(client.connected());
    BOOST_TEST(recv_until("\"type\":\"connected\"").has_value());
    BOOST_TEST(server.client_count() == 1u);

    // The retained frame is replayed on connect.
    auto first = client.read_frame();
    BOOST_REQUIRE(first.has_value());
    BOOST_TEST(decode(*first).body()["type"].as_string() == "render");

    // Broadcast with a binary buffer reaches the client intact.
    std::vector<LispVal> xs;
    for (int i = 0; i < 1000; ++i) xs.push_back(v.flo(i * 0.001));
    ui::detail::FrameWriter w2(v.heap, v.intern);
    BOOST_REQUIRE(w2.write(v.call("hash-map", {v.str("xs"), v.vec(xs)})).has_value());
    BOOST_TEST(server.broadcast(w2.finish(), false) == 1u);
    auto second = client.read_frame();
    BOOST_REQUIRE(second.has_value());
    auto f2 = decode(*second);
    auto buf = f2.buffer(static_cast<std::size_t>(f2.body()["xs"]["$buf"].as_int()));
    BOOST_REQUIRE_EQUAL(buf.size(), 1000u);
    BOOST_TEST(buf[999] == 0.999);

    // Client -> server messages arrive in the inbox.
    BOOST_REQUIRE(client.send_json("{\"type\":\"event\",\"h\":\"r/0#on-click\",\"data\":{}}"));
    auto event = recv_until("\"type\":\"event\"");
    BOOST_REQUIRE(event.has_value());
    BOOST_TEST(event->find("r/0#on-click") != std::string::npos);

    server.stop();
    BOOST_TEST(!server.running());
}

BOOST_AUTO_TEST_CASE(rerun_reuses_port_and_token) {
    TempUiHome home;
    auto& server = ui::UiServer::instance();
    server.stop();

    auto first = server.start("eta.tests.rerun", home.web);
    BOOST_REQUIRE(first.has_value());
    server.stop();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    auto second = server.start("eta.tests.rerun", home.web);
    BOOST_REQUIRE(second.has_value());
    BOOST_TEST(*first == *second);
    server.stop();
}

BOOST_AUTO_TEST_CASE(takeover_request_reaches_previous_run) {
    TempUiHome home;
    auto& server = ui::UiServer::instance();
    server.stop();

    auto url = server.start("eta.tests.takeover", home.web);
    BOOST_REQUIRE(url.has_value());
    const auto parsed = parse_url(*url);
    BOOST_TEST(ui::request_previous_run_quit(parsed.port, parsed.token));
    BOOST_TEST(recv_until("\"type\":\"quit\"").has_value());
    server.stop();
}

BOOST_AUTO_TEST_CASE(recv_times_out_with_nothing_queued) {
    auto& server = ui::UiServer::instance();
    while (server.recv(0)) {}
    const auto t0 = std::chrono::steady_clock::now();
    BOOST_TEST(!server.recv(50).has_value());
    const auto waited = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t0).count();
    BOOST_TEST(waited >= 40);
}

BOOST_AUTO_TEST_SUITE_END()
