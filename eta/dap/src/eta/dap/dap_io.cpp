#include "dap_io.h"

#include <functional>
#include <iostream>
#include <mutex>
#include <string>

namespace eta::dap {

namespace {

std::mutex hook_mutex;
TraceHook  trace_hook;

void emit_trace(std::string_view direction, std::string_view body) {
    TraceHook hook_copy;
    {
        std::lock_guard<std::mutex> lk(hook_mutex);
        hook_copy = trace_hook;
    }
    if (hook_copy) hook_copy(direction, body);
}

} // namespace

std::optional<std::string> read_message(std::istream& in) {
    static constexpr std::size_t MAX_MESSAGE_SIZE = 64u * 1024u * 1024u;

    while (true) {
        std::size_t content_length = 0;
        bool got_content_length = false;
        std::string header_line;

        while (std::getline(in, header_line)) {
            if (!header_line.empty() && header_line.back() == '\r')
                header_line.pop_back();
            if (header_line.empty()) break;

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
        }

        if (in.eof() || in.fail()) return std::nullopt;

        if (!got_content_length || content_length == 0) {
            std::cerr << "[eta_dap] warning: received message with missing or zero "
                         "Content-Length; skipping\n";
            continue;
        }

        /// Guard against unbounded allocation from a crafted Content-Length value.
        if (content_length > MAX_MESSAGE_SIZE) {
            std::cerr << "[eta_dap] warning: Content-Length " << content_length
                      << " exceeds maximum (" << MAX_MESSAGE_SIZE << " bytes); skipping\n";
            continue;
        }

        std::string body(content_length, '\0');
        in.read(body.data(), static_cast<std::streamsize>(content_length));
        if (in.fail()) return std::nullopt;

        emit_trace("in", body);
        return body;
    }
}

void write_message(std::ostream& out, const std::string& body) {
    emit_trace("out", body);
    out << "Content-Length: " << body.size() << "\r\n\r\n" << body;
    out.flush();
}

/// Convenience wrappers using the process stdin/stdout

std::optional<std::string> read_message() {
    return read_message(std::cin);
}

void write_message(const std::string& body) {
    write_message(std::cout, body);
}

void set_trace_hook(TraceHook hook) {
    std::lock_guard<std::mutex> lk(hook_mutex);
    trace_hook = std::move(hook);
}

} ///< namespace eta::dap

