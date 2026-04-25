#pragma once

#include <string_view>

namespace eta::jupyter::comm {

/**
 * @brief Jupyter comm target used for actor lifecycle messages.
 */
std::string_view actors_target();

} // namespace eta::jupyter::comm
