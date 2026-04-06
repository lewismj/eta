#pragma once
// Content-Length framed I/O over stdin/stdout — same wire protocol as LSP.

#include <optional>
#include <string>

namespace eta::dap {

/// Read one DAP message from stdin.
/// Returns the raw JSON body, or nullopt on EOF / error.
std::optional<std::string> read_message();

/// Write one DAP message to stdout (Content-Length framed).
void write_message(const std::string& body);

} // namespace eta::dap

