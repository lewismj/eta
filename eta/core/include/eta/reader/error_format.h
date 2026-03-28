#pragma once

#include "eta/reader/parser.h"
#include <ostream>

namespace eta::reader {

    inline void write_span(std::ostream& os, const parser::Span& sp) {
        os << "[file " << sp.file_id
           << ":" << sp.start.line << ":" << sp.start.column
           << "-" << sp.end.line << ":" << sp.end.column << "]";
    }

} // namespace eta::reader
