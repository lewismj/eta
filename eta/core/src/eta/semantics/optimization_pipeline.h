#pragma once

#include <memory>
#include <string>
#include <vector>

#include "eta/semantics/optimization_pass.h"

namespace eta::semantics {

/**
 * @brief Ordered pipeline of IR optimization passes.
 *
 * Usage:
 *   OptimizationPipeline pipeline;
 *   pipeline.add_pass(std::make_unique<MyConstantFolder>());
 *   pipeline.run(module_semantics);
 *
 * Passes execute in registration order over each module.
 */
class OptimizationPipeline {
public:
    OptimizationPipeline() = default;

    /// Append a pass to the end of the pipeline.
    void add_pass(std::unique_ptr<OptimizationPass> pass) {
        passes_.push_back(std::move(pass));
    }

    /// Run every registered pass, in order, over a single module.
    void run(ModuleSemantics& mod) const {
        for (const auto& pass : passes_) {
            pass->run(mod);
        }
    }

    /// Run every registered pass over a list of modules.
    void run_all(std::vector<ModuleSemantics>& modules) const {
        for (auto& mod : modules) {
            run(mod);
        }
    }

    /// Number of registered passes.
    [[nodiscard]] std::size_t size() const noexcept { return passes_.size(); }

    /// True when no passes are registered.
    [[nodiscard]] bool empty() const noexcept { return passes_.empty(); }

    /// Names of all registered passes (useful for --dump-passes).
    [[nodiscard]] std::vector<std::string> pass_names() const {
        std::vector<std::string> names;
        names.reserve(passes_.size());
        for (const auto& p : passes_) names.emplace_back(p->name());
        return names;
    }

private:
    std::vector<std::unique_ptr<OptimizationPass>> passes_;
};

} // namespace eta::semantics

