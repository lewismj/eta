#pragma once

#include <optional>
#include <string>
#include <eta/runtime/nanbox.h>

namespace eta::runtime::types {
    using namespace eta::runtime::nanbox;

    /// An unbound or bound logic variable for structural unification.
    /// binding == std::nullopt  →  variable is unbound
    /// binding == some LispVal  →  variable is bound to that value
    ///
    /// `name` is an optional debug label (empty string when unset).
    /// It has no effect on unification semantics — purely for tracing,
    /// `(var-name v)`, and error messages.
    struct LogicVar {
        std::optional<LispVal> binding;
        std::string            name;
    };

}

