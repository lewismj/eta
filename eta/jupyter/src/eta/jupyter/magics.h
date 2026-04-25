#pragma once

#include <string>
#include <string_view>

namespace eta::jupyter {

/**
 * @brief Check whether the input line starts with a Jupyter-style magic prefix.
 *
 * @param line Source line from a cell.
 * @return True when the line starts with '%' or '%%'.
 */
bool is_magic(std::string_view line);

/**
 * @brief Remove the leading magic prefix from a line.
 *
 * @param line Source line from a cell.
 * @return Source line with one or two leading '%' characters removed.
 */
std::string strip_magic_prefix(std::string_view line);

} // namespace eta::jupyter
