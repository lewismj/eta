#pragma once

#include <memory_resource>
#include <string_view>

#include "eta/cst/parse.h"

namespace eta::format {
    using DocId = std::size_t;
    enum class DocKind { Nil, Text, Line, SoftLine, HardLine, Nest, Align, Group, IfFlat, Concat };

    /// Text views must retain their source owner until rendering ends.
    class Docs {
    public:
        struct Node {
            DocKind kind;
            std::string_view text{};
            DocId left{};
            DocId right{};
            std::size_t amount{};
        };

        explicit Docs(std::pmr::memory_resource *resource = std::pmr::get_default_resource());
        DocId text(std::string_view value);
        DocId line();
        DocId soft_line();
        DocId hard_line();
        DocId nest(std::size_t amount, DocId child);
        DocId align(DocId child);
        DocId group(DocId child);
        DocId if_flat(DocId flat, DocId broken);
        DocId concat(DocId left, DocId right);
        const Node &operator[](DocId id) const;
        std::pmr::memory_resource *resource() const;

    private:
        DocId add(Node node);
        std::pmr::vector<Node> nodes_;
    };

    std::expected<std::string, cst::Failure> render(const Docs &docs, DocId root,
        std::size_t width, std::string_view newline, const cst::Operation &operation = {});
}
