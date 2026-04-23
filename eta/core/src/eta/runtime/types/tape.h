#pragma once

#include <cstdint>
#include <cmath>
#include <vector>

#include "tape_ref.h"

namespace eta::runtime::types {


    /// Operation recorded on the AD tape (Wengert list).
    enum class TapeOp : std::uint8_t {
        Const,   ///< Literal constant (no operands)
        Var,     ///< Independent variable (no operands)
        Add,
        Sub,
        Mul,
        Div,
        Abs,
        Min,
        Max,
        Exp,
        Log,
        Sqrt,
        Sin,
        Cos,
        Tan,
        Asin,
        Acos,
        Atan,
        Pow,
    };

    /// One node in the Wengert list.  ~32 bytes per entry.
    struct TapeEntry {
        TapeOp   op {};
        uint32_t left {};       ///< Index of left operand in tape (or self for unary/const/var)
        uint32_t right {};      ///< Index of right operand (unused for unary/const/var)
        double   primal {};     ///< Forward-pass result value
        double   adjoint {};    ///< Accumulated adjoint (filled during backward)
    };

    /**
     * The tape: a flat vector of TapeEntry nodes.
     * Forward pass appends entries; backward pass sweeps in reverse
     * accumulating adjoints via the chain rule.
     */
    struct Tape {
        uint32_t tape_id {0};
        uint32_t generation {1};
        std::vector<TapeEntry> entries;

        /// Append an entry and return its index.
        uint32_t push(TapeEntry e) {
            auto idx = static_cast<uint32_t>(entries.size());
            entries.push_back(e);
            return idx;
        }

