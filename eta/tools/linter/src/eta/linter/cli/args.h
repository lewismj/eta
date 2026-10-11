#pragma once

#include <expected>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "eta/linter/lint.h"

namespace eta::linter::cli {
    constexpr std::string_view help_text =
        "Usage: eta_lint [--format pretty|json|sarif|github] [--preset minimal|recommended|all] [--rules ETA104,ETA203] [--no-rule ETA401] [paths...]\n"
        "       eta_lint --stdin --stdin-filename sample.eta\n"
        "Lints .eta files recursively; with no paths, uses the current directory.\n"
        "  --format FORMAT        pretty|json|sarif|github (default: pretty)\n"
        "  --fix                  Apply validated ETA205/ETA401 fixes and re-lint\n"
        "  --preset NAME          minimal|recommended|all (default: recommended)\n"
        "  --rules RULES          Comma-separated enabled rule codes (replaces the set)\n"
        "  --no-rule RULE         Remove a rule from the active set (repeatable)\n"
        "  --max-warnings N       Fail when warnings exceed N (default: unlimited)\n"
        "  --list-rules           Print the registered rule catalogue and exit\n"
        "  --explain CODE         Print the selected rule's metadata and exit\n"
        "  --config FILE          Load .eta-lint.toml-style defaults from FILE\n"
        "  --stdin                Read source from stdin (exclusive with paths)\n"
        "  --stdin-filename NAME  Logical identity used for stdin diagnostics\n"
        "  --exclude GLOB         Repeatable root-relative glob (* and **)\n"
        "  --jobs N               Positive worker limit (default 1; at most 64)\n"
        "  --quiet                Hide informational findings\n"
        "  --help, --version      Show help/version\n"
        "Exit codes: 0 clean, 1 findings/limit, 2 input failure, 64 usage, 70 internal.\n";

    struct Arguments {
        LintOptions options;
        bool list_rules{false};
        std::optional<std::string> explain_rule;
        std::optional<std::filesystem::path> config;
        bool help{false};
        bool version{false};
        std::vector<std::filesystem::path> paths;
    };

    std::expected<Arguments, std::string> arguments(int argc, char **argv);
}
