#include "eta/linter/cli/args.h"

#include <charconv>
#include <cstring>
#include <filesystem>
#include <string_view>

namespace eta::linter::cli {
    namespace fs = std::filesystem;

    std::expected<Arguments, std::string> arguments(int argc, char **argv) {
        Arguments result;

        for (int i = 1; i < argc; ++i) {
            const std::string_view arg(argv[i]);
            if (arg == "--help") {
                result.help = true;
                continue;
            }
            if (arg == "--version") {
                result.version = true;
                continue;
            }
            if (arg == "--list-rules") {
                result.list_rules = true;
                continue;
            }
            if (arg == "--explain") {
                if (i + 1 == argc) return std::unexpected("missing value for --explain");
                result.explain_rule = std::string(argv[++i]);
                continue;
            }
            if (arg == "--config") {
                if (i + 1 == argc) return std::unexpected("missing value for --config");
                result.config = fs::u8path(argv[++i]);
                continue;
            }
            if (arg == "--preset") {
                if (i + 1 == argc) return std::unexpected("missing value for --preset");
                result.options.preset = argv[++i];
                continue;
            }
            if (arg == "--stdin") {
                result.options.stdin_input = true;
                continue;
            }
            if (arg == "--stdin-filename") {
                if (i + 1 == argc) return std::unexpected("missing value for --stdin-filename");
                result.options.stdin_filename = argv[++i];
                continue;
            }
            if (arg == "--format") {
                if (i + 1 == argc) return std::unexpected("missing value for --format");
                result.options.format = argv[++i];
                if (result.options.format != "pretty" && result.options.format != "json" && result.options.format != "sarif" && result.options.format != "github")
                    return std::unexpected("unsupported --format value: " + result.options.format);
                continue;
            }
            if (arg == "--fix") {
                result.options.fix = true;
                continue;
            }
            if (arg == "--rules") {
                if (i + 1 == argc) return std::unexpected("missing value for --rules");
                std::string_view text(argv[++i]);
                std::size_t start = 0;
                while (start <= text.size()) {
                    const auto pos = text.find(',', start);
                    const auto token = text.substr(start, pos == std::string_view::npos ? text.size() - start : pos - start);
                    if (!token.empty()) result.options.rules.push_back(std::string(token));
                    if (pos == std::string_view::npos) break;
                    start = pos + 1;
                }
                continue;
            }
            if (arg == "--no-rule") {
                if (i + 1 == argc) return std::unexpected("missing value for --no-rule");
                result.options.no_rules.push_back(argv[++i]);
                continue;
            }
            if (arg == "--max-warnings") {
                if (i + 1 == argc) return std::unexpected("missing value for --max-warnings");
                std::size_t value = 0;
                const auto length = std::strlen(argv[++i]);
                const auto parsed = std::from_chars(argv[i], argv[i] + length, value);
                if (parsed.ec != std::errc{} || parsed.ptr != argv[i] + length) {
                    return std::unexpected("expected a non-negative integer for --max-warnings");
                }
                result.options.max_warnings = value;
                continue;
            }
            if (arg == "--exclude") {
                if (i + 1 == argc) return std::unexpected("missing value for --exclude");
                result.options.excludes.push_back(argv[++i]);
                continue;
            }
            if (arg == "--jobs") {
                if (i + 1 == argc) return std::unexpected("missing value for --jobs");
                std::size_t value = 0;
                const auto length = std::strlen(argv[++i]);
                const auto parsed = std::from_chars(argv[i], argv[i] + length, value);
                if (parsed.ec != std::errc{} || parsed.ptr != argv[i] + length || value == 0) {
                    return std::unexpected("expected a positive integer for --jobs");
                }
                result.options.jobs = value;
                continue;
            }
            if (arg == "--quiet") {
                result.options.quiet = true;
                continue;
            }
            if (!arg.starts_with('-')) {
                result.paths.emplace_back(fs::u8path(arg));
                continue;
            }
            return std::unexpected("unknown option: " + std::string(arg));
        }
        if (result.options.stdin_input && !result.paths.empty()) {
            return std::unexpected("--stdin is exclusive with paths");
        }
        if (result.options.stdin_input && result.options.fix) {
            return std::unexpected("--fix is not supported with --stdin");
        }
        return result;
    }
}

