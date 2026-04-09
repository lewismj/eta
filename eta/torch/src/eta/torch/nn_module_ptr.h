#pragma once

#include <torch/torch.h>
#include <functional>
#include <memory>

namespace eta::torch_bindings {

    /// Heap-allocated wrapper around a torch::nn::Module.
    /// Stored as ObjectKind::NNModule in the Eta heap.
    struct NNModulePtr {
        std::shared_ptr<::torch::nn::Module> module;
        /// Type-erased forward function captured from the concrete module type.
        /// torch::nn::Module does not expose a virtual forward(), so we store
        /// a std::function that calls the concrete impl's forward(Tensor).
        std::function<::torch::Tensor(::torch::Tensor)> forward_fn;
        std::string name;  ///< Human-readable description (e.g. "Linear(4, 2)")
    };

} // namespace eta::torch_bindings

