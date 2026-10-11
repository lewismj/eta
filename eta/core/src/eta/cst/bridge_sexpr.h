#pragma once

#include "parse.h"

namespace eta::cst {
    /// Convert concrete structure using the shared atom decoder and module
    /// normalization. Suppressed data and directives never become expressions.
    std::expected<std::vector<reader::parser::SExprPtr>, Failure> to_sexpr(const ParseResult &parsed);

    /// Independent oracle: fresh original reader at BOF under the same config.
    using OriginalReadError = std::variant<reader::parser::ReaderError, Failure>;

    std::expected<std::vector<reader::parser::SExprPtr>, OriginalReadError>
    read_original(const File &file, bool compare_trace = false);

    bool equivalent_sexpr(const reader::parser::SExpr &left, const reader::parser::SExpr &right);
}
