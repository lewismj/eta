#include "eta/linter/rules/rules.h"

#include <algorithm>

namespace eta::linter::rules {
    namespace {
        struct ImportForm {
            cst::ByteSpan span{};
            std::string_view module;
        };

        bool intersects(cst::ByteSpan a, cst::ByteSpan b) {
            return a.begin < b.end && b.begin < a.end;
        }

        bool has_comment_overlap(const cst::File &file, cst::ByteSpan span) {
            for (const auto &association : file.associations) {
                const auto &comment = file.trivia[association.comment];
                if (comment.kind == cst::TriviaKind::LineComment && intersects(comment.span, span)) return true;
                if (comment.kind == cst::TriviaKind::BlockComment && intersects(comment.span, span)) return true;
            }
            return false;
        }

        bool is_safe_import_form(const cst::File &file, const cst::Node &node, std::string_view &module) {
            if (node.kind != cst::NodeKind::List || node.children.size() != 2 || node.discarded || node.verbatim) return false;
            const auto head = symbol_text(file, file.nodes[node.children[0]]);
            if (head != "import") return false;
            module = symbol_text(file, file.nodes[node.children[1]]);
            return !module.empty();
        }

        void maybe_emit(const cst::File &file, std::string_view path, std::vector<Diagnostic> &out,
            const std::vector<ImportForm> &group) {
            if (group.size() < 2) return;
            const auto group_span = cst::ByteSpan{group.front().span.begin, group.back().span.end};
            if (has_comment_overlap(file, group_span)) return;

            std::vector<std::size_t> order(group.size());
            for (std::size_t i = 0; i < order.size(); ++i) order[i] = i;
            std::stable_sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) {
                return group[a].module < group[b].module;
            });
            bool changed = false;
            for (std::size_t i = 0; i < order.size(); ++i) {
                if (order[i] != i) {
                    changed = true;
                    break;
                }
            }
            if (!changed) return;

            out.push_back({"ETA205", "eta_lint", Severity::Info,
                "import declarations are not in canonical order for a proven-safe group", group_span,
                true, 1, 1, std::string(path)});
        }
    }

    void run_eta205(RuleContext &context) {
        std::vector<ImportForm> group;
        for (const auto root : context.file.roots) {
            const auto &node = context.file.nodes[root];
            std::string_view module;
            if (is_safe_import_form(context.file, node, module)) {
                group.push_back({node_extent(context.file, node), module});
            } else {
                maybe_emit(context.file, context.path, context.out, group);
                group.clear();
            }
        }
        maybe_emit(context.file, context.path, context.out, group);
    }
}

