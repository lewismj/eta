#pragma once

#include <cstdint>
#include <variant>

namespace eta {
    using Number = std::variant<int64_t, double>;
}
