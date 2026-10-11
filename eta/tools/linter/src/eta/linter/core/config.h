#pragma once

#include <cstddef>
#include <expected>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace eta::linter {
    struct LintConfig {
        std::optional<std::string> preset;
        std::optional<std::size_t> max_warnings;
        std::vector<std::string> includes;
        std::vector<std::string> excludes;
        std::vector<std::string> disabled_rules;
    };

    std::expected<LintConfig, std::string> load_lint_config(const std::filesystem::path &path);
    std::optional<std::filesystem::path> find_lint_config(const std::filesystem::path &start);
}

