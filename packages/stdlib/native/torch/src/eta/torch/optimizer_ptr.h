#pragma once

#include <torch/torch.h>
#include <memory>

namespace eta::torch_bindings {

    /**
     * Heap-allocated wrapper around a torch::optim::Optimizer.
     * Stored as ObjectKind::Optimizer in the Eta heap.
     */
    struct OptimizerPtr {
        std::shared_ptr<::torch::optim::Optimizer> optimizer;
        std::string name;  ///< Human-readable description (e.g. "Adam(lr=0.001)")
    };

} ///< namespace eta::torch_bindings

