#pragma once

#include <vector>
#include <cstdint>

namespace eta::runtime::types {

    struct ByteVector {
        std::vector<std::uint8_t> data {};
    };

}
