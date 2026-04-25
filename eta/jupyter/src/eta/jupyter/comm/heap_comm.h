#pragma once

#include <string_view>

namespace eta::jupyter::comm {

/**
 * @brief Jupyter comm target used for heap-inspection messages.
 */
std::string_view heap_target();

} // namespace eta::jupyter::comm
