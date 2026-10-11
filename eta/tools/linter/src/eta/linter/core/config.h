#pragma once

#include <cstddef>
#include <expected>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace eta::linter {
    struct RuleConfigOverride {
        bool disabled{false};
        std::optional<std::string> severity;
        std::map<std::string, std::size_t> integer_options;
    };

    struct LintConfig {
        std::optional<std::string> preset;
        std::optional<std::size_t> max_warnings;
        std::vector<std::string> includes;
        std::vector<std::string> excludes;
        std::map<std::string, RuleConfigOverride> rules;
    };

    std::expected<LintConfig, std::string> load_lint_config(const std::filesystem::path &path);
    std::optional<std::filesystem::path> find_lint_config(const std::filesystem::path &start);
}

