#include "doc.h"

#include <algorithm>
#include <limits>

namespace eta::format {
    Docs::Docs(std::pmr::memory_resource *resource) : nodes_(resource) { add({DocKind::Nil}); }
    DocId Docs::add(Node node) { nodes_.push_back(node); return nodes_.size() - 1; }
    DocId Docs::text(std::string_view value) { return value.empty() ? 0 : add({DocKind::Text, value}); }
    DocId Docs::line() { return add({DocKind::Line}); }
    DocId Docs::soft_line() { return add({DocKind::SoftLine}); }
    DocId Docs::hard_line() { return add({DocKind::HardLine}); }
    DocId Docs::nest(std::size_t amount, DocId child) { return add({DocKind::Nest, {}, child, 0, amount}); }
    DocId Docs::align(DocId child) { return add({DocKind::Align, {}, child}); }
    DocId Docs::group(DocId child) { return add({DocKind::Group, {}, child}); }
    DocId Docs::if_flat(DocId flat, DocId broken) { return add({DocKind::IfFlat, {}, flat, broken}); }
    DocId Docs::concat(DocId left, DocId right) {
        return !left ? right : !right ? left : add({DocKind::Concat, {}, left, right});
    }
    const Docs::Node &Docs::operator[](DocId id) const { return nodes_.at(id); }
    std::pmr::memory_resource *Docs::resource() const { return nodes_.get_allocator().resource(); }

    namespace {
        void checkpoint(const cst::Operation &operation) {
            if (operation.cancelled && operation.cancelled->load(std::memory_order_relaxed))
                throw cst::detail::OperationStopped{{"format.Cancelled", "formatting cancelled", 2}};
        }

        std::size_t columns(std::string_view text) {
            return std::count_if(text.begin(), text.end(), [](unsigned char c) { return (c & 0xC0) != 0x80; });
        }

        bool fits(const Docs &docs, DocId root, std::size_t remaining, const cst::Operation &operation) {
            std::pmr::vector<DocId> pending(docs.resource());
            pending.push_back(root);
            while (!pending.empty()) {
                checkpoint(operation);
                const auto node = docs[pending.back()];
                pending.pop_back();
                switch (node.kind) {
                    case DocKind::Nil: case DocKind::SoftLine: break;
                    case DocKind::HardLine: return false;
                    case DocKind::Line:
                        if (!remaining) return false;
                        --remaining;
                        break;
                    case DocKind::Text: {
                        if (node.text.find_first_of("\r\n") != std::string_view::npos) return false;
                        const auto count = columns(node.text);
                        if (count > remaining) return false;
                        remaining -= count;
                        break;
                    }
                    case DocKind::Concat:
                        pending.push_back(node.right);
                        pending.push_back(node.left);
                        break;
                    case DocKind::IfFlat: case DocKind::Nest: case DocKind::Align: case DocKind::Group:
                        pending.push_back(node.left);
                        break;
                }
            }
            return true;
        }
    }

    std::expected<std::string, cst::Failure> render(const Docs &docs, DocId root,
        std::size_t width, std::string_view newline, const cst::Operation &operation) {
        try {
            if (!width || (newline != "\n" && newline != "\r\n"))
                return std::unexpected(cst::Failure{"format.Options", "invalid width or newline policy", 64});
            struct Frame { DocId id; std::size_t indent; bool flat; };
            std::pmr::vector<Frame> pending(docs.resource());
            pending.push_back({root, 0, false});
            std::string output;
            std::size_t column = 0;
            bool at_line_start = false;
            auto append = [&](std::string_view value) {
                if (value.size() > operation.limits.output_bytes - output.size())
                    throw cst::detail::OperationStopped{{"format.OutputLimit", "formatted output limit exceeded", 2}};
                for (std::size_t offset = 0; offset < value.size(); offset += 4096) {
                    checkpoint(operation);
                    output.append(value.substr(offset, 4096));
                }
            };
            while (!pending.empty()) {
                checkpoint(operation);
                const auto frame = pending.back();
                pending.pop_back();
                const auto node = docs[frame.id];
                switch (node.kind) {
                    case DocKind::Nil: break;
                    case DocKind::Text: {
                        if (at_line_start && !node.text.empty()) {
                            if (frame.indent > operation.limits.output_bytes - output.size())
                                throw cst::detail::OperationStopped{{"format.OutputLimit", "indentation exceeds output limit", 2}};
                            for (std::size_t i = 0; i < frame.indent; i += 4096)
                                append(std::string(std::min<std::size_t>(4096, frame.indent - i), ' '));
                            column = frame.indent;
                            at_line_start = false;
                        }
                        append(node.text);
                        const auto last = node.text.rfind('\n');
                        column = last == std::string_view::npos ? column + columns(node.text) : columns(node.text.substr(last + 1));
                        at_line_start = last != std::string_view::npos && last + 1 == node.text.size();
                        break;
                    }
                    case DocKind::Line: case DocKind::SoftLine: case DocKind::HardLine:
                        if (frame.flat && node.kind != DocKind::HardLine) {
                            if (node.kind == DocKind::Line) { append(" "); ++column; }
                        } else {
                            // The shared lexer includes CR in a CRLF line-comment slice.
                            // Retain that raw slice and emit only its remaining LF.
                            append(newline == "\r\n" && output.ends_with('\r') ? "\n" : newline);
                            column = 0;
                            at_line_start = true;
                        }
                        break;
                    case DocKind::Concat:
                        pending.push_back({node.right, frame.indent, frame.flat});
                        pending.push_back({node.left, frame.indent, frame.flat});
                        break;
                    case DocKind::Nest:
                        if (node.amount > std::numeric_limits<std::size_t>::max() - frame.indent)
                            throw cst::detail::OperationStopped{{"format.OutputLimit", "indentation overflow", 2}};
                        pending.push_back({node.left, frame.indent + node.amount, frame.flat});
                        break;
                    case DocKind::Align: pending.push_back({node.left, at_line_start ? frame.indent : column, frame.flat}); break;
                    case DocKind::IfFlat: pending.push_back({frame.flat ? node.left : node.right, frame.indent, frame.flat}); break;
                    case DocKind::Group:
                        pending.push_back({node.left, frame.indent, frame.flat || fits(docs, node.left,
                            width > (at_line_start ? frame.indent : column) ? width - (at_line_start ? frame.indent : column) : 0, operation)});
                        break;
                }
            }
            return output;
        } catch (const cst::detail::OperationStopped &stop) {
            return std::unexpected(stop.failure);
        } catch (const std::bad_alloc &) {
            return std::unexpected(cst::Failure{"format.MemoryLimit", "document rendering allocation failed", 2});
        } catch (const std::exception &error) {
            return std::unexpected(cst::Failure{"format.Internal", error.what(), 70});
        }
    }
}