        /// Drop all entries and invalidate prior TapeRef generations.
        void clear_and_bump_generation() {
            entries.clear();
            generation = tape_ref::next_generation(generation);
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

            /// Zero all adjoints first
            for (auto& e : entries) e.adjoint = 0.0;

            /// Seed the output
            entries[output_idx].adjoint = 1.0;

            /// Reverse sweep from output_idx down to 0
            for (int64_t i = static_cast<int64_t>(output_idx); i >= 0; --i) {
                auto& e = entries[static_cast<std::size_t>(i)];
                double adj = e.adjoint;
                if (adj == 0.0) continue;

                const auto in_range = [&](uint32_t idx) {
                    return idx < entries.size();
                };

                switch (e.op) {
                    case TapeOp::Const:
                    case TapeOp::Var:
                        break;

                    case TapeOp::Add:
                        /// z = a + b;  dz/da = 1, dz/db = 1
                        if (!in_range(e.left) || !in_range(e.right)) break;
                        entries[e.left].adjoint  += adj;
                        entries[e.right].adjoint += adj;
                        break;

                    case TapeOp::Sub:
                        /// z = a - b;  dz/da = 1, dz/db = -1
                        if (!in_range(e.left) || !in_range(e.right)) break;
                        entries[e.left].adjoint  += adj;
                        entries[e.right].adjoint -= adj;
                        break;

                    case TapeOp::Mul:
                        /// z = a * b;  dz/da = b, dz/db = a
                        if (!in_range(e.left) || !in_range(e.right)) break;
                        entries[e.left].adjoint  += adj * entries[e.right].primal;
                        entries[e.right].adjoint += adj * entries[e.left].primal;
                        break;

                    case TapeOp::Div:
                        if (!in_range(e.left) || !in_range(e.right)) break;
                        entries[e.left].adjoint  += adj / entries[e.right].primal;
                        entries[e.right].adjoint -= adj * entries[e.left].primal
                                                      / (entries[e.right].primal * entries[e.right].primal);
                        break;

                    case TapeOp::Abs:
                        /// z = abs(a); dz/da is sign(a), tie handled as zero-subgrad
                        if (!in_range(e.left)) break;
                        if (entries[e.left].primal > 0.0) {
                            entries[e.left].adjoint += adj;
                        } else if (entries[e.left].primal < 0.0) {
                            entries[e.left].adjoint -= adj;
                        }
                        break;

                    case TapeOp::Min:
                        /// z = min(a, b); tie handled as zero-subgrad
                        if (!in_range(e.left) || !in_range(e.right)) break;
                        if (entries[e.left].primal < entries[e.right].primal) {
                            entries[e.left].adjoint += adj;
                        } else if (entries[e.right].primal < entries[e.left].primal) {
                            entries[e.right].adjoint += adj;
                        }
                        break;

                    case TapeOp::Max:
                        /// z = max(a, b); tie handled as zero-subgrad
                        if (!in_range(e.left) || !in_range(e.right)) break;
                        if (entries[e.left].primal > entries[e.right].primal) {
                            entries[e.left].adjoint += adj;
                        } else if (entries[e.right].primal > entries[e.left].primal) {
                            entries[e.right].adjoint += adj;
                        }
                        break;

                    case TapeOp::Exp:
                        /// z = exp(a);  dz/da = exp(a) = z
                        if (!in_range(e.left)) break;
                        entries[e.left].adjoint += adj * e.primal;
                        break;

                    case TapeOp::Log:
                        /// z = log(a);  dz/da = 1/a
                        if (!in_range(e.left)) break;
                        entries[e.left].adjoint += adj / entries[e.left].primal;
                        break;

                    case TapeOp::Sqrt:
                        /// z = sqrt(a);  dz/da = 1/(2*sqrt(a)) = 1/(2*z)
                        if (!in_range(e.left)) break;
                        entries[e.left].adjoint += adj / (2.0 * e.primal);
                        break;

                    case TapeOp::Sin:
                        /// z = sin(a);  dz/da = cos(a)
                        if (!in_range(e.left)) break;
                        entries[e.left].adjoint += adj * std::cos(entries[e.left].primal);
                        break;

                    case TapeOp::Cos:
                        /// z = cos(a);  dz/da = -sin(a)
                        if (!in_range(e.left)) break;
                        entries[e.left].adjoint -= adj * std::sin(entries[e.left].primal);
                        break;

                    case TapeOp::Tan:
                        /// z = tan(a); dz/da = 1 / cos(a)^2
                        if (!in_range(e.left)) break;
                        {
                            const double c = std::cos(entries[e.left].primal);
                            entries[e.left].adjoint += adj / (c * c);
                        }
                        break;

                    case TapeOp::Asin:
                        /// z = asin(a); dz/da = 1 / sqrt(1 - a^2)
                        if (!in_range(e.left)) break;
                        {
                            const double x = entries[e.left].primal;
                            entries[e.left].adjoint += adj / std::sqrt(1.0 - x * x);
                        }
                        break;

                    case TapeOp::Acos:
                        /// z = acos(a); dz/da = -1 / sqrt(1 - a^2)
                        if (!in_range(e.left)) break;
                        {
                            const double x = entries[e.left].primal;
                            entries[e.left].adjoint -= adj / std::sqrt(1.0 - x * x);
                        }
                        break;

                    case TapeOp::Atan:
                        /// z = atan(a); dz/da = 1 / (1 + a^2)
                        if (!in_range(e.left)) break;
                        {
                            const double x = entries[e.left].primal;
                            entries[e.left].adjoint += adj / (1.0 + x * x);
                        }
                        break;

                    case TapeOp::Pow:
                        /// z = pow(a, b)
                        if (!in_range(e.left) || !in_range(e.right)) break;
                        {
                            const double a = entries[e.left].primal;
                            const double b = entries[e.right].primal;
                            double d_base = 0.0;
                            double d_exp = 0.0;

                            if (a > 0.0) {
                                d_base = b * std::pow(a, b - 1.0);
                                d_exp = e.primal * std::log(a);
                            } else if (a == 0.0) {
                                if (b == 1.0) {
                                    d_base = 1.0;
                                } else if (b > 1.0) {
                                    d_base = 0.0;
                                }
                            } else if (std::isfinite(b) && std::floor(b) == b) {
                                /// Negative base with integer exponent: base derivative is defined.
                                d_base = b * std::pow(a, b - 1.0);
                            }

                            entries[e.left].adjoint += adj * d_base;
                            entries[e.right].adjoint += adj * d_exp;
                        }
                        break;
                }
            }
        }
    };

} ///< namespace eta::runtime::types

