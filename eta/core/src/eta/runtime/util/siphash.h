#pragma once

#include <cstddef>
#include <cstdint>

namespace eta::runtime::util {

inline std::uint64_t rotl64(const std::uint64_t x, const int shift) {
    return (x << shift) | (x >> (64 - shift));
}

inline std::uint64_t read_u64_le(const std::uint8_t* bytes) {
    return static_cast<std::uint64_t>(bytes[0])
         | (static_cast<std::uint64_t>(bytes[1]) << 8)
         | (static_cast<std::uint64_t>(bytes[2]) << 16)
         | (static_cast<std::uint64_t>(bytes[3]) << 24)
         | (static_cast<std::uint64_t>(bytes[4]) << 32)
         | (static_cast<std::uint64_t>(bytes[5]) << 40)
         | (static_cast<std::uint64_t>(bytes[6]) << 48)
         | (static_cast<std::uint64_t>(bytes[7]) << 56);
}

/**
 * SipHash-2-4 reference-compatible implementation.
 */
inline std::uint64_t siphash24(const std::uint8_t* data,
                               const std::size_t size,
                               const std::uint64_t k0,
                               const std::uint64_t k1) {
    std::uint64_t v0 = 0x736f6d6570736575ULL ^ k0;
    std::uint64_t v1 = 0x646f72616e646f6dULL ^ k1;
    std::uint64_t v2 = 0x6c7967656e657261ULL ^ k0;
    std::uint64_t v3 = 0x7465646279746573ULL ^ k1;

    auto sip_round = [&]() {
        v0 += v1;
        v1 = rotl64(v1, 13);
        v1 ^= v0;
        v0 = rotl64(v0, 32);

        v2 += v3;
        v3 = rotl64(v3, 16);
        v3 ^= v2;

        v0 += v3;
        v3 = rotl64(v3, 21);
        v3 ^= v0;

        v2 += v1;
        v1 = rotl64(v1, 17);
        v1 ^= v2;
        v2 = rotl64(v2, 32);
    };

    const auto* current = data;
    std::size_t remaining = size;
    while (remaining >= 8) {
        const std::uint64_t m = read_u64_le(current);
        v3 ^= m;
        sip_round();
        sip_round();
        v0 ^= m;

        current += 8;
        remaining -= 8;
    }

    std::uint64_t tail = static_cast<std::uint64_t>(size) << 56;
    for (std::size_t i = 0; i < remaining; ++i) {
        tail |= static_cast<std::uint64_t>(current[i]) << (8 * i);
    }

    v3 ^= tail;
    sip_round();
    sip_round();
    v0 ^= tail;

    v2 ^= 0xff;
    sip_round();
    sip_round();
    sip_round();
    sip_round();

    return v0 ^ v1 ^ v2 ^ v3;
}

} // namespace eta::runtime::util

