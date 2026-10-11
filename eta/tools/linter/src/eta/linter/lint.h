#pragma once

#include <cstddef>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "eta/cst/source.h"

namespace eta::linter {
    enum class Severity { Error, Warning, Info };

    /// A single finding with an exact byte span resolved from the shared CST.
    struct Diagnostic {
        std::string code;
        std::string owner{"eta_lint"};
        Severity severity{Severity::Warning};
        std::string message;
        cst::ByteSpan span{};
        bool located{true};
        /// One-based display position derived from the source map (§2.4).
        std::size_t line{1};
        std::size_t column{1};
        std::string path;
    };

    /// A recorded analysis limitation. A skip is never a clean bill of health.
    struct CoverageNote {
        std::string scope;
        std::string reason;
        std::vector<std::string> rules;
    };

    struct LintOptions {
        bool quiet{false};
        std::string format{"pretty"};
        std::string preset{};
        std::vector<std::string> rules;
        std::vector<std::string> no_rules;
        std::vector<std::string> excludes;
        std::vector<std::string> includes;
        std::optional<std::size_t> max_warnings;
        std::string stdin_filename{"<stdin>"};
        bool stdin_input{false};
        bool list_rules{false};
        std::optional<std::string> explain_rule;
        std::optional<std::filesystem::path> config;
        bool fix{false};
        std::size_t jobs{1};
        std::map<std::string, Severity> severity_overrides;
        std::map<std::string, std::size_t> max_line_overrides;
    };

    /// Result of analysing a single source buffer.
    struct FileReport {
        std::string path;
        std::vector<Diagnostic> diagnostics;
        std::vector<CoverageNote> coverage;
        std::optional<std::string> fixed_source;
        int exit_class{0};
    };
}
