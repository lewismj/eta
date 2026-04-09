#pragma once

#include <cstdint>
#include <cmath>
#include <vector>

namespace eta::runtime::types {

    /// Thread-local active tape for tape-based AD.
    /// Set by tape-start!/tape-stop! builtins. Read by VM arithmetic and
    /// tape-aware transcendentals (sin, cos, exp, log, sqrt).
    /// Value is a NaN-boxed HeapObject LispVal pointing to a Tape, or 0 = inactive.
    inline thread_local std::uint64_t g_active_tape = 0;

    /// Operation recorded on the AD tape (Wengert list).
    enum class TapeOp : std::uint8_t {
        Const,   ///< Literal constant (no operands)
        Var,     ///< Independent variable (no operands)
        Add,
        Sub,
        Mul,
        Div,
        Exp,
        Log,
        Sqrt,
        Sin,
        Cos,
    };

    /// One node in the Wengert list.  ~32 bytes per entry.
    struct TapeEntry {
        TapeOp   op {};
        uint32_t left {};       ///< Index of left operand in tape (or self for unary/const/var)
        uint32_t right {};      ///< Index of right operand (unused for unary/const/var)
        double   primal {};     ///< Forward-pass result value
        double   adjoint {};    ///< Accumulated adjoint (filled during backward)
    };

    /// The tape: a flat vector of TapeEntry nodes.
    /// Forward pass appends entries; backward pass sweeps in reverse
    /// accumulating adjoints via the chain rule.
    struct Tape {
        std::vector<TapeEntry> entries;

        /// Append an entry and return its index.
        uint32_t push(TapeEntry e) {
            auto idx = static_cast<uint32_t>(entries.size());
            entries.push_back(e);
            return idx;
        }

        /// Create an independent variable node.
        uint32_t push_var(double value) {
            return push({TapeOp::Var, 0, 0, value, 0.0});
        }

        /// Create a constant node (not differentiated).
        uint32_t push_const(double value) {
            return push({TapeOp::Const, 0, 0, value, 0.0});
        }

        /// Reverse-mode sweep: propagate adjoint = 1.0 from output_idx back to inputs.
        void backward(uint32_t output_idx) {
            if (output_idx >= entries.size()) return;

            // Zero all adjoints first
            for (auto& e : entries) e.adjoint = 0.0;

            // Seed the output
            entries[output_idx].adjoint = 1.0;

            // Reverse sweep from output_idx down to 0
            for (int64_t i = static_cast<int64_t>(output_idx); i >= 0; --i) {
                auto& e = entries[static_cast<std::size_t>(i)];
                double adj = e.adjoint;
                if (adj == 0.0) continue;

                switch (e.op) {
                    case TapeOp::Const:
                    case TapeOp::Var:
                        // Leaf nodes — nothing to propagate
                        break;

                    case TapeOp::Add:
                        // z = a + b;  dz/da = 1, dz/db = 1
                        entries[e.left].adjoint  += adj;
                        entries[e.right].adjoint += adj;
                        break;

                    case TapeOp::Sub:
                        // z = a - b;  dz/da = 1, dz/db = -1
                        entries[e.left].adjoint  += adj;
                        entries[e.right].adjoint -= adj;
                        break;

                    case TapeOp::Mul:
                        // z = a * b;  dz/da = b, dz/db = a
                        entries[e.left].adjoint  += adj * entries[e.right].primal;
                        entries[e.right].adjoint += adj * entries[e.left].primal;
                        break;

                    case TapeOp::Div:
                        // z = a / b;  dz/da = 1/b, dz/db = -a/b²
                        entries[e.left].adjoint  += adj / entries[e.right].primal;
                        entries[e.right].adjoint -= adj * entries[e.left].primal
                                                      / (entries[e.right].primal * entries[e.right].primal);
                        break;

                    case TapeOp::Exp:
                        // z = exp(a);  dz/da = exp(a) = z
                        entries[e.left].adjoint += adj * e.primal;
                        break;

                    case TapeOp::Log:
                        // z = log(a);  dz/da = 1/a
                        entries[e.left].adjoint += adj / entries[e.left].primal;
                        break;

                    case TapeOp::Sqrt:
                        // z = sqrt(a);  dz/da = 1/(2*sqrt(a)) = 1/(2*z)
                        entries[e.left].adjoint += adj / (2.0 * e.primal);
                        break;

                    case TapeOp::Sin:
                        // z = sin(a);  dz/da = cos(a)
                        entries[e.left].adjoint += adj * std::cos(entries[e.left].primal);
                        break;

                    case TapeOp::Cos:
                        // z = cos(a);  dz/da = -sin(a)
                        entries[e.left].adjoint -= adj * std::sin(entries[e.left].primal);
                        break;
                }
            }
        }
    };

} // namespace eta::runtime::types

