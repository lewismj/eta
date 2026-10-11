#include "forms.h"

#include <algorithm>
#include <array>
#include <set>

namespace eta::format {
    namespace {
        using namespace cst;
        using Kind = reader::lexer::Token::Kind;

        std::string_view raw(const File &file, TokenId token) { return file.source->slice(file.tokens[token].span); }

        bool formals_impl(const File &file, NodeId id, bool signature) {
            const auto &node = file.nodes[id];
            if (node.kind == NodeKind::Atom) return !signature && is_identifier(file, id);
            if (node.kind != NodeKind::List || (signature && node.children.empty())) return false;
            std::set<std::string_view> names;
            for (auto child : node.children) {
                if (!is_identifier(file, child) || !names.insert(raw(file, file.nodes[child].token_begin)).second) return false;
            }
            return !node.tail || (is_identifier(file, *node.tail) && names.insert(raw(file, file.nodes[*node.tail].token_begin)).second);
        }
    }

    bool is_identifier(const File &file, NodeId id) {
        const auto &node = file.nodes[id];
        return node.kind == NodeKind::Atom && file.tokens[node.token_begin].reader_kind == Kind::Symbol && raw(file, node.token_begin) != "Nil";
    }

    bool formals(const File &file, NodeId id, bool signature) { return formals_impl(file, id, signature); }

    FormAlias alias(const File &file, const Node &node) {
        if (node.kind != NodeKind::List || node.dot || node.children.size() < 2) return FormAlias::None;
        const auto &head = file.nodes[node.children[0]];
        if (head.kind != NodeKind::Atom) return FormAlias::None;
        struct AliasSpec { std::string_view name; FormAlias alias; };
        static constexpr std::array kAliases{
            AliasSpec{"defun", FormAlias::Define},
            AliasSpec{"define", FormAlias::Define},
            AliasSpec{"lambda", FormAlias::Lambda},
            AliasSpec{"let", FormAlias::Let},
            AliasSpec{"let*", FormAlias::Let},
            AliasSpec{"letrec*", FormAlias::Let},
            AliasSpec{"begin", FormAlias::Sequence},
            AliasSpec{"module", FormAlias::Module},
            AliasSpec{"if", FormAlias::Conditional},
            AliasSpec{"when", FormAlias::Conditional},
            AliasSpec{"unless", FormAlias::Conditional},
            AliasSpec{"cond", FormAlias::ClauseList},
            AliasSpec{"case", FormAlias::ClauseList},
            AliasSpec{"do", FormAlias::Loop},
            AliasSpec{"import", FormAlias::ImportExport},
            AliasSpec{"export", FormAlias::ImportExport},
            AliasSpec{"define-record-type", FormAlias::RecordType},
        };
        const auto name = raw(file, head.token_begin);
        const auto found = std::find_if(kAliases.begin(), kAliases.end(), [&](auto value) { return value.name == name; });
        if (found == kAliases.end()) return FormAlias::None;
        const auto second = node.children[1];
        if (found->alias == FormAlias::Define)
            if (found->name == "defun")
                return is_identifier(file, second) && node.children.size() >= 4 && formals(file, node.children[2], true) ? FormAlias::Define : FormAlias::None;
            return (is_identifier(file, second) && node.children.size() == 3) || formals(file, second, true) ? FormAlias::Define : FormAlias::None;
        if (found->alias == FormAlias::Lambda) return formals(file, second, false) ? FormAlias::Lambda : FormAlias::None;
        if (found->alias == FormAlias::Let) {
            if (found->name == "let") {
                const auto &bindings = file.nodes[second];
                if (bindings.kind != NodeKind::List || bindings.dot) return FormAlias::None;
                std::set<std::string_view> names;
                for (auto binding : bindings.children) {
                    const auto &entry = file.nodes[binding];
                    if (entry.kind != NodeKind::List || entry.dot || entry.children.size() != 2 || !is_identifier(file, entry.children[0])
                        || !names.insert(raw(file, file.nodes[entry.children[0]].token_begin)).second) return FormAlias::None;
                }
                return FormAlias::Let;
            }
            return node.children.size() >= 3 ? FormAlias::Let : FormAlias::None;
        }
        if (found->alias == FormAlias::Sequence) return node.children.size() >= 2 ? FormAlias::Sequence : FormAlias::None;
        if (found->alias == FormAlias::Module) return node.children.size() >= 3 ? FormAlias::Module : FormAlias::None;
        if (found->alias == FormAlias::Conditional) return node.children.size() >= 3 ? FormAlias::Conditional : FormAlias::None;
        if (found->alias == FormAlias::ClauseList) return node.children.size() >= 3 ? FormAlias::ClauseList : FormAlias::None;
        if (found->alias == FormAlias::Branching) return node.children.size() >= 3 ? FormAlias::Branching : FormAlias::None;
        if (found->alias == FormAlias::ImportExport) return node.children.size() >= 2 ? FormAlias::ImportExport : FormAlias::None;
        if (found->alias == FormAlias::RecordType) return node.children.size() >= 2 ? FormAlias::RecordType : FormAlias::None;
        if (found->alias == FormAlias::Loop) return node.children.size() >= 3 ? FormAlias::Loop : FormAlias::None;
        return FormAlias::None;
    }
}

