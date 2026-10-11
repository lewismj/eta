#include "eta/linter/rules/rules.h"

namespace eta::linter::rules {
    namespace {
        /// An escaped or generated spelling carries `|...|` bars or `\` escapes;
        /// such names are excluded from the convention check.
        bool escaped(std::string_view name) {
            return name.find('|') != std::string_view::npos || name.find('\\') != std::string_view::npos;
        }

        /// A kebab-case violation is an underscore or an ASCII upper-case letter;
        /// conventional Lisp sigils (`?`, `!`, `->`, `*`, ...) stay allowed.
        bool violates_kebab_case(std::string_view name) {
            for (const char ch : name)
                if (ch == '_' || (ch >= 'A' && ch <= 'Z')) return true;
            return false;
        }

        const cst::Node *name_node(const cst::File &file, const cst::Node &form) {
            if (form.children.size() < 2) return nullptr;
            const auto &candidate = file.nodes[form.children[1]];
            if (candidate.kind == cst::NodeKind::Atom) return &candidate;
            if (candidate.kind == cst::NodeKind::List && !candidate.children.empty())
                return &file.nodes[candidate.children.front()];
            return nullptr;
        }
    }

    void run_eta104(RuleContext &context) {
        for (const auto &form : context.forms) {
            if (form.head != "define" && form.head != "defun") continue;
            const auto *target = name_node(context.file, context.file.nodes[form.id]);
            if (!target) continue;
            const auto name = symbol_text(context.file, *target);
            if (name.empty() || name == "_" || escaped(name) || !violates_kebab_case(name)) continue;
            context.out.push_back({"ETA104", "eta_lint", Severity::Info,
                "definition name should use kebab-case", node_extent(context.file, *target),
                true, 1, 1, std::string(context.path)});
        }
    }
}

