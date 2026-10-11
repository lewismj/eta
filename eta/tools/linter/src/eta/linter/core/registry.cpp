#include "eta/linter/core/registry.h"

#include <algorithm>
#include <sstream>

namespace eta::linter {
    namespace {
        std::string_view severity_name(Severity severity) {
            switch (severity) {
                case Severity::Error: return "error";
                case Severity::Warning: return "warning";
                case Severity::Info: return "info";
            }
            return "warning";
        }

        std::string_view status_name(RuleStatus status) {
            switch (status) {
                case RuleStatus::Enabled: return "enabled";
                case RuleStatus::Gated: return "gated";
                case RuleStatus::Delegated: return "delegated";
                case RuleStatus::Deprecated: return "deprecated";
            }
            return "gated";
        }

        const std::vector<RuleSpec> &registry() {
            static const std::vector<RuleSpec> rules = {
                {"ETA104", "NonKebabCase", "eta_lint", RuleStatus::Enabled, Severity::Info, "cst",
                 "Recognised definition names use kebab-case; escaped and generated names are excluded.",
                 {"recognised-definition-name"}, {"recognised-definition-name"},
                 {"escaped or generated spellings carrying bars or escapes are excluded",
                  "conventional sigils such as ? ! and -> remain valid"},
                 "none", "No automatic rename: renaming a definition changes its public identity.",
                 {"ETA104_snake_case"}, {"ETA104_kebab_case"}, {"shared_mismatch"}, {"shared_recovered"},
                 {"minimal", "recommended", "strict", "all"}},
                {"ETA203", "DuplicateExport", "eta_lint", RuleStatus::Enabled, Severity::Warning, "cst",
                 "Explicit export declarations are unique within a recognised module; a repeated name is redundant and usually a copy/paste error.",
                 {"recognised-export-declaration"}, {"recognised-export-declaration"},
                 {"rename and other non-symbol export specs are not compared as L0 duplicates"},
                 "none", "No automatic removal: deleting an export can change a module's public surface.",
                 {"ETA203_duplicate"}, {"ETA203_unique"}, {"shared_mismatch"}, {"shared_recovered"},
                 {"minimal", "recommended", "strict", "all"}},
                {"ETA205", "ImportOrder", "eta_lint", RuleStatus::Enabled, Severity::Info, "semantic",
                 "Report import groups that are proven reorderable but not in canonical lexical order.",
                 {"proven-reorderable-import-group"}, {"proven-reorderable-import-group"},
                 {"groups with comments, directives or non-symbol import items are treated as unknown and skipped"},
                 "conditional", "Reordering is offered only for contiguous top-level imports with symbol-only module names and no overlapping comments/directives.",
                 {"ETA205_unsorted_imports"}, {"ETA205_sorted_imports"}, {"shared_mismatch"}, {"shared_recovered"},
                 {"strict", "all"}},
                {"ETA301", "UnusedLetBinding", "eta_lint", RuleStatus::Gated, Severity::Warning, "scope",
                 "Report a binding with no resolved references once ordinary-let binding and reference semantics are proven.",
                 {"verified-let-scopes", "complete-reference-coverage"}, {},
                 {"unmodelled macro references: suppress the affected scope"},
                 "none", "No removal fix: the initializer can have effects.", {}, {}, {}, {}, {}},
                {"ETA401", "CommentMarkerStyle", "eta_lint", RuleStatus::Enabled, Severity::Info, "cst",
                 "Ordinary line comments use the canonical marker for their association: one semicolon end-of-line, two for a standalone statement comment.",
                 {"ordinary-line-comment-association"}, {"ordinary-line-comment-association"},
                 {"three-semicolon banners are permitted", "strings, block comments, datum comments and shebangs are never matched"},
                 "safe", "Marker-only fixes rewrite semicolon runs for ordinary non-directive line comments while preserving comment bodies and line endings.",
                 {"ETA401_double_eol"}, {"ETA401_canonical"}, {"shared_mismatch"}, {"shared_recovered"},
                 {"minimal", "recommended", "strict", "all"}},
                {"ETA501", "FunctionTooLong", "eta_lint", RuleStatus::Enabled, Severity::Warning, "cst",
                 "Recognised function definitions stay within the physical-line budget.",
                 {"recognised-function-extent"}, {"recognised-function-extent"},
                 {"only define, defun and lambda extents are measured"},
                 "none", "No automatic fix: splitting a function is a semantic refactor.",
                 {"ETA501_long_function"}, {"ETA501_short_function"}, {"shared_mismatch"}, {"shared_recovered"},
                 {"minimal", "recommended", "strict", "all"}},
            };
            return rules;
        }
    }

