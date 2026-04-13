#include "dap_io.h"

#include <iostream>
#include <string>

namespace eta::dap {

std::optional<std::string> read_message(std::istream& in) {
    // Loop so that malformed (zero-length) messages are skipped rather than
    // terminating the connection.
    while (true) {
        std::size_t content_length = 0;
        bool got_content_length = false;
        std::string header_line;

        while (std::getline(in, header_line)) {
            if (!header_line.empty() && header_line.back() == '\r')
                header_line.pop_back();
            if (header_line.empty()) break; // blank line = end of headers

            const std::string prefix = "Content-Length: ";
            if (header_line.substr(0, prefix.size()) == prefix) {
                try {
                    content_length = std::stoull(header_line.substr(prefix.size()));
                    got_content_length = true;
                } catch (const std::exception&) {
                    std::cerr << "[eta_dap] warning: malformed Content-Length header: "
                              << header_line << "\n";
                }
            }
            // Ignore other headers (Content-Type, etc.)
        }

        if (in.eof() || in.fail()) return std::nullopt;

        if (!got_content_length || content_length == 0) {
            // Malformed message — log a warning and try the next one.
            std::cerr << "[eta_dap] warning: received message with missing or zero "
                         "Content-Length; skipping\n";
            continue;
        }

        std::string body(content_length, '\0');
        in.read(body.data(), static_cast<std::streamsize>(content_length));
        if (in.fail()) return std::nullopt;

        return body;
    }
}

void write_message(std::ostream& out, const std::string& body) {
    out << "Content-Length: " << body.size() << "\r\n\r\n" << body;
    out.flush();
}

// Convenience wrappers using the process stdin/stdout

std::optional<std::string> read_message() {
    return read_message(std::cin);
}

void write_message(const std::string& body) {
    write_message(std::cout, body);
}

} // namespace eta::dap

