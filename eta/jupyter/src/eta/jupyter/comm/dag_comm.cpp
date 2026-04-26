#include "eta/jupyter/comm/dag_comm.h"

#include <string>

#include <nlohmann/json.hpp>

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

std::string_view dag_target()
{
    return "eta.dag";
}

nlohmann::json build_dag_payload(nlohmann::json graph)
{
    nlohmann::json payload = nlohmann::json::object();
    payload["version"] = 1;
    payload["graph"] = std::move(graph);
    return payload;
}

std::string dag_payload_html(const nlohmann::json& payload)
{
    std::string html;
    html += "<div class=\"eta-dag\">";
    html += "<div><strong>Causal DAG</strong></div>";

    const auto& graph = payload.contains("graph") ? payload["graph"] : nlohmann::json::object();
    std::size_t nodes = 0;
    std::size_t edges = 0;
    if (graph.is_object()) {
        if (graph.contains("nodes") && graph["nodes"].is_array()) {
            nodes = graph["nodes"].size();
        }
        if (graph.contains("edges") && graph["edges"].is_array()) {
            edges = graph["edges"].size();
        }
    }

    html += "<div>nodes=" + escape_html(std::to_string(nodes))
         + " edges=" + escape_html(std::to_string(edges)) + "</div>";
    html += "<pre>" + escape_html(graph.dump(2)) + "</pre>";
    html += "</div>";
    return html;
}

} // namespace eta::jupyter::comm
