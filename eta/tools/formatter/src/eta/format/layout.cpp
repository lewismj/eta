#include "layout.h"

#include <algorithm>
#include <optional>
#include <set>

#include "forms.h"

namespace eta::format {
    namespace {
        using namespace cst;

        std::string_view raw(const File &file, TokenId token) { return file.source->slice(file.tokens[token].span); }

        ByteSpan extent(const File &file, const Node &node) {
            return {file.tokens[node.token_begin].span.begin, file.tokens[node.token_end - 1].span.end};
        }
    }

    Layout::Layout(const File &file, const Options &options, std::vector<Coverage> &coverage)
        : file_(file), options_(options), coverage_(coverage), docs_(file.resource()) {}

    std::expected<std::string, Failure> Layout::run() {
        for (const auto &trivia : file_.trivia) {
            if (trivia.kind == TriviaKind::LineComment && file_.source->slice(trivia.span).find("eta-lint:disable-line=") != std::string_view::npos) {
                file_.account_external_bytes(sizeof(Coverage) + 256);
                coverage_.push_back({"physical-line lint directive retains the entire source line context", {0, file_.source->text().size()}});
                return std::string(file_.source->text());
            }
        }
        DocId result = 0;
        std::size_t next = 0;
        for (auto id : file_.roots) {
            const auto &node = file_.nodes[id];
            while (next < node.token_begin) {
                result = docs_.concat(result, header(next));
                ++next;
            }
            result = docs_.concat(result, node.token_begin == 0 ? header_gap(0) : gap(node.token_begin, docs_.hard_line(), true));
            result = docs_.concat(result, build(id));
            next = node.token_end;
        }
        while (next + 1 < file_.tokens.size()) { result = docs_.concat(result, header(next)); ++next; }
        const auto eof = file_.tokens.size() - 1;
        result = docs_.concat(result, eof == 0 ? header_gap(eof) : gap(eof, 0, true));
        return render(docs_, result, options_.width, options_.newline->kind == Newline::Crlf ? "\r\n" : "\n", file_.operation);
    }

    DocId Layout::header_gap(TokenId token) {
        const auto &item = file_.tokens[token];
        if (item.trivia_begin == item.trivia_end) return 0;
        return docs_.text(file_.source->slice({file_.trivia[item.trivia_begin].span.begin, item.span.begin}));
    }

    DocId Layout::header(TokenId token) { return docs_.concat(header_gap(token), docs_.text(raw(file_, token))); }

    DocId Layout::breaks(std::size_t count) {
        DocId result = 0;
        for (std::size_t i = 0; i < count; ++i) result = docs_.concat(result, docs_.hard_line());
        return result;
    }

    DocId Layout::gap(TokenId token, DocId ordinary, bool blank_lines) {
        const auto &item = file_.tokens[token];
        const auto begin = token ? file_.tokens[token - 1].span.end : 0;
        std::size_t cursor = begin;
        DocId result = 0;
        bool comments = false;
        for (auto i = item.trivia_begin; i < item.trivia_end; ++i) {
            const auto &trivia = file_.trivia[i];
            if (trivia.kind == TriviaKind::Whitespace) continue;
            comments = true;
            const auto whitespace = file_.source->slice({cursor, trivia.span.begin});
            const auto lines = std::count(whitespace.begin(), whitespace.end(), '\n');
            result = docs_.concat(result, lines ? breaks(lines) : docs_.text(" "));
            result = docs_.concat(result, docs_.text(file_.source->slice(trivia.span)));
            cursor = trivia.span.end;
        }
        const auto whitespace = file_.source->slice({cursor, item.span.begin});
        const auto lines = std::count(whitespace.begin(), whitespace.end(), '\n');
        if (comments) return docs_.concat(result, lines ? breaks(lines) : token == file_.tokens.size() - 1 ? 0 : docs_.text(" "));
        if (blank_lines && lines > 1) return breaks(2);
        return ordinary;
    }

