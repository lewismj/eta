#include "bridge_sexpr.h"

#include <cmath>
#include <array>
#include <bit>
#include <type_traits>

namespace eta::cst {
    namespace {
        namespace parser = reader::parser;
        using Kind = reader::lexer::Token::Kind;

        parser::Span span(const File &file, const Node &node) {
            const auto begin = file.tokens[node.token_begin].span.begin;
            const auto end = file.tokens[node.token_end - 1].span.end;
            const auto first = *file.source->position(begin, PositionEncoding::Utf8);
            const auto last = *file.source->position(end, PositionEncoding::Utf8);
            return {
                file.source->id(),
                {begin, static_cast<std::uint32_t>(first.line + 1), static_cast<std::uint32_t>(first.character + 1)},
                {end, static_cast<std::uint32_t>(last.line + 1), static_cast<std::uint32_t>(last.character + 1)}
            };
        }

        bool active(const Node &node) {
            return !node.discarded && node.kind != NodeKind::DatumComment && node.kind != NodeKind::Directive;
        }

        parser::SExprPtr convert(const File &file, NodeId id) {
            file.checkpoint();
            const auto &node = file.nodes[id];
            if (!active(node)) return {};
            if (node.kind == NodeKind::Error || node.virtual_close || node.verbatim)
                throw detail::OperationStopped{
                    {"cst.BridgeRecovery", "cannot bridge recovered or unsupported syntax", 2}
                };
            auto result = std::make_unique<parser::SExpr>();
            const auto location = span(file, node);
            if (node.kind == NodeKind::Atom || node.kind == NodeKind::BytevectorLiteral) {
                // Reuse the atom and byte-literal conversions, including their different
                // overflow/radix rules. Lists and reader forms are bridged structurally.
                const auto begin = file.tokens[node.token_begin].span.begin;
                const auto end = file.tokens[node.token_end - 1].span.end;
                reader::lexer::ScanObserver observer;
                observer.checkpoint = [&] { file.checkpoint(); };
                reader::lexer::Lexer lexer(file.source->id(), file.source->slice({begin, end}), &observer);
                parser::Parser reader(lexer, file.config.strict_quasiquote);
                auto decoded = reader.parse_datum();
                if (!decoded) throw detail::OperationStopped{
                    {"cst.BridgeDecode", "shared decoder rejected concrete structure", 70}
                };
                std::visit([&](auto &value) { value.span = location; }, (*decoded)->value);
                return std::move(*decoded);
            }
            std::vector<parser::SExprPtr> children;
            for (const auto child: node.children)
                if (auto value = convert(file, child)) children.push_back(std::move(value));
            switch (node.kind) {
                case NodeKind::List: {
                    parser::List list;
                    list.span = location;
                    list.elems = std::move(children);
                    list.dotted = node.dot.has_value();
                    if (node.tail) list.tail = convert(file, *node.tail);
                    if (list.dotted && (!list.tail || list.elems.empty()))
                        throw detail::OperationStopped{{"cst.BridgeList", "invalid dotted-list structure", 70}};
                    result->value = std::move(list);
                    break;
                }
                case NodeKind::VectorLiteral: {
                    parser::Vector vector;
                    vector.span = location;
                    vector.elems = std::move(children);
                    result->value = std::move(vector);
                    break;
                }
                case NodeKind::ReaderForm: {
                    if (children.size() != 1) throw detail::OperationStopped{
                        {"cst.BridgePrefix", "reader prefix must have one active operand", 70}
                    };
                    parser::ReaderForm form;
                    form.span = location;
                    const auto kind = file.tokens[node.token_begin].reader_kind;
                    form.kind = kind == Kind::Quote
                                    ? parser::QuoteKind::Quote
                                    : kind == Kind::Backtick
                                          ? parser::QuoteKind::Quasiquote
                                          : kind == Kind::Comma
                                                ? parser::QuoteKind::Unquote
                                                : parser::QuoteKind::UnquoteSplicing;
                    form.expr = std::move(children.front());
                    result->value = std::move(form);
                    break;
                }
                default: throw detail::OperationStopped{{"cst.BridgeKind", "unexpected active CST node", 70}};
            }
            return result;
        }

        bool expressions(const std::vector<parser::SExprPtr> &left, const std::vector<parser::SExprPtr> &right) {
            if (left.size() != right.size()) return false;
            for (std::size_t i = 0; i < left.size(); ++i)
                if (!equivalent_sexpr(*left[i], *right[i])) return false;
            return true;
        }
    }

