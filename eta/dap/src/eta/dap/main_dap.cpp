#include "dap_server.h"
#include "dap_io.h"
#include "dap_trace.h"

#include <filesystem>
#include <iostream>
#include <optional>
#include <string>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

namespace {

void print_usage() {
    std::cerr
        << "Usage: eta_dap [--trace-protocol [path]] [--log-level=info|debug|trace]\n";
}

} // namespace

int main(int argc, char** argv) {
    std::optional<std::string> trace_path;
    bool trace_protocol = false;
    eta::dap::LogLevel log_level = eta::dap::LogLevel::Info;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];

        if (arg == "--trace-protocol") {
            trace_protocol = true;
            if (i + 1 < argc) {
                std::string maybe_path = argv[i + 1];
                if (maybe_path.rfind("--", 0) != 0) {
                    trace_path = maybe_path;
                    ++i;
                }
            }
            continue;
        }

        const std::string level_prefix = "--log-level=";
        if (arg.rfind(level_prefix, 0) == 0) {
            auto level = eta::dap::parse_log_level(
                std::string_view(arg).substr(level_prefix.size()));
            if (!level.has_value()) {
                std::cerr << "[eta_dap] error: invalid log level: " << arg << "\n";
                print_usage();
                return 2;
            }
            log_level = *level;
            continue;
        }

        if (arg == "--help" || arg == "-h") {
            print_usage();
            return 0;
        }

        std::cerr << "[eta_dap] error: unknown option: " << arg << "\n";
        print_usage();
        return 2;
    }

    /// Parsed for CLI compatibility; currently only protocol tracing is emitted.
    (void)log_level;

    std::optional<eta::dap::DapTrace> trace;
    if (trace_protocol) {
        if (trace_path.has_value()) {
            trace.emplace(std::filesystem::path(*trace_path));
        } else {
            trace.emplace();
        }

        if (trace->ready()) {
            eta::dap::set_trace_hook([&trace](std::string_view direction, std::string_view body) {
                if (trace.has_value()) {
                    trace->record(direction, body);
                }
            });
        }
    }

    /// Set stdin/stdout to binary mode on Windows to prevent \r\n mangling
#ifdef _WIN32
    _setmode(_fileno(stdin),  _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
#endif

    /// Disable stdio sync for throughput (same pattern as the LSP server)
    std::ios_base::sync_with_stdio(false);
    std::cin.tie(nullptr);

    eta::dap::DapServer server;
    server.run();
    eta::dap::set_trace_hook({});

    return 0;
}
