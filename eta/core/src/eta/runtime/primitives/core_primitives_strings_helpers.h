#pragma once

#include <expected>
#include <string>
#include <string_view>
#include <utility>

#include "eta/runtime/core_primitives_internal.h"
#include "eta/runtime/string_view.h"

namespace eta::runtime::detail::core_primitives_strings {

/**
 * @brief Validate a string argument and return its StringView.
 */
inline std::expected<StringView, RuntimeError> expect_string_arg(
    LispVal value,
    InternTable& intern_table,
    const char* op_name,
    const char* position) {
    auto sv = StringView::try_from(value, intern_table);
    if (!sv) {
        return std::unexpected(RuntimeError{VMError{
            RuntimeErrorCode::TypeError,
            std::string(op_name) + ": " + position + " is not a string"}});
    }
    return *sv;
}

/**
 * @brief Evaluate a two-argument string comparison primitive.
 */
template <typename Compare>
inline std::expected<LispVal, RuntimeError> compare_two_strings(
    PrimReg::Args args,
    InternTable& intern_table,
    const char* op_name,
    Compare&& compare) {
    auto lhs = expect_string_arg(args[0], intern_table, op_name, "first argument");
    if (!lhs) return std::unexpected(lhs.error());

    auto rhs = expect_string_arg(args[1], intern_table, op_name, "second argument");
    if (!rhs) return std::unexpected(rhs.error());

    return std::forward<Compare>(compare)(lhs->view(), rhs->view())
        ? nanbox::True
        : nanbox::False;
}

} // namespace eta::runtime::detail::core_primitives_strings
