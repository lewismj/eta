#include "dap_io.h"

#include <iostream>
#include <string>

namespace eta::dap {

std::optional<std::string> read_message() {
    std::size_t content_length = 0;
    std::string header_line;

    while (std::getline(std::cin, header_line)) {
        if (!header_line.empty() && header_line.back() == '\r')
            header_line.pop_back();
        if (header_line.empty()) break; // end of headers

        const std::string prefix = "Content-Length: ";
        if (header_line.substr(0, prefix.size()) == prefix)
            content_length = std::stoull(header_line.substr(prefix.size()));
        // Ignore other headers (Content-Type, etc.)
    }

    if (std::cin.eof() || std::cin.fail()) return std::nullopt;
    if (content_length == 0) return std::nullopt;

    std::string body(content_length, '\0');
    std::cin.read(body.data(), static_cast<std::streamsize>(content_length));
    if (std::cin.fail()) return std::nullopt;

    return body;
}

void write_message(const std::string& body) {
    std::cout << "Content-Length: " << body.size() << "\r\n\r\n" << body;
    std::cout.flush();
}

} // namespace eta::dap

