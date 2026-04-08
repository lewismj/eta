#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "eta/semantics/semantic_analyzer.h"

namespace eta::semantics {

/**
 * @brief Abstract base for IR-level optimization passes.
 *
 * Passes operate on the Core IR graph (ModuleSemantics) *before* bytecode
 * emission.  This is the right abstraction level for transformations such
 * as constant folding, dead-code elimination, inlining, and copy
 * propagation — all of which benefit from high-level type and scope
 * information that is lost once the IR is lowered to bytecode.
 *
 * Implementations should use the IRVisitor CRTP base (ir_visitor.h) for
 * tree walks and create replacement nodes via ModuleSemantics::emplace().
 * Old nodes are left in the arena and freed when the arena destructs.
 */
class OptimizationPass {
public:
    virtual ~OptimizationPass() = default;

    /// Human-readable name of the pass (for diagnostics / --dump-passes).
    [[nodiscard]] virtual std::string_view name() const noexcept = 0;

    /// Run the pass over a single module's IR.  May mutate the
    /// toplevel_inits list and any nodes reachable from it.
    virtual void run(ModuleSemantics& mod) = 0;
};

} // namespace eta::semantics

