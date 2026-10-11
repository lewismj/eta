#include "eta/linter/rules/rules.h"

namespace eta::linter::rules {
    using Kind = reader::lexer::Token::Kind;

    cst::ByteSpan node_extent(const cst::File &file, const cst::Node &node) {
        return {file.tokens[node.token_begin].span.begin, file.tokens[node.token_end - 1].span.end};
    }

    std::string_view symbol_text(const cst::File &file, const cst::Node &node) {
        if (node.kind != cst::NodeKind::Atom) return {};
        const auto &token = file.tokens[node.token_begin];
        if (token.kind != cst::TokenKind::Reader || token.reader_kind != Kind::Symbol) return {};
        return file.source->slice(token.span);
    }

    namespace {
        bool quoting_head(std::string_view head) {
            return head == "quote" || head == "quasiquote" || head == "unquote" || head == "unquote-splicing";
        }

        void walk(const cst::File &file, cst::NodeId id, bool active, std::vector<ActiveForm> &forms) {
            const auto &node = file.nodes[id];
            if (node.discarded || node.verbatim || node.kind == cst::NodeKind::Error) return;
            switch (node.kind) {
                case cst::NodeKind::List: {
                    bool child_active = active;
                    if (!node.children.empty()) {
                        const auto head = symbol_text(file, file.nodes[node.children.front()]);
                        if (active && !head.empty()) forms.push_back({id, head});
                        if (quoting_head(head)) child_active = false;
                    }
                    for (const auto child : node.children) walk(file, child, child_active, forms);
                    if (node.tail) walk(file, *node.tail, child_active, forms);
                    break;
                }
                case cst::NodeKind::ReaderForm:
                case cst::NodeKind::VectorLiteral:
                case cst::NodeKind::BytevectorLiteral:
                    for (const auto child : node.children) walk(file, child, false, forms);
                    break;
                case cst::NodeKind::DatumComment:
                case cst::NodeKind::Directive:
                case cst::NodeKind::Atom:
                case cst::NodeKind::Error:
                    break;
            }
        }
    }

    std::vector<ActiveForm> collect_active_forms(const cst::File &file) {
        std::vector<ActiveForm> forms;
        for (const auto root : file.roots) walk(file, root, true, forms);
        return forms;
    }
}

