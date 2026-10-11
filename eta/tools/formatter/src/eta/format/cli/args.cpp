#include "args.h"

#include <charconv>
#include <filesystem>

namespace eta::format::cli {
    namespace fs = std::filesystem;

    std::expected<Arguments, std::string> arguments(int argc, char **argv) {
        Arguments result;
        bool selected_mode = false;
        bool filename = false;
        bool positional = false;
        for (int i = 1; i < argc; ++i) {
            const std::string_view arg(argv[i]);
            if (arg == "--" && !positional) {
                positional = true;
                continue;
            }
            if (positional || !arg.starts_with('-')) {
                result.paths.emplace_back(fs::u8path(arg));
                continue;
            }
            auto value = [&]() -> std::expected<std::string_view, std::string> {
                if (i + 1 == argc) return std::unexpected("missing value for " + std::string(arg));
                return std::string_view(argv[++i]);
            };
            auto number = [&]() -> std::expected<std::size_t, std::string> {
                const auto text = value();
                if (!text) return std::unexpected(text.error());
                std::size_t number = 0;
                const auto parsed = std::from_chars(text->data(), text->data() + text->size(), number);
                if (parsed.ec != std::errc{} || parsed.ptr != text->data() + text->size() || !number)
                    return std::unexpected("expected a positive integer for " + std::string(arg));
                return number;
            };
            if (arg == "--help") result.help = true;
            else if (arg == "--version") result.version = true;
            else if (arg == "--no-color") {}
            else if (arg == "--stdin") result.stdin_input = true;
            else if (arg == "--check" || arg == "--diff" || arg == "--stdout") {
                if (selected_mode) return std::unexpected("--check, --diff and --stdout are mutually exclusive");
                selected_mode = true;
                result.mode = arg == "--check" ? Mode::Check : arg == "--diff" ? Mode::Diff : Mode::Stdout;
            } else if (arg == "--max-width" || arg == "--indent" || arg == "--jobs") {
                const auto parsed = number();
                if (!parsed) return std::unexpected(parsed.error());
                if (arg == "--max-width") result.options.width = *parsed;
                else if (arg == "--indent") result.options.indent = *parsed;
                else result.jobs = *parsed;
            } else if (arg == "--stdin-filename" || arg == "--exclude") {
                const auto text = value();
                if (!text || text->empty()) return std::unexpected(text ? "empty option value" : text.error());
                if (arg == "--stdin-filename") {
                    result.stdin_filename = *text;
                    filename = true;
                } else {
                    result.excludes.emplace_back(*text);
                }
            } else {
                return std::unexpected("unknown option: " + std::string(arg));
            }
        }
        if (result.stdin_input && (!result.paths.empty() || result.mode == Mode::Stdout))
            return std::unexpected("--stdin is exclusive with paths and --stdout");
        if (filename && !result.stdin_input) return std::unexpected("--stdin-filename requires --stdin");
        std::error_code path_error;
        if (result.mode == Mode::Stdout && (result.paths.size() != 1 || fs::is_directory(result.paths[0], path_error)))
            return std::unexpected("--stdout requires exactly one explicit file");
        if (result.stdin_input && !selected_mode) result.mode = Mode::Stdout;
        return result;
    }
}

