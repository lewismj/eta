#include "eta/jupyter/comm/tensor_comm.h"

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

std::string_view tensor_target()
{
    return "eta.tensor";
}

nlohmann::json build_tensor_payload(nlohmann::json tensor)
{
    nlohmann::json payload = nlohmann::json::object();
    payload["version"] = 1;
    payload["tensor"] = std::move(tensor);
    return payload;
}

std::string tensor_payload_html(const nlohmann::json& payload)
{
    std::string html;
    html += "<div class=\"eta-tensor-explorer\">";
    html += "<div><strong>Tensor Explorer</strong></div>";

    const auto& tensor = payload.contains("tensor") ? payload["tensor"] : nlohmann::json::object();
    if (tensor.is_object() && tensor.contains("shape")) {
        html += "<div>shape: " + escape_html(tensor["shape"].dump()) + "</div>";
    }
    if (tensor.is_object() && tensor.contains("dtype")) {
        html += "<div>dtype: " + escape_html(tensor["dtype"].dump()) + "</div>";
    }
    html += "<pre>" + escape_html(tensor.dump(2)) + "</pre>";
    html += "</div>";
    return html;
}

} // namespace eta::jupyter::comm
