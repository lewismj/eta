/**
 * @file repl_input.cpp
 * @brief Shared helpers for splitting and validating REPL input.
 */

#include "eta/session/repl_input.h"

#include <cctype>

namespace eta::session {

bool is_complete_repl_input(std::string_view src, std::string* indent_hint) {
    int paren_depth = 0;
    int block_comment_depth = 0;
    bool in_string = false;
    bool in_line_comment = false;
    bool escape = false;

    if (indent_hint != nullptr) {
        indent_hint->clear();
    }

    for (std::size_t i = 0; i < src.size(); ++i) {
        const char c = src[i];
        const char next = (i + 1 < src.size()) ? src[i + 1] : '\0';

        if (in_line_comment) {
            if (c == '\n') in_line_comment = false;
            continue;
        }

        if (block_comment_depth > 0) {
            if (c == '#' && next == '|') {
                ++block_comment_depth;
                ++i;
                continue;
            }
            if (c == '|' && next == '#') {
                --block_comment_depth;
                ++i;
            }
            continue;
        }

        if (in_string) {
            if (escape) {
                escape = false;
                continue;
            }
            if (c == '\\') {
                escape = true;
                continue;
            }
            if (c == '"') in_string = false;
            continue;
        }

        if (c == ';') {
            in_line_comment = true;
            continue;
        }

        if (c == '#' && next == '|') {
            ++block_comment_depth;
            ++i;
            continue;
        }

        if (c == '"') {
            in_string = true;
            continue;
        }

        if (c == '(') ++paren_depth;
        else if (c == ')' && paren_depth > 0) --paren_depth;
    }

    bool dot_prefixed_continuation = false;
    std::size_t line_start = 0;
    while (line_start <= src.size()) {
        std::size_t line_end = src.find('\n', line_start);
        if (line_end == std::string_view::npos) line_end = src.size();

        const auto line = src.substr(line_start, line_end - line_start);
        const auto first_non_ws = line.find_first_not_of(" \t\r");
        if (first_non_ws != std::string_view::npos) {
            const auto trimmed = line.substr(first_non_ws);
            if (!trimmed.empty() && trimmed.front() != ';') {
                dot_prefixed_continuation = (trimmed.front() == '.');
            }
        }

        if (line_end == src.size()) break;
        line_start = line_end + 1;
    }

    const bool complete =
        (paren_depth == 0) &&
        !in_string &&
        (block_comment_depth == 0) &&
        !dot_prefixed_continuation;

    if (!complete && indent_hint != nullptr) {
        if (paren_depth > 0) {
            indent_hint->assign(static_cast<std::size_t>(paren_depth) * 2u, ' ');
        } else if (dot_prefixed_continuation) {
            *indent_hint = "  ";
        } else {
            indent_hint->clear();
        }
    }

    return complete;
}

std::vector<std::string> split_toplevel_forms(std::string_view input) {
    std::vector<std::string> forms;
    int depth = 0;
    bool in_string = false;
    bool escape = false;
    std::size_t form_start = std::string_view::npos;

    for (std::size_t i = 0; i < input.size(); ++i) {
        const char c = input[i];

        if (escape) {
            escape = false;
            continue;
        }
        if (c == '\\' && in_string) {
            escape = true;
            continue;
        }
        if (c == '"') {
            in_string = !in_string;
            if (form_start == std::string_view::npos) form_start = i;
            continue;
        }
        if (in_string) continue;

        if (std::isspace(static_cast<unsigned char>(c))) {
            if (depth == 0 && form_start != std::string_view::npos) {
                forms.emplace_back(input.substr(form_start, i - form_start));
                form_start = std::string_view::npos;
            }
            continue;
        }

        if (c == ';') {
            if (depth == 0 && form_start != std::string_view::npos) {
                forms.emplace_back(input.substr(form_start, i - form_start));
                form_start = std::string_view::npos;
            }
            while (i < input.size() && input[i] != '\n') ++i;
            continue;
        }

        if (form_start == std::string_view::npos) form_start = i;

        if (c == '(') {
            ++depth;
        } else if (c == ')') {
            --depth;
            if (depth == 0) {
                forms.emplace_back(input.substr(form_start, i + 1 - form_start));
                form_start = std::string_view::npos;
            }
        }
    }

    if (form_start != std::string_view::npos) {
        const auto trailing = input.substr(form_start);
        if (trailing.find_first_not_of(" \t\n\r") != std::string_view::npos) {
            forms.emplace_back(trailing);
        }
    }

    return forms;
}

} // namespace eta::session
