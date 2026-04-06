#pragma once
// Content-Length framed I/O over stdin/stdout — same wire protocol as LSP.

#include <iosfwd>
#include <optional>
#include <string>

namespace eta::dap {

/// Read one DAP message from the given stream.
/// Returns the raw JSON body, or nullopt on EOF / error.
std::optional<std::string> read_message(std::istream& in);

/// Write one DAP message to the given stream (Content-Length framed).
void write_message(std::ostream& out, const std::string& body);

/// Convenience wrappers that use std::cin / std::cout (production use).
std::optional<std::string> read_message();
void write_message(const std::string& body);

} // namespace eta::dap

