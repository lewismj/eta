#include "eta/jupyter/comm/actors_comm.h"

#include <string>

#include <nlohmann/json.hpp>

#include "eta/session/driver.h"

namespace eta::jupyter::comm {

namespace {

[[nodiscard]] std::string escape_html(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (const char ch : text) {
        switch (ch) {
            case '&': out += "&amp;"; break;
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            case '"': out += "&quot;"; break;
            case '\'': out += "&#39;"; break;
            default: out.push_back(ch); break;
        }
    }
    return out;
}

} // namespace

std::string_view actors_target()
{
    return "eta.actors";
}

nlohmann::json build_actor_snapshot(eta::session::Driver& driver)
{
    nlohmann::json out = nlohmann::json::object();
    out["version"] = 1;
    out["threads"] = nlohmann::json::array();
    out["children"] = nlohmann::json::array();

    const auto* pm = driver.process_manager();
    if (!pm) return out;

    for (const auto& thread : pm->list_threads()) {
        out["threads"].push_back(nlohmann::json::object({
            {"index", thread.index},
            {"endpoint", thread.endpoint},
            {"modulePath", thread.module_path},
            {"function", thread.func_name},
            {"alive", thread.alive},
        }));
    }

    for (const auto& child : pm->list_children()) {
        out["children"].push_back(nlohmann::json::object({
            {"pid", child.pid},
            {"endpoint", child.endpoint},
            {"modulePath", child.module_path},
            {"alive", child.alive},
        }));
    }

    return out;
}

nlohmann::json build_actor_event(std::string_view kind,
                                 int index,
                                 std::string_view name)
{
    return nlohmann::json::object({
        {"version", 1},
        {"kind", std::string(kind)},
        {"index", index},
        {"name", std::string(name)},
    });
}

std::string actors_snapshot_html(const nlohmann::json& payload)
{
    std::string html;
    html += "<div class=\"eta-actors\">";
    html += "<div><strong>Actors</strong></div>";

    html += "<div><em>Threads</em></div><ul>";
    if (payload.contains("threads") && payload["threads"].is_array()) {
        for (const auto& t : payload["threads"]) {
            const auto index = t.value("index", -1);
            const auto name = t.value("function", "");
            const auto alive = t.value("alive", false) ? "alive" : "exited";
            html += "<li>#";
            html += escape_html(std::to_string(index));
            html += " ";
            html += escape_html(name);
            html += " (";
            html += escape_html(alive);
            html += ")</li>";
        }
    }
    html += "</ul>";

    html += "<div><em>Children</em></div><ul>";
    if (payload.contains("children") && payload["children"].is_array()) {
        for (const auto& c : payload["children"]) {
            const auto pid = c.value("pid", -1);
            const auto module = c.value("modulePath", "");
            const auto alive = c.value("alive", false) ? "alive" : "exited";
            html += "<li>pid ";
            html += escape_html(std::to_string(pid));
            html += " ";
            html += escape_html(module);
            html += " (";
            html += escape_html(alive);
            html += ")</li>";
        }
    }
    html += "</ul>";

    html += "</div>";
    return html;
}

} // namespace eta::jupyter::comm
