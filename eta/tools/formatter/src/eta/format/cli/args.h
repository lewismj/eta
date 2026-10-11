#pragma once

#include <expected>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "eta/format/format_source.h"

namespace eta::format::cli {
    enum class Mode { InPlace, Check, Diff, Stdout };

    struct Arguments {
        Mode mode{Mode::InPlace};
        format::Options options;
        std::vector<std::filesystem::path> paths;
        std::vector<std::string> excludes;
        std::string stdin_filename{"<stdin>"};
        bool stdin_input{false};
        bool help{false};
        bool version{false};
        std::size_t jobs{1};
    };

    constexpr std::string_view help_text =
        "Usage: eta_fmt [--check | --diff | --stdout] [paths...]\n"
        "       eta_fmt --stdin [--stdin-filename name] [--check | --diff]\n"
        "Formats .eta files recursively; with no paths, uses the current directory.\n"
        "  --check             Report differences without writing (exit 1)\n"
        "  --diff              Print unified diffs without writing (exit 1)\n"
        "  --stdout FILE       Format exactly one explicit file to stdout\n"
        "  --stdin             Read stdin; filename is a logical identity only\n"
        "  --stdin-filename N  Name used in stdin diagnostics/diffs\n"
        "  --max-width N       Positive target width (default 100)\n"
        "  --indent N          Positive indentation (default 2)\n"
        "  --exclude GLOB      Repeatable root-relative glob (* and **)\n"
        "  --jobs N            Positive worker limit (default 1; at most 64)\n"
        "  --no-color          Plain diagnostics\n"
        "  --help, --version   Show help/version\n"
        "Exit codes: 0 success, 1 differences, 2 operation failure, 64 usage, 70 internal.\n";

    std::expected<Arguments, std::string> arguments(int argc, char **argv);
}

