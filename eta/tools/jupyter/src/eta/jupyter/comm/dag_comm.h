#pragma once

#include <string>
#include <string_view>

#include <nlohmann/json_fwd.hpp>

namespace eta::jupyter::comm {

/**
 * @brief Jupyter comm target used for causal DAG visualisation payloads.
 */
std::string_view dag_target();

/**
 * @brief Wrap DAG data for widget and MIME rendering.
 */
[[nodiscard]] nlohmann::json build_dag_payload(nlohmann::json graph);

/**
 * @brief Render a static HTML fallback for DAG payloads.
 */
[[nodiscard]] std::string dag_payload_html(const nlohmann::json& payload);

} // namespace eta::jupyter::comm
