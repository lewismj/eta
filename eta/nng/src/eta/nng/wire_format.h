#pragma once

#include <expected>
#include <string>
#include <string_view>

#include "eta/runtime/datum_reader.h"
#include "eta/runtime/value_formatter.h"

namespace eta::nng {

using namespace eta::runtime;
using namespace eta::runtime::nanbox;
using namespace eta::runtime::error;
using namespace eta::runtime::memory::heap;
using namespace eta::runtime::memory::intern;

/**
 * @brief Serialize a LispVal to its s-expression (Write-mode) string.
 *
 * Uses the existing format_value() in Write mode, which produces
 * machine-readable output (quoted strings, #\ char syntax, etc.)
 * suitable for round-trip through deserialize_value().
 *
 * Non-serializable values (closures, continuations, ports, tensors)
 * will produce opaque strings like "#<closure>" that cannot be
 * deserialized.
 */
inline std::string serialize_value(LispVal v, Heap& heap, InternTable& intern) {
    return format_value(v, FormatMode::Write, heap, intern);
}

/**
 * @brief Deserialize an s-expression string back to a LispVal.
 *
 * Parses the string using the Eta reader (Lexer + Parser) and
 * converts the resulting AST into runtime heap objects.
 *
 * @param data   The s-expression text to parse.
 * @param heap   The runtime heap for allocating objects.
 * @param intern The intern table for strings and symbols.
 * @return The deserialized LispVal, or an error on malformed input.
 */
inline std::expected<LispVal, RuntimeError>
deserialize_value(std::string_view data, Heap& heap, InternTable& intern) {
    return parse_datum_string(data, heap, intern);
}

} // namespace eta::nng

