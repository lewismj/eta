#pragma once

#include <optional>

#include "eta/runtime/core_primitives_internal.h"

namespace eta::runtime::detail::core_primitives_logic {

inline std::optional<memory::intern::InternId> get_symbol_id(LispVal value) {
    if (!ops::is_boxed(value) || ops::tag(value) != Tag::Symbol) return std::nullopt;
    return static_cast<memory::intern::InternId>(ops::payload(value));
}

} // namespace eta::runtime::detail::core_primitives_logic

