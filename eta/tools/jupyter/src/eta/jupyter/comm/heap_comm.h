#pragma once

#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

namespace eta::session {
class Driver;
} // namespace eta::session

namespace eta::jupyter::comm {

/**
 * @brief Jupyter comm target used for heap-inspection messages.
 */
std::string_view heap_target();

/**
 * @brief Build a heap snapshot payload for Jupyter comm and MIME rendering.
 *
 * The payload mirrors the DAP heap-inspection shape at a summary level so the
 * existing webview rendering logic can be reused by notebook widgets.
 */
[[nodiscard]] nlohmann::json build_heap_snapshot(eta::session::Driver& driver);

/**
 * @brief Render a static HTML fallback for heap snapshot payloads.
 */
[[nodiscard]] std::string heap_snapshot_html(const nlohmann::json& snapshot);

} // namespace eta::jupyter::comm
