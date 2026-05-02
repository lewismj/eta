#pragma once

#include <expected>
#include <eta/arch.h>
#include <eta/runtime/nanbox.h>
#include <eta/runtime/error.h>
#include <eta/runtime/memory/heap.h>

#include "tensor_ptr.h"
#include "nn_module_ptr.h"
#include "optimizer_ptr.h"

namespace eta::torch_bindings::factory {

    using namespace eta::runtime::nanbox;
    using namespace eta::runtime::error;
    using namespace eta::runtime::memory::heap;

    inline std::expected<LispVal, RuntimeError>
    make_tensor(Heap& heap, ::torch::Tensor tensor) {
        auto id = heap.allocate<TensorPtr, ObjectKind::Tensor>(TensorPtr{std::move(tensor)});
        if (id.has_value()) return ops::box(Tag::HeapObject, *id);
        return id;
    }

    /**
     * T must be a concrete Module impl (e.g. LinearImpl, SequentialImpl).
     * Callers typically pass `holder.ptr()` which yields `shared_ptr<ConcreteImpl>`.
     */
    template <typename T>
    inline std::expected<LispVal, RuntimeError>
    make_nn_module(Heap& heap, std::shared_ptr<T> mod, std::string name = "") {
        /**
         * Capture the concrete type's forward(Tensor)->Tensor before we
         * erase the type to shared_ptr<Module>.
         */
        auto fwd = [mod](::torch::Tensor input) -> ::torch::Tensor {
            return mod->forward(std::move(input));
        };
        auto id = heap.allocate<NNModulePtr, ObjectKind::NNModule>(
            NNModulePtr{std::move(mod), std::move(fwd), std::move(name)});
        if (id.has_value()) return ops::box(Tag::HeapObject, *id);
        return id;
    }

    inline std::expected<LispVal, RuntimeError>
    make_optimizer(Heap& heap, std::shared_ptr<::torch::optim::Optimizer> opt, std::string name = "") {
        auto id = heap.allocate<OptimizerPtr, ObjectKind::Optimizer>(
            OptimizerPtr{std::move(opt), std::move(name)});
        if (id.has_value()) return ops::box(Tag::HeapObject, *id);
        return id;
    }

} ///< namespace eta::torch_bindings::factory

