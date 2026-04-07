#pragma once

#include <optional>
#include <eta/runtime/nanbox.h>

namespace eta::runtime::types {
    using namespace eta::runtime::nanbox;

    /// An unbound or bound logic variable for structural unification.
    /// binding == std::nullopt  →  variable is unbound
    /// binding == some LispVal  →  variable is bound to that value
    struct LogicVar {
        std::optional<LispVal> binding;
    };

}

