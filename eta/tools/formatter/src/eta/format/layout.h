#pragma once

#include <expected>
#include <string>
#include <string_view>
#include <vector>

#include "eta/cst/parse.h"
#include "eta/format/doc.h"
#include "eta/format/format_source.h"

namespace eta::format {
    class Layout {
    public:
        Layout(const cst::File &file, const Options &options, std::vector<Coverage> &coverage);
        std::expected<std::string, cst::Failure> run();

    private:
        const cst::File &file_;
        const Options &options_;
        std::vector<Coverage> &coverage_;
        Docs docs_;

        DocId header_gap(cst::TokenId token);
        DocId header(cst::TokenId token);
        DocId breaks(std::size_t count);
        DocId gap(cst::TokenId token, DocId ordinary, bool blank_lines = false);
        bool line_directive(const cst::Node &node) const;
        DocId opaque(const cst::Node &node, std::string reason);
        DocId build(cst::NodeId id);
    };
}
