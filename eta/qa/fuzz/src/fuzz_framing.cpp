#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <sstream>
#include <string>

#include "eta/tooling/framing.h"

/**
 * Fuzz target for the LSP/DAP Content-Length framing
 * (eta::tooling::read_framed_message).
 *
 * Oracles:
 *  - no returned body is larger than max_bytes;
 *  - writing the input as one frame and reading it back returns it unchanged.
 */

namespace {
    constexpr std::size_t kMaxBytes = 4096;
    constexpr int kMaxFrames = 64;
}

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    const std::string input(reinterpret_cast<const char*>(data), size);

    std::istringstream in(input, std::ios::binary);
    for (int i = 0; i < kMaxFrames; ++i) {
        auto body = eta::tooling::read_framed_message(in, "fuzz", kMaxBytes);
        if (!body) break;
        if (body->size() > kMaxBytes) std::abort();
    }

    if (!input.empty() && input.size() <= kMaxBytes) {
        std::ostringstream out(std::ios::binary);
        eta::tooling::write_framed_message(out, input);
        std::istringstream round_trip(out.str(), std::ios::binary);
        auto body = eta::tooling::read_framed_message(round_trip, "fuzz", kMaxBytes);
        if (!body || *body != input) std::abort();
    }
    return 0;
}