    bool Layout::line_directive(const Node &node) const {
        for (auto i = node.token_begin; i < node.token_end; ++i) {
            const auto &token = file_.tokens[i];
            for (auto j = token.trivia_begin; j < token.trivia_end; ++j) {
                const auto &trivia = file_.trivia[j];
                if (trivia.kind != TriviaKind::LineComment) continue;
                const auto text = file_.source->slice(trivia.span);
                if (text.find("eta-lint:disable-line=") != std::string_view::npos) return true;
            }
        }
        return false;
    }

    DocId Layout::opaque(const Node &node, std::string reason) {
        const auto span = extent(file_, node);
        file_.account_external_bytes(sizeof(Coverage) + reason.capacity() + 64);
        coverage_.push_back({std::move(reason), span});
        return docs_.text(file_.source->slice(span));
    }

    DocId Layout::build(NodeId id) {
        file_.checkpoint();
        const auto &node = file_.nodes[id];
        if (node.kind == NodeKind::DatumComment) return opaque(node, "discarded datum retained under the compiler's permissive discard grammar");
        if (line_directive(node)) return opaque(node, "line-bound lint directive retains its original applicability");
        if (node.kind == NodeKind::Atom || node.kind == NodeKind::Directive) return docs_.text(raw(file_, node.token_begin));
        DocId result = docs_.text(raw(file_, node.token_begin));
        if (node.kind == NodeKind::ReaderForm) {
            for (auto child : node.children) {
                const auto token = file_.nodes[child].token_begin;
                const auto separator = token == node.token_begin + 1 ? DocId{0} : docs_.text(" ");
                result = docs_.concat(result, gap(token, separator));
                result = docs_.concat(result, build(child));
            }
            return docs_.align(result);
        }
        const auto chosen = alias(file_, node);
        bool body_gaps = false;
        if (!node.children.empty()) {
            const auto &head = file_.nodes[node.children[0]];
            body_gaps = head.kind == NodeKind::Atom && (raw(file_, head.token_begin) == "begin" || raw(file_, head.token_begin) == "module");
        }
        struct Part { TokenId token; std::optional<NodeId> node; };
        std::pmr::vector<Part> parts(file_.resource());
        for (auto child : node.children) parts.push_back({file_.nodes[child].token_begin, child});
        if (node.dot) parts.push_back({*node.dot, {}});
        if (node.tail) parts.push_back({file_.nodes[*node.tail].token_begin, *node.tail});
        std::sort(parts.begin(), parts.end(), [](auto a, auto b) { return a.token < b.token; });
        // Recognised forms keep their leading operands (name, formals, bindings,
        // test) on the head line: `defun`/`defmacro` have two (name and
        // formals), the rest one. A sequence has none, so its body breaks like
        // any other list. Doc extraction relies on the name staying on the head line.
        std::size_t head_operands = 0;
        if (chosen != FormAlias::None && chosen != FormAlias::Sequence) {
            const auto head_name = raw(file_, file_.nodes[node.children[0]].token_begin);
            head_operands = chosen == FormAlias::Define && (head_name == "defun" || head_name == "defmacro") ? 2 : 1;
        }
        // A list headed by another list (let bindings, clause lists, alists) is
        // data, not a call: align its elements under the first one.
        const bool data_list = chosen == FormAlias::None && !node.children.empty()
            && file_.nodes[node.children[0]].kind == NodeKind::List;
        const std::size_t continuation = data_list ? 1 : options_.indent;
        for (std::size_t i = 0; i < parts.size(); ++i) {
            const auto part = parts[i];
            const bool on_head_line = i > 0 && i <= head_operands;
            auto separator = i == 0 ? DocId{0} : on_head_line ? docs_.text(" ") : docs_.line();
            auto content = docs_.concat(gap(part.token, separator, body_gaps), part.node ? build(*part.node) : docs_.text(raw(file_, part.token)));
            if (i && !on_head_line) content = docs_.nest(continuation, content);
            result = docs_.concat(result, content);
        }
        const auto close = node.token_end - 1;
        result = docs_.concat(result, gap(close, 0));
        result = docs_.concat(result, docs_.text(raw(file_, close)));
        return docs_.group(docs_.align(result));
    }
}

