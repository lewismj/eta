#pragma once

#include <string>
#include <string_view>

#include <nlohmann/json_fwd.hpp>

namespace eta::session {
class Driver;
} // namespace eta::session

namespace eta::jupyter::comm {

/**
 * @brief Jupyter comm target used for disassembly messages.
 */
std::string_view disasm_target();

/**
 * @brief Build a disassembly payload for widget rendering.
 *
 * @param driver Session driver that owns the function registry and VM.
 * @param scope  `"current"` for active frame, `"all"` for full registry.
 * @param function_name Optional function name override.
 */
[[nodiscard]] nlohmann::json build_disassembly(eta::session::Driver& driver,
                                               std::string_view scope,
                                               std::string_view function_name = {});

/**
 * @brief Render a static HTML fallback for disassembly payloads.
 */
[[nodiscard]] std::string disassembly_html(const nlohmann::json& payload);

} // namespace eta::jupyter::comm