    std::expected<std::vector<parser::SExprPtr>, Failure> to_sexpr(const ParseResult &parsed) {
        if (!parsed.file || parsed.status != Status::Valid)
            return std::unexpected(Failure{"cst.BridgeStatus", "CST bridge requires valid supported input", 2});
        try {
            const auto &file = *parsed.file;
            file.check_reader_budget();
            std::vector<parser::SExprPtr> forms;
            for (const auto id: file.roots) {
                if (auto form = convert(file, id)) {
                    if (auto *list = form->as<parser::List>())
                        if (auto module = parser::Parser::recognize_module(*list)) form->value = std::move(*module);
                    forms.push_back(std::move(form));
                }
            }
            file.checkpoint();
            return forms;
        } catch (const detail::OperationStopped &stop) {
            return std::unexpected(stop.failure);
        } catch (const std::bad_alloc &) {
            return std::unexpected(Failure{"cst.MemoryLimit", "bridge allocation failed", 2});
        }
    }

    std::expected<std::vector<parser::SExprPtr>, OriginalReadError>
    read_original(const File &file, bool compare_trace) {
        try {
            file.checkpoint();
            file.check_reader_budget();
            reader::lexer::ScanObserver observer;
            observer.checkpoint = [&] { file.checkpoint(); };
            parser::Parser *active_parser = nullptr;
            std::size_t token_id = 0;
            bool agrees = true;
            if (compare_trace)
                observer.item = [&](const reader::lexer::RawItem &raw) {
                    using RawKind = reader::lexer::RawItem::Kind;
                    if (raw.kind == RawKind::Whitespace || raw.kind == RawKind::LineComment || raw.kind ==
                        RawKind::BlockComment) return;
                    if (token_id >= file.tokens.size()) {
                        agrees = false;
                        return;
                    }
                    const auto &expected = file.tokens[token_id++];
                    const ReaderState before{
                        raw.at_bof_before, file.config.strict_quasiquote,
                        static_cast<std::size_t>(active_parser->quasiquote_depth()),
                        raw.at_bof_before && raw.span.start.line == 1 && raw.span.start.column == 1
                    };
                    const ReaderState after{raw.at_bof_after, file.config.strict_quasiquote, before.quasiquote_depth,
                        raw.at_bof_after && raw.span.end.line == 1 && raw.span.end.column == 1};
                    agrees = agrees && expected.before == before && expected.after == after;
                };
            reader::lexer::Lexer lexer(file.source->id(), file.source->text(), &observer);
            parser::Parser parser(lexer, file.config.strict_quasiquote);
            active_parser = &parser;
            auto result = parser.parse_toplevel();
            if (compare_trace && result && (!agrees || token_id != file.tokens.size()))
                throw detail::OperationStopped{{"cst.ReaderDisagreement", "reader state trace disagrees with CST", 2}};
            file.checkpoint();
            if (!result) return std::unexpected(OriginalReadError{result.error()});
            return std::move(*result);
        } catch (const detail::OperationStopped &stop) {
            return std::unexpected(OriginalReadError{stop.failure});
        } catch (const std::bad_alloc &) {
            return std::unexpected(OriginalReadError{Failure{"cst.MemoryLimit", "reader allocation failed", 2}});
        }
    }

    bool equivalent_sexpr(const parser::SExpr &left, const parser::SExpr &right) {
        if (left.value.index() != right.value.index()) return false;
        return std::visit([&](const auto &a) {
            using T = std::decay_t<decltype(a)>;
            const auto &b = std::get<T>(right.value);
            if constexpr (std::is_same_v<T, parser::Nil>) return true;
            else if constexpr (std::is_same_v<T, parser::Symbol>) return a.name == b.name;
            else if constexpr (std::is_same_v<T, parser::Number>) {
                if (a.value.index() != b.value.index()) return false;
                return std::visit([&](auto number) {
                    using N = decltype(number);
                    const auto other = std::get<N>(b.value);
                    if constexpr (std::is_floating_point_v<N>)
                        return std::bit_cast<std::array<std::byte, sizeof(N)>>(number)
                            == std::bit_cast<std::array<std::byte, sizeof(N)>>(other);
                    else return number == other;
                }, a.value);
            } else if constexpr (std::is_same_v<T, parser::List>)
                return a.dotted == b.dotted && expressions(a.elems, b.elems) &&
                       (!a.dotted || (a.tail && b.tail && equivalent_sexpr(*a.tail, *b.tail)));
            else if constexpr (std::is_same_v<T, parser::Vector>) return expressions(a.elems, b.elems);
            else if constexpr (std::is_same_v<T, parser::ByteVector>) return a.bytes == b.bytes;
            else if constexpr (std::is_same_v<T, parser::ReaderForm>) return
                    a.kind == b.kind && equivalent_sexpr(*a.expr, *b.expr);
            else if constexpr (std::is_same_v<T, parser::ModuleForm>) return
                    a.name == b.name && a.exports == b.exports && expressions(a.body, b.body);
            else return a.value == b.value;
        }, left.value);
    }
}
