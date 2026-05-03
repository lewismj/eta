#pragma once

#include <atomic>

#include <eta/runtime/nanbox.h>

namespace eta::runtime::types {
    using namespace eta::runtime::nanbox;

    /**
     * @brief Single-cell mutable reference with atomic update semantics.
     *
     * The cell stores a raw LispVal and is traced by GC as a strong edge.
     */
    struct Atom {
        std::atomic<LispVal> cell{nanbox::Nil};

        explicit Atom(const LispVal initial) : cell(initial) {}
    };
}

