#pragma once

#include <vector>
#include <eta/runtime/nanbox.h>

namespace eta::runtime::types {
    using namespace eta::runtime::nanbox;

    /// A structured logic term with a symbol functor and zero or more arguments.
    ///
    /// Built by `(term 'f a1 a2 ... aN)`; inspected by `(functor t)`, `(arity t)`,
    /// `(arg i t)`, and `(compound? t)`. Participates in `unify`, `deref`,
    /// `occurs_check`, `copy_term`, and `ground?` exactly like `Cons` / `Vector`.
    ///
    /// Two compound terms unify iff they have the same functor symbol, the same
    /// arity, and their arguments unify pairwise.  Structural, not nominal —
    /// there is no type hierarchy.
    ///
    /// Using a dedicated heap kind (rather than encoding as `(functor a1 …)`
    /// inside a `Cons` spine) gives us O(1) arity, O(1) functor access, and a
    /// place to hang future per-term metadata (hash cache, dirty bit, etc.)
    /// without changing surface syntax.
    struct CompoundTerm {
        LispVal              functor;   ///< must be Tag::Symbol
        std::vector<LispVal> args;
    };

}

