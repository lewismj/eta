#pragma once

#include <expected>
#include <string>
#include <string_view>
#include <vector>

#include "eta/linter/lint.h"

namespace eta::linter {
    enum class RuleStatus { Enabled, Gated, Delegated, Deprecated };

    /// Runtime view of a rule. The authoritative specification lives in
    /// `eta/tools/linter/rules/<CODE>.toml`; this table mirrors it for the
    /// engine, CI compares the two so they cannot drift.
    struct RuleSpec {
        std::string code;
        std::string name;
        std::string owner;
        RuleStatus status{RuleStatus::Gated};
        Severity severity{Severity::Warning};
        std::string analysis_level;
        std::string rationale;
        std::vector<std::string> prerequisites;
        std::vector<std::string> verified_prerequisites;
        std::vector<std::string> false_positives;
        std::string fix;
        std::string fix_safety;
        std::vector<std::string> positive_fixtures;
        std::vector<std::string> negative_fixtures;
        std::vector<std::string> dependency_fixtures;
        std::vector<std::string> unknown_fixtures;
        std::vector<std::string> presets;
    };

    /// All known rules in stable code order, enabled and gated alike.
    const std::vector<RuleSpec> &rule_registry();

    const RuleSpec *find_rule(std::string_view code);

    std::string rule_list_text();
    std::expected<std::string, std::string> rule_explanation(std::string_view code);

    /// Resolve the active rule-code set from compiled defaults plus the CLI
    /// overrides. `--rules` replaces the set with explicitly requested enabled
    /// rules; `--no-rule` then removes from it. Neither can enable a gated or
    /// delegated detector. Returns a usage error (exit 64) on an unknown or
    /// non-enableable request.
    std::expected<std::vector<std::string>, std::string> resolve_rules(const LintOptions &options);
}

