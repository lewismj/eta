#include "dap_trace.h"

#include <chrono>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>

#include "eta/util/json.h"

namespace eta::dap {

namespace {

std::string timestamp_now_utc() {
    using namespace std::chrono;
    const auto now = system_clock::now();
    const auto ms = duration_cast<milliseconds>(now.time_since_epoch()) % 1000;
    const std::time_t tt = system_clock::to_time_t(now);

    std::tm tm_utc{};
#ifdef _WIN32
    gmtime_s(&tm_utc, &tt);
#else
    gmtime_r(&tt, &tm_utc);
#endif

    std::ostringstream oss;
    oss << std::put_time(&tm_utc, "%Y-%m-%dT%H:%M:%S")
        << '.'
        << std::setfill('0')
        << std::setw(3)
        << ms.count()
        << 'Z';
    return oss.str();
}

} // namespace

std::optional<LogLevel> parse_log_level(std::string_view raw) {
    if (raw == "info")  return LogLevel::Info;
    if (raw == "debug") return LogLevel::Debug;
    if (raw == "trace") return LogLevel::Trace;
    return std::nullopt;
}

DapTrace::DapTrace() : stream_(&std::cerr) {}

void DapTrace::record(std::string_view direction, std::string_view body) {
    if (!stream_) return;

    eta::json::Value parsed_msg;
    bool parsed_ok = false;
    try {
        parsed_msg = eta::json::parse(std::string(body));
        parsed_ok = true;
    } catch (const std::exception&) {
        parsed_ok = false;
    }

    eta::json::Value seq_val;
    if (parsed_ok && parsed_msg.is_object()) {
        auto seq = parsed_msg.get_int("seq");
        if (seq.has_value()) seq_val = eta::json::Value(*seq);
    }

    eta::json::Value msg_val = parsed_ok
        ? std::move(parsed_msg)
        : eta::json::Value(std::string(body));

    auto trace_row = eta::json::object({
        {"ts",  eta::json::Value(timestamp_now_utc())},
        {"dir", eta::json::Value(std::string(direction))},
        {"seq", std::move(seq_val)},
        {"msg", std::move(msg_val)},
    });

    const std::string line = eta::json::to_string(trace_row);

    std::lock_guard<std::mutex> lk(write_mutex_);
    (*stream_) << line << '\n';
    stream_->flush();
}

} ///< namespace eta::dap
