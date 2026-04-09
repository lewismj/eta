#pragma once

#include <torch/torch.h>

namespace eta::torch_bindings {

    /// Heap-allocated wrapper around torch::Tensor.
    /// Stored as ObjectKind::Tensor in the Eta heap.
    /// The destructor runs automatically when the GC deallocates the entry,
    /// which decrements libtorch's intrusive_ptr reference count.
    struct TensorPtr {
        ::torch::Tensor tensor;
    };

} // namespace eta::torch_bindings

