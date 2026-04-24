#pragma once

#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <optional>
#include <string>
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

    /// Trace to a file (append mode).
    explicit DapTrace(const std::filesystem::path& path);

    [[nodiscard]] bool ready() const noexcept { return stream_ != nullptr; }
    [[nodiscard]] bool using_stderr() const noexcept { return stream_ == &std::cerr; }
    [[nodiscard]] const std::string& file_path() const noexcept { return file_path_; }

    /**
     * Record one protocol message.
     * @param direction "in" or "out"
     * @param body      Raw JSON-RPC message body
     */
    void record(std::string_view direction, std::string_view body);

private:
    std::ofstream file_stream_;
    std::ostream* stream_{nullptr};
    std::string   file_path_;
    std::mutex    write_mutex_;
};

} ///< namespace eta::dap
