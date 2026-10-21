#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <span>

#include "eta/nng/wire_format.h"
#include "eta/runtime/memory/heap.h"
#include "eta/runtime/memory/intern_table.h"

/**
 * Fuzz target for the actor binary wire format (eta::nng::deserialize_binary).
 * Frames arrive from remote peers, so this is the most exposed decoder.
 *
 * Oracle: anything that decodes must re-encode to a buffer that decodes again.
 */

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    using namespace eta::runtime::memory;

    heap::Heap heap{4 * 1024 * 1024};
    intern::InternTable intern;

    auto decoded = eta::nng::deserialize_binary(std::span<const std::uint8_t>(data, size), heap, intern);
    if (!decoded) return 0;

    auto encoded = eta::nng::serialize_binary(*decoded, heap, intern);
    if (encoded.empty()) return 0;
    auto again = eta::nng::deserialize_binary(std::span<const std::uint8_t>(encoded), heap, intern);
    if (!again) std::abort();
    return 0;
}
