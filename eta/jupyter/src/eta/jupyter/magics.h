#pragma once

#include <string>
#include <string_view>

namespace eta::jupyter {

/**
 * @brief Kind of parsed Jupyter magic directive.
 */
enum class MagicKind {
    None,
    Line,
    Cell,
};

/**
 * @brief Normalized identifier for supported Jupyter magics.
 */
enum class MagicName {
    Unknown,
    Time,
    Timeit,
    Bytecode,
    Load,
    Run,
    Env,
    Cwd,
    Import,
    Reload,
    Who,
    Whos,
    Plot,
    Table,
    Trace,
};

/**
 * @brief Parsed representation of a `%` or `%%` directive.
 */
struct ParsedMagic {
    MagicKind kind{MagicKind::None};
    MagicName name{MagicName::Unknown};
    std::string name_text;
    std::string args;
    std::string body;
};

/**
 * @brief Parse the first line/cell magic from a cell.
 *
 * Returns `kind == MagicKind::None` when the cell is not a magic directive.
 */
[[nodiscard]] ParsedMagic parse_magic(std::string_view code);

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

/**
 * @brief Convert a parsed magic enum to its canonical text label.
 */
[[nodiscard]] std::string_view magic_name(MagicName name);

} // namespace eta::jupyter
