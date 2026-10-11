#include "eta/linter/rules/rules.h"

namespace eta::linter::rules {
    namespace {
        constexpr std::size_t default_max_lines = 60;

        std::size_t physical_lines(std::string_view extent) {
            std::size_t lines = 1;
            for (std::size_t i = 0; i < extent.size(); ++i)
                if (extent[i] == '\n') ++lines;
            return lines;
        }
    }

    void run_eta501(RuleContext &context) {
        const auto configured = context.options.max_line_overrides.find("ETA501");
        const auto limit = configured == context.options.max_line_overrides.end() ? default_max_lines : configured->second;
        for (const auto &form : context.forms) {
            if (form.head != "define" && form.head != "defun" && form.head != "lambda") continue;
            const auto span = node_extent(context.file, context.file.nodes[form.id]);
            const auto extent = context.file.source->slice(span);
            if (physical_lines(extent) <= limit) continue;
            context.out.push_back({"ETA501", "eta_lint", Severity::Warning,
                "function definition exceeds the configured line limit", span,
                true, 1, 1, std::string(context.path)});
        }
    }
}

