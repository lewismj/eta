#pragma once

#include <string>

#include "eta/cst/parse.h"
#include "eta/linter/lint.h"

namespace eta::linter {
    /// Analyse one source buffer with an already-resolved rule set. Parsing,
    /// recovery and limit failures are input failures (exit class 2); gated and
    /// skipped rules are reported through coverage, never silently.
    FileReport lint_source(std::string source, std::string identity,
        const std::vector<std::string> &rules, const LintOptions &options, cst::Operation operation = {});

    /// Deterministic JSON report for one or more file reports. Diagnostics are
    /// sorted by path, byte start, owner and code; positions are one-based.
    std::string json_report(const std::vector<FileReport> &reports);

    /// SARIF 2.1.0 report with the same deterministic diagnostic ordering.
    std::string sarif_report(const std::vector<FileReport> &reports);

    /// GitHub command annotations for the same deterministic diagnostic ordering.
    std::string github_report(const std::vector<FileReport> &reports);
}

