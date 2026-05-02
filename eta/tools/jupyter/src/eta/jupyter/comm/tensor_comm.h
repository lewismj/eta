#pragma once

#include <string>
#include <string_view>

#include <nlohmann/json_fwd.hpp>

namespace eta::jupyter::comm {

/**
 * @brief Jupyter comm target used for tensor explorer payloads.
 */
std::string_view tensor_target();

/**
 * @brief Wrap tensor explorer payload data for widget rendering.
 */
[[nodiscard]] nlohmann::json build_tensor_payload(nlohmann::json tensor);

/**
 * @brief Render a static HTML fallback for tensor-explorer payloads.
 */
[[nodiscard]] std::string tensor_payload_html(const nlohmann::json& payload);

} // namespace eta::jupyter::comm
