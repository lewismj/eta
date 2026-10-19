#pragma once

#include <cstddef>
#include <iosfwd>
#include <optional>
#include <string>
#include <string_view>

namespace eta::tooling {
    /// Largest message body accepted by read_framed_message (LSP and DAP).
    inline constexpr std::size_t kMaxFramedMessageBytes = 64u * 1024u * 1024u;

    /**
     * Read one Content-Length framed message (the LSP/DAP base protocol).
     *
     * Frames with a missing, malformed or zero Content-Length are skipped.
     * A frame whose length exceeds @p max_bytes is skipped *after* its body
     * has been consumed, so the next header is read from the right offset.
     * Warnings are written to stderr prefixed with @p log_tag.
     *
     * @return The raw body, or nullopt on EOF or a truncated body.
     */
    std::optional<std::string> read_framed_message(std::istream& in,
        std::string_view log_tag, std::size_t max_bytes = kMaxFramedMessageBytes);

    /// Write one Content-Length framed message and flush.
    void write_framed_message(std::ostream& out, std::string_view body);
}
