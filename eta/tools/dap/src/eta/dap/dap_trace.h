#pragma once

#include <iostream>
#include <mutex>
#include <optional>
#include <string_view>

namespace eta::dap {

enum class LogLevel {
    Info,
    Debug,
    Trace,
};

/// Parse a CLI log level token ("info", "debug", "trace").
std::optional<LogLevel> parse_log_level(std::string_view raw);

/**
 * Line-oriented DAP protocol tracer.
 * Each observed message is emitted as a single JSON object line.
 */
class DapTrace {
public:
    /// Trace to stderr.
    DapTrace();

    [[nodiscard]] bool ready() const noexcept { return stream_ != nullptr; }

    /**
     * Record one protocol message.
     * @param direction "in" or "out"
     * @param body      Raw JSON-RPC message body
     */
    void record(std::string_view direction, std::string_view body);

private:
    std::ostream* stream_{nullptr};
    std::mutex    write_mutex_;
};

} ///< namespace eta::dap
