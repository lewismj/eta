#include "eta/tooling/framing.h"

#include <algorithm>
#include <exception>
#include <iostream>
#include <istream>
#include <ostream>

namespace eta::tooling {
    namespace {
        /// Consume @p count bytes without buffering them; false if the stream ends first.
        bool discard_bytes(std::istream& in, std::size_t count) {
            constexpr std::size_t kChunk = 1u << 20;
            while (count > 0) {
                const auto n = static_cast<std::streamsize>(std::min(count, kChunk));
                in.ignore(n);
                if (in.gcount() != n) return false;
                count -= static_cast<std::size_t>(n);
            }
            return true;
        }
    }

    std::optional<std::string> read_framed_message(std::istream& in,
        std::string_view log_tag, std::size_t max_bytes) {
        constexpr std::string_view prefix = "Content-Length: ";

        while (true) {
            std::size_t content_length = 0;
            bool got_content_length = false;
            std::string header_line;

            while (std::getline(in, header_line)) {
                if (!header_line.empty() && header_line.back() == '\r')
                    header_line.pop_back();
                if (header_line.empty()) break;

                /// Other headers (Content-Type, etc.) are ignored.
                if (header_line.starts_with(prefix)) {
                    try {
                        content_length = std::stoull(header_line.substr(prefix.size()));
                        got_content_length = true;
                    } catch (const std::exception&) {
                        std::cerr << "[" << log_tag << "] warning: malformed Content-Length header: "
                                  << header_line << "\n";
                    }
                }
            }

            if (in.eof() || in.fail()) return std::nullopt;

            /// Without a usable length there is no body to consume; resume at the next header.
            if (!got_content_length || content_length == 0) {
                std::cerr << "[" << log_tag << "] warning: received message with missing or zero "
                             "Content-Length; skipping\n";
                continue;
            }

            /**
             * Refuse to buffer an over-limit body, but still consume it: otherwise
             * its bytes would be parsed as the next message's headers.
             */
            if (content_length > max_bytes) {
                std::cerr << "[" << log_tag << "] warning: Content-Length " << content_length
                          << " exceeds maximum (" << max_bytes << " bytes); skipping\n";
                if (!discard_bytes(in, content_length)) return std::nullopt;
                continue;
            }

            std::string body(content_length, '\0');
            in.read(body.data(), static_cast<std::streamsize>(content_length));
            if (in.fail()) return std::nullopt;
            return body;
        }
    }

    void write_framed_message(std::ostream& out, std::string_view body) {
        out << "Content-Length: " << body.size() << "\r\n\r\n" << body;
        out.flush();
    }
}
