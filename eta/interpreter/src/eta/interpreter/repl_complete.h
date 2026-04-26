#pragma once

#include <cstddef>
#include <string>
#include <string_view>

namespace eta::interpreter::repl_complete {

/**
 * @brief Token slice around a cursor position in REPL-style source text.
 */
struct CursorToken {
    std::string text;
    std::size_t start{0};
    std::size_t end{0};
};

/**
 * @brief Return true when @p c is part of an Eta symbol token.
 */
inline bool is_symbol_char(char c) noexcept {
    if (c <= ' ') return false;
    if (c == '(' || c == ')' || c == '[' || c == ']' ||
        c == '"' || c == ';' || c == '#') {
        return false;
    }
    return true;
}

/**
 * @brief Extract the symbol token surrounding @p cursor_pos.
 *
 * If the cursor is between tokens, this returns the token immediately to the
 * left when possible.  When no symbol token is adjacent, the returned token is
 * empty and start/end are set to the clamped cursor offset.
 */
inline CursorToken token_at(std::string_view source, std::size_t cursor_pos) {
    if (cursor_pos > source.size()) cursor_pos = source.size();

    std::size_t pos = cursor_pos;
    if (pos < source.size() && is_symbol_char(source[pos])) {
        /// Cursor is on a symbol character.
    } else if (pos > 0 && is_symbol_char(source[pos - 1])) {
        /// Cursor is just after a symbol character.
        --pos;
    } else {
        return CursorToken{.text = {}, .start = cursor_pos, .end = cursor_pos};
    }

    std::size_t start = pos;
    while (start > 0 && is_symbol_char(source[start - 1])) --start;

    std::size_t end = pos;
    while (end < source.size() && is_symbol_char(source[end])) ++end;

    return CursorToken{
        .text = std::string(source.substr(start, end - start)),
        .start = start,
        .end = end,
    };
}

} ///< namespace eta::interpreter::repl_complete
