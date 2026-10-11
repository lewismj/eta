#include <cstdlib>

#include "eta/format/format_source.h"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t *data, std::size_t size) {
    if (size > 1024 * 1024) return 0;
    const std::string source(reinterpret_cast<const char *>(data), size);
    auto result = eta::format::format_source(source);
    if (result.output) {
        const auto second = eta::format::format_source(*result.output, result.effective_options);
        if (!second.output || *second.output != *result.output) std::abort();
    } else if (result.exit_class == 70) std::abort();
    return 0;
}
