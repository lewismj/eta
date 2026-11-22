/**
 * @file ui_app_tests.cpp
 * @brief End-to-end tests of std.ui apps: launch the real cookbook programs
 * with etai (ETA_UI=none), connect like a browser over the WebSocket, and
 * drive them through their event handlers.
 *
 * These cover the whole loop: Eta view -> wire format -> native frames ->
 * page events -> Eta handlers -> state -> re-render.
 */

#include <boost/test/unit_test.hpp>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <eta/util/json.h>

#include "ui_test_support.h"

#ifndef ETA_ETAI_PATH
#define ETA_ETAI_PATH ""
#endif
#ifndef ETA_STDLIB_DIR
#define ETA_STDLIB_DIR ""
#endif
#ifndef ETA_COOKBOOK_DIR
#define ETA_COOKBOOK_DIR ""
#endif

using namespace eta_ui_test;
namespace json = eta::json;

namespace {

/// Depth-first search for nodes of type `t` (optionally matching a predicate).
void find_nodes(const json::Value& node, const std::string& t,
                std::vector<const json::Value*>& out,
                const std::function<bool(const json::Value&)>& pred = nullptr) {
    if (!node.is_object()) return;
    if (node["t"].is_string() && node["t"].as_string() == t && (!pred || pred(node))) out.push_back(&node);
    // Nested nodes in props (axes) are not searched; children only.
    if (node["c"].is_array()) {
        for (const auto& c : node["c"].as_array()) find_nodes(c, t, out, pred);
    }
}

const json::Value* find_one(const json::Value& tree, const std::string& t,
                            const std::function<bool(const json::Value&)>& pred = nullptr) {
    std::vector<const json::Value*> out;
    find_nodes(tree, t, out, pred);
    return out.empty() ? nullptr : out.front();
}

auto with_label(const std::string& label) {
    return [label](const json::Value& n) {
        return n["p"]["label"].is_string() && n["p"]["label"].as_string() == label;
    };
}

auto with_title_prefix(const std::string& prefix) {
    return [prefix](const json::Value& n) {
        return n["p"]["title"].is_string() && n["p"]["title"].as_string().rfind(prefix, 0) == 0;
    };
}

std::string handler_id(const json::Value& node, const std::string& prop) {
    const auto& h = node["p"][prop];
    BOOST_REQUIRE_MESSAGE(h.is_object() && h["$h"].is_string(), "node has no handler " + prop);
    return h["$h"].as_string();
}

double number(const json::Value& v) {
    if (v.is_int()) return static_cast<double>(v.as_int());
    BOOST_REQUIRE(v.is_double());
    return v.as_double();
}

/// A running app process plus a connected page.
class AppSession {
public:
    /// `use_flag`: pass `--ui none` on the command line instead of ETA_UI.
    AppSession(const std::string& app_relpath, const std::string& app_id, bool use_flag = false) {
        etai_ = ETA_ETAI_PATH;
        stdlib_ = ETA_STDLIB_DIR;
        const fs::path app = fs::path(ETA_COOKBOOK_DIR) / app_relpath;
        if (etai_.empty() || !fs::exists(etai_) || !fs::exists(app)) {
            skipped_ = true;
            return;
        }
        if (!use_flag) no_browser_.emplace("ETA_UI", "none");
        const std::string ui_flag = use_flag ? " --ui none" : "";
        url_file_ = home_.root / "config" / "eta" / "ui" / (app_id + ".url");

        std::string cmd = "\"\"" + etai_ + "\"" + ui_flag + " --path \"" + stdlib_ + "\" \"" + app.string() + "\"\"";
#ifndef _WIN32
        cmd = "\"" + etai_ + "\"" + ui_flag + " --path \"" + stdlib_ + "\" \"" + app.string() + "\"";
#endif
        runner_ = std::thread([this, cmd] { exit_code_ = std::system(cmd.c_str()); done_ = true; });

        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
        while (std::chrono::steady_clock::now() < deadline && !done_) {
            std::error_code ec;
            if (fs::exists(url_file_, ec)) {
                std::ifstream in(url_file_);
                std::getline(in, url_);
                if (!url_.empty()) break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        BOOST_REQUIRE_MESSAGE(!url_.empty(), "app did not publish its URL (exited: " + std::to_string(done_.load()) + ")");
        const auto parsed = parse_url(url_);
        client_.emplace(parsed.port, "?t=" + parsed.token);
        BOOST_REQUIRE(client_->connected());
    }

    ~AppSession() {
        if (skipped_) return;
        if (client_ && client_->connected()) client_->send_json("{\"type\":\"quit\"}");
        client_.reset();
        if (runner_.joinable()) runner_.join();
    }

    [[nodiscard]] bool skipped() const { return skipped_; }
    [[nodiscard]] int exit_code() const { return exit_code_; }

    /// Next "render" message (fails on handler errors).
    json::Value next_render() {
        for (int i = 0; i < 20; ++i) {
            auto frame = client_->read_frame();
            BOOST_REQUIRE_MESSAGE(frame.has_value(), "connection closed while waiting for a render");
            auto f = decode(*frame);
            const auto& body = f.body();
            const std::string type = body["type"].is_string() ? body["type"].as_string() : "";
            BOOST_REQUIRE_MESSAGE(type != "error",
                                  "handler error: " + (body["message"].is_string() ? body["message"].as_string() : ""));
            if (type == "render") {
                last_frame_ = std::move(f);
                return last_frame_.body();
            }
        }
        BOOST_FAIL("no render message received");
        return {};
    }

    /// Length of a buffer-backed column in the last render.
    std::size_t buffer_length(const json::Value& ref) {
        BOOST_REQUIRE(ref.is_object() && ref["$buf"].is_int());
        return last_frame_.buffer(static_cast<std::size_t>(ref["$buf"].as_int())).size();
    }

    std::vector<double> buffer(const json::Value& ref) {
        BOOST_REQUIRE(ref.is_object() && ref["$buf"].is_int());
        return last_frame_.buffer(static_cast<std::size_t>(ref["$buf"].as_int()));
    }

    void event(const std::string& handler, const std::string& data_json = "{}") {
        BOOST_REQUIRE(client_->send_json("{\"type\":\"event\",\"h\":" + ui::json_quote(handler)
                                         + ",\"data\":" + data_json + "}"));
    }

    void quit() {
        client_->send_json("{\"type\":\"quit\"}");
        client_.reset();
        if (runner_.joinable()) runner_.join();
    }

private:
    TempUiHome home_;
    std::optional<ScopedEnv> no_browser_;
    std::string etai_;
    std::string stdlib_;
    fs::path url_file_;
    std::string url_;
    std::optional<WsClient> client_;
    std::thread runner_;
    std::atomic<bool> done_{false};
    int exit_code_{-1};
    bool skipped_{false};
    DecodedFrame last_frame_;
};

} // namespace

BOOST_AUTO_TEST_SUITE(ui_app_tests)

BOOST_AUTO_TEST_CASE(frontier_app_renders_and_handles_events) {
    AppSession app("ui/frontier.eta", "eta.cookbook.frontier", /*use_flag=*/true);
    if (app.skipped()) {
        BOOST_TEST_MESSAGE("etai or cookbook not available - skipping");
        return;
    }
    auto first = app.next_render();
    const auto& tree = first["tree"];
    BOOST_TEST(tree["p"]["title"].as_string() == "Portfolio explorer");

    const auto* points = find_one(tree, "points");
    BOOST_REQUIRE(points);
    BOOST_TEST(app.buffer_length((*points)["p"]["xs"]) == 600u);
    const auto xs_before = app.buffer((*points)["p"]["xs"]);

    const auto* stress = find_one(tree, "button", with_label("Apply stress scenario"));
    BOOST_REQUIRE(stress);
    app.event(handler_id(*stress, "on-click"));
    auto second = app.next_render();
    const auto* points2 = find_one(second["tree"], "points");
    BOOST_REQUIRE(points2);
    const auto xs_after = app.buffer((*points2)["p"]["xs"]);
    BOOST_REQUIRE_EQUAL(xs_after.size(), xs_before.size());
    std::size_t moved = 0;
    for (std::size_t i = 0; i < xs_after.size(); ++i) moved += xs_after[i] != xs_before[i];
    BOOST_TEST(moved == xs_after.size());
    BOOST_TEST(number(second["rev"]) > number(first["rev"]));

    app.event(handler_id(*points2, "on-select"), "{\"index\":42,\"key\":\"42\"}");
    auto third = app.next_render();
    BOOST_TEST(number(find_one(third["tree"], "points")->operator[]("p")["selected"]) == 42.0);

    app.quit();
    BOOST_TEST(app.exit_code() == 0);
}

BOOST_AUTO_TEST_CASE(wwr_app_renders_every_view) {
    AppSession app("ui/wwr/wwr.eta", "eta.cookbook.wwr");
    if (app.skipped()) {
        BOOST_TEST_MESSAGE("etai or cookbook not available - skipping");
        return;
    }
    auto msg = app.next_render();
    const auto& tree = msg["tree"];
    BOOST_TEST(tree["p"]["title"].as_string() == "Wrong-way risk explorer");

    // Exposure profile: band + fill + three lines over 60 monthly steps.
    const auto* profile = find_one(tree, "plot", with_title_prefix("Exposure profile - CP-17"));
    BOOST_REQUIRE(profile);
    std::vector<const json::Value*> lines;
    find_nodes(*profile, "line", lines);
    BOOST_TEST(lines.size() == 3u);
    for (const auto* l : lines) BOOST_TEST(app.buffer_length((*l)["p"]["ys"]) == 60u);
    std::vector<const json::Value*> areas;
    find_nodes(*profile, "area", areas);
    BOOST_TEST(areas.size() == 2u);

    // Bars: top 12, selected CP-17.
    const auto* bars = find_one(tree, "bars");
    BOOST_REQUIRE(bars);
    BOOST_TEST((*bars)["p"]["categories"].as_array().size() == 12u);
    BOOST_TEST((*bars)["p"]["selected"].as_string() == "CP-17");

    // Heatmap: 12 x 3 elasticities, CP-17 selected.
    const auto* heat = find_one(tree, "heatmap");
    BOOST_REQUIRE(heat);
    BOOST_TEST((*heat)["p"]["cols"].as_array().size() == 3u);
    BOOST_TEST(app.buffer_length((*heat)["p"]["values"]) == 36u);

    // Table: 30 counterparties.
    const auto* table = find_one(tree, "table");
    BOOST_REQUIRE(table);
    BOOST_TEST(app.buffer_length((*table)["p"]["data"]["wwr"]) == 30u);

    // The headline stat.
    const auto* stat = find_one(tree, "stat", with_label("Book CVA with wrong-way risk"));
    BOOST_REQUIRE(stat);
    const double cva_wrong = number((*stat)["p"]["value"]);
    BOOST_TEST(cva_wrong > 0.0);
}

BOOST_AUTO_TEST_CASE(wwr_app_reprices_on_assumption_changes) {
    AppSession app("ui/wwr/wwr.eta", "eta.cookbook.wwr");
    if (app.skipped()) return;
    auto first = app.next_render();
    const auto* stat = find_one(first["tree"], "stat", with_label("Book CVA with wrong-way risk"));
    BOOST_REQUIRE(stat);
    const double cva0 = number((*stat)["p"]["value"]);

    // Right-way correlation lowers book CVA.
    const auto* rho = find_one(first["tree"], "slider", with_label("Wrong-way correlation (rho)"));
    BOOST_REQUIRE(rho);
    app.event(handler_id(*rho, "on-change"), "{\"value\":-0.4}");
    auto second = app.next_render();
    const double cva1 = number(find_one(second["tree"], "stat", with_label("Book CVA with wrong-way risk"))->operator[]("p")["value"]);
    BOOST_TEST(cva1 < cva0);
    BOOST_TEST(number(find_one(second["tree"], "slider", with_label("Wrong-way correlation (rho)"))->operator[]("p")["value"]) == -0.4);

    // Collateral: the CSA toggle reveals two more sliders and cuts CVA.
    const auto* csa = find_one(second["tree"], "toggle", with_label("Collateralised (CSA)"));
    BOOST_REQUIRE(csa);
    app.event(handler_id(*csa, "on-change"), "{\"value\":true}");
    auto third = app.next_render();
    BOOST_TEST(find_one(third["tree"], "slider", with_label("Margin period of risk (days)")) != nullptr);
    const double cva2 = number(find_one(third["tree"], "stat", with_label("Book CVA with wrong-way risk"))->operator[]("p")["value"]);
    BOOST_TEST(cva2 < 0.5 * cva1);
    const auto* threshold = find_one(third["tree"], "slider", with_label("Threshold"));
    BOOST_REQUIRE(threshold);
    app.event(handler_id(*threshold, "on-change"), "{\"value\":10000}");
    third = app.next_render();
    threshold = find_one(third["tree"], "slider", with_label("Threshold"));
    BOOST_REQUIRE(threshold);
    BOOST_TEST(number((*threshold)["p"]["value"]) == 10000.0);
    // A positive oil shock raises CVA.
    const auto* shock_driver = find_one(third["tree"], "select", with_label("Macro shock"));
    BOOST_REQUIRE(shock_driver);
    app.event(handler_id(*shock_driver, "on-change"), "{\"value\":\"oil\"}");
    auto fourth = app.next_render();
    const auto* size = find_one(fourth["tree"], "slider", with_label("Shock size"));
    BOOST_REQUIRE(size);
    app.event(handler_id(*size, "on-change"), "{\"value\":0.25}");
    auto fifth = app.next_render();
    const double cva3 = number(find_one(fifth["tree"], "stat", with_label("Book CVA with wrong-way risk"))->operator[]("p")["value"]);
    BOOST_TEST(cva3 > cva2);
}

BOOST_AUTO_TEST_CASE(wwr_app_links_selection_across_views) {
    AppSession app("ui/wwr/wwr.eta", "eta.cookbook.wwr");
    if (app.skipped()) return;
    auto first = app.next_render();
    const auto* bars = find_one(first["tree"], "bars");
    BOOST_REQUIRE(bars);
    const std::string target = (*bars)["p"]["categories"].as_array().at(3).as_string();

    // Select via the bars: every view follows.
    app.event(handler_id(*bars, "on-select"), "{\"index\":3,\"key\":" + ui::json_quote(target) + "}");
    auto second = app.next_render();
    BOOST_TEST(find_one(second["tree"], "bars")->operator[]("p")["selected"].as_string() == target);
    BOOST_TEST(find_one(second["tree"], "heatmap")->operator[]("p")["selected-row"].as_string() == target);
    BOOST_TEST(find_one(second["tree"], "table")->operator[]("p")["selected"].as_string() == target);
    BOOST_TEST(find_one(second["tree"], "plot", with_title_prefix("Exposure profile - " + target)) != nullptr);

    // Select via the heatmap (row 0, column 1).
    const auto* heat = find_one(second["tree"], "heatmap");
    const std::string row0 = (*heat)["p"]["rows"].as_array().at(0).as_string();
    app.event(handler_id(*heat, "on-select"), "{\"row\":0,\"col\":1}");
    auto third = app.next_render();
    BOOST_TEST(find_one(third["tree"], "table")->operator[]("p")["selected"].as_string() == row0);

    // Select via the table (row index 29 = CP-30).
    const auto* table = find_one(third["tree"], "table");
    app.event(handler_id(*table, "on-select"), "{\"index\":29,\"key\":\"CP-30\"}");
    auto fourth = app.next_render();
    BOOST_TEST(find_one(fourth["tree"], "bars")->operator[]("p")["selected"].as_string() == "CP-30");

    // Switch to the 3D surface view: 10 quantiles x 30 times.
    const auto* surface_button = find_one(fourth["tree"], "button", with_label("Exposure surface (3D)"));
    BOOST_REQUIRE(surface_button);
    app.event(handler_id(*surface_button, "on-click"));
    auto fifth = app.next_render();
    const auto* surface = find_one(fifth["tree"], "surface");
    BOOST_REQUIRE(surface);
    BOOST_TEST(app.buffer_length((*surface)["p"]["zs"]) == 300u);
    BOOST_TEST(find_one(fifth["tree"], "plot", with_title_prefix("Exposure profile")) == nullptr);
}

BOOST_AUTO_TEST_SUITE_END()
