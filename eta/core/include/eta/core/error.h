#pragma once

#include "eta/reader/lexer.h"
#include <string>

namespace eta {

    struct SourceError {
        lexer::Span span{};
        std::string message;
        std::string phase; // e.g., "Lexer", "Parser", "Expander", "Linker", "Semantic"

        SourceError(lexer::Span s, std::string msg, std::string p)
            : span(s), message(std::move(msg)), phase(std::move(p)) {}
    };

} // namespace eta
