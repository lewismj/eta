#pragma once

#include <cstddef>
#include <string>

#include <nlohmann/json.hpp>

#include "eta/jupyter/render_options.h"

/**
 * @file display.h
 * @brief MIME bundle rendering helpers for Jupyter execute results.
 */
namespace eta::session {
class Driver;
struct DisplayValue;
} // namespace eta::session

/**
 * @brief Jupyter-facing display helpers.
 */
namespace eta::jupyter::display {

/**
 * @brief Rendered MIME output and metadata for one execute_result message.
 */
struct RenderResult {
    nlohmann::json data = nlohmann::json::object();
    nlohmann::json metadata = nlohmann::json::object();
};

/**
 * @brief Build a MIME bundle for a display value.
 *
 * The returned payload is suitable for xeus `publish_execution_result`.
 *
 * @param driver Session driver used to inspect heap-backed values.
 * @param value  Display value returned by Driver::eval_to_display.
 * @param options Rendering options for preview and truncation limits.
 * @return MIME bundle and metadata for Jupyter output publication.
 */
RenderResult render_display_value(eta::session::Driver& driver,
                                  const eta::session::DisplayValue& value,
                                  const RenderOptions& options = {});

} // namespace eta::jupyter::display
