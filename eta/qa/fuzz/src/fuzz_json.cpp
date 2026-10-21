#include <cstddef>
#include <cstdint>
#include <string_view>

#include "eta/util/json.h"

/**
 * Fuzz target for eta::json (util/json.h), the parser behind the LSP and DAP
 * servers. Malformed input must raise json::ParseError; any other exception,
 * crash or stack overflow is a finding.
 */

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    const std::string_view input(reinterpret_cast<const char*>(data), size);
    try {
        auto value = eta::json::parse(input);
        (void)eta::json::to_string(value);
    } catch (const eta::json::ParseError&) {
    }
    return 0;
}
