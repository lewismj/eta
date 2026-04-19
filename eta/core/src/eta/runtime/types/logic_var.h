#pragma once

#include <optional>
#include <string>
#include <unordered_map>
#include <eta/runtime/nanbox.h>
#include <eta/runtime/memory/intern_table.h>

namespace eta::runtime::types {
    using namespace eta::runtime::nanbox;

    /**
     * An unbound or bound logic variable for structural unification.
     *
     * `name` is an optional debug label (empty string when unset).
     * `(var-name v)`, and error messages.
     *
     * `attrs` is the attributed-variable map.
     * Each key is an interned symbol (the "attribute module" identifier
     * chosen by the library author, e.g., `clp.fd`, `freeze`, `dif`),
     * and the value is an arbitrary LispVal payload the library keeps
     * associated with this var.  An unbound LogicVar whose `attrs` map
     * is non-empty is an *attributed variable*: `(attr-var? v)` returns
     * #t iff `!attrs.empty()`.  Attribute writes are trailed as
     * `TrailEntry::Kind::Attr` so they unwind correctly on backtracking.
     */
    struct LogicVar {
        std::optional<LispVal>                            binding;
        std::string                                       name;
        std::unordered_map<memory::intern::InternId,
                           LispVal>                       attrs;
    };

}

