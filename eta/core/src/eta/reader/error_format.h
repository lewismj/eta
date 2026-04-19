#pragma once

#include "eta/reader/parser.h"
#include <functional>
#include <ostream>
#include <string>

namespace eta::reader {

    using FileResolver = std::function<std::string(uint32_t file_id)>;

    inline void write_span(std::ostream& os, const parser::Span& sp,
                            const FileResolver& resolve_file = {}) {
        os << "[";
        if (resolve_file) {
            auto name = resolve_file(sp.file_id);
            if (!name.empty())
                os << name;
            else
                os << "file " << sp.file_id;
        } else {
            os << "file " << sp.file_id;
        }
        os << ":" << sp.start.line << ":" << sp.start.column
           << "-" << sp.end.line << ":" << sp.end.column << "]";
    }

} ///< namespace eta::reader

