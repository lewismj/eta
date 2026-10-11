#include "eta/linter/rules/rules.h"

#include <string>
#include <unordered_set>

namespace eta::linter::rules {
    void run_eta203(RuleContext &context) {
        std::unordered_set<std::string> seen;
        for (const auto &form : context.forms) {
            if (form.head != "export") continue;
            const auto &node = context.file.nodes[form.id];
            for (std::size_t i = 1; i < node.children.size(); ++i) {
                const auto &spec = context.file.nodes[node.children[i]];
                const auto name = symbol_text(context.file, spec);
                if (name.empty()) continue;  // renames/other specs are not L0 duplicates
                if (!seen.insert(std::string(name)).second) {
                    context.out.push_back({"ETA203", "eta_lint", Severity::Warning,
                        "duplicate export declaration", node_extent(context.file, spec),
                        true, 1, 1, std::string(context.path)});
                }
            }
        }
    }
}

