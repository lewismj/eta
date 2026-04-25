#pragma once

#include <string_view>

namespace eta::jupyter::comm {

/**
 * @brief Jupyter comm target used for tensor explorer payloads.
 */
std::string_view tensor_target();

} // namespace eta::jupyter::comm
