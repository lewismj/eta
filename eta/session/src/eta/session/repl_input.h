/**
 * @file repl_input.h
 * @brief Shared helpers for splitting and validating REPL input.
 */

#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace eta::session {

/**
 * @brief Check whether REPL input is complete.
 *
 * Completeness accounts for parenthesis depth, nested block comments, string
 * literals with escapes, and dotted continuation lines.
 *
 * @param src Source text to inspect.
 * @param indent_hint Optional continuation indentation hint.
 * @return true when source is complete, false when more input is required.
 */
[[nodiscard]] bool is_complete_repl_input(std::string_view src,
                                          std::string* indent_hint = nullptr);

/**
 * @brief Split REPL source text into top-level forms.
 *
 * Each returned element is one top-level list form or bare atom expression.
 */
[[nodiscard]] std::vector<std::string> split_toplevel_forms(std::string_view input);

} // namespace eta::session
