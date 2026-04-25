#pragma once

#include <string_view>

namespace eta::jupyter::comm {

/**
 * @brief Jupyter comm target used for causal DAG visualisation payloads.
 */
std::string_view dag_target();

} // namespace eta::jupyter::comm
