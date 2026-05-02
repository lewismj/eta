#pragma once

#include <functional>
#include <iosfwd>
#include <optional>
#include <string>
#include <string_view>

namespace eta::dap {

using TraceHook = std::function<void(std::string_view direction, std::string_view body)>;

/**
 * Read one DAP message from the given stream.
 * Returns the raw JSON body, or nullopt on EOF / error.
 */
std::optional<std::string> read_message(std::istream& in);

/// Write one DAP message to the given stream (Content-Length framed).
void write_message(std::ostream& out, const std::string& body);

/// Convenience wrappers that use std::cin / std::cout (production use).
std::optional<std::string> read_message();
void write_message(const std::string& body);

/**
 * Install or clear a protocol trace hook.
 * When installed, every framed inbound/outbound message body is reported to
 * the hook with direction set to "in" or "out".
 */
void set_trace_hook(TraceHook hook);

} ///< namespace eta::dap