    const std::vector<RuleSpec> &rule_registry() { return registry(); }

    const RuleSpec *find_rule(std::string_view code) {
        for (const auto &rule : registry())
            if (rule.code == code) return &rule;
        return nullptr;
    }

    std::string rule_list_text() {
        std::ostringstream out;
        for (const auto &rule : registry()) {
            out << rule.code << ' ' << rule.name << " [" << status_name(rule.status) << '/' << severity_name(rule.severity)
                << '/' << rule.analysis_level << "] presets=";
            for (std::size_t i = 0; i < rule.presets.size(); ++i) {
                if (i) out << ',';
                out << rule.presets[i];
            }
            out << '\n';
        }
        return out.str();
    }

    std::expected<std::string, std::string> rule_explanation(std::string_view code) {
        const auto *rule = find_rule(code);
        if (!rule) return std::unexpected("unknown rule code: " + std::string(code));
        auto join = [](const std::vector<std::string> &items) {
            std::ostringstream out;
            for (std::size_t i = 0; i < items.size(); ++i) {
                if (i) out << ", ";
                out << items[i];
            }
            return out.str();
        };
        std::ostringstream out;
        out << rule->code << ' ' << rule->name << '\n'
            << "status: " << status_name(rule->status) << '\n'
            << "severity: " << severity_name(rule->severity) << '\n'
            << "analysis-level: " << rule->analysis_level << '\n'
            << "rationale: " << rule->rationale << '\n';
        if (!rule->prerequisites.empty()) out << "prerequisites: " << join(rule->prerequisites) << '\n';
        if (!rule->verified_prerequisites.empty()) out << "verified-prerequisites: " << join(rule->verified_prerequisites) << '\n';
        if (!rule->false_positives.empty()) out << "false-positives: " << join(rule->false_positives) << '\n';
        out << "fix: " << rule->fix << '\n' << "fix-safety: " << rule->fix_safety << '\n';
        if (!rule->positive_fixtures.empty()) out << "positive-fixtures: " << join(rule->positive_fixtures) << '\n';
        if (!rule->negative_fixtures.empty()) out << "negative-fixtures: " << join(rule->negative_fixtures) << '\n';
        if (!rule->dependency_fixtures.empty()) out << "dependency-fixtures: " << join(rule->dependency_fixtures) << '\n';
        if (!rule->unknown_fixtures.empty()) out << "unknown-fixtures: " << join(rule->unknown_fixtures) << '\n';
        if (!rule->presets.empty()) out << "presets: " << join(rule->presets) << '\n';
        return out.str();
    }

    std::expected<std::vector<std::string>, std::string> resolve_rules(const LintOptions &options) {
        std::vector<std::string> active;
        if (options.rules.empty()) {
            const auto preset = options.preset.empty() ? std::string_view{"recommended"} : std::string_view{options.preset};
            if (preset != "minimal" && preset != "recommended" && preset != "strict" && preset != "all")
                return std::unexpected("unknown preset: " + std::string(preset));
            for (const auto &rule : registry()) {
                if (rule.status != RuleStatus::Enabled) continue;
                if (preset == "all" || std::find(rule.presets.begin(), rule.presets.end(), preset) != rule.presets.end())
                    active.push_back(rule.code);
            }
        } else {
            for (const auto &code : options.rules) {
                const auto *rule = find_rule(code);
                if (!rule) return std::unexpected("unknown rule code: " + code);
                if (rule->status != RuleStatus::Enabled)
                    return std::unexpected("rule is not enableable: " + code);
                if (std::find(active.begin(), active.end(), code) == active.end()) active.push_back(code);
            }
        }
        for (const auto &code : options.no_rules) {
            if (!find_rule(code)) return std::unexpected("unknown rule code: " + code);
            active.erase(std::remove(active.begin(), active.end(), code), active.end());
        }
        std::sort(active.begin(), active.end());
        return active;
    }
}
