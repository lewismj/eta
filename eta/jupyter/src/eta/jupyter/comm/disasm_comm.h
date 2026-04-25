#pragma once

#include <string_view>

namespace eta::jupyter::comm {

/**
 * @brief Jupyter comm target used for disassembly messages.
 */
std::string_view disasm_target();

} // namespace eta::jupyter::comm
