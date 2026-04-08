#pragma once

#include <eta/runtime/nanbox.h>

namespace eta::runtime::types {
    using namespace eta::runtime::nanbox;

    /// AD dual number: (primal, backpropagator-closure).
    /// Dedicated heap object kind — enables the VM arithmetic opcodes
    /// (Add, Sub, Mul, Div) to transparently lift when either operand
    /// is a Dual, making reverse-on-reverse AD possible without any
    /// library-level dispatch wrappers.
    struct Dual {
        LispVal primal {};      ///< The forward (numeric) value
        LispVal backprop {};    ///< A closure: adjoint → list of (index . contribution)
    };

}

