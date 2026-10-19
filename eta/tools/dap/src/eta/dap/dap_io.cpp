#include "dap_io.h"

#include "eta/tooling/framing.h"

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
    auto body = tooling::read_framed_message(in, "eta_dap");
    if (body) emit_trace("in", *body);
    return body;
}

void write_message(std::ostream& out, const std::string& body) {
    emit_trace("out", body);
    tooling::write_framed_message(out, body);
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

