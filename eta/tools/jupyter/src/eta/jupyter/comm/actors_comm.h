#pragma once

#include <string>
#include <string_view>

#include <nlohmann/json_fwd.hpp>

namespace eta::session {
class Driver;
} // namespace eta::session

namespace eta::jupyter::comm {

/**
 * @brief Jupyter comm target used for actor lifecycle messages.
 */
std::string_view actors_target();

/**
 * @brief Build a process/thread actor snapshot payload.
 */
[[nodiscard]] nlohmann::json build_actor_snapshot(eta::session::Driver& driver);

/**
 * @brief Build a lightweight lifecycle event payload.
 */
[[nodiscard]] nlohmann::json build_actor_event(std::string_view kind,
                                               int index,
                                               std::string_view name);

/**
 * @brief Render a static HTML fallback for actor snapshot payloads.
 */
[[nodiscard]] std::string actors_snapshot_html(const nlohmann::json& payload);

} // namespace eta::jupyter::comm
