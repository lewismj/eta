#pragma once

/**
 * @file torch_primitives.h
 * @brief Register libtorch tensor/autograd/nn/optim primitives into an Eta BuiltinEnvironment.
 *
 * All primitives capture Heap& by reference for allocation.
 */

#include <cmath>
#include <sstream>
#include <string>
#include <vector>

#include <torch/torch.h>

#include <eta/runtime/builtin_env.h>
#include <eta/runtime/memory/heap.h>
#include <eta/runtime/memory/intern_table.h>
#include <eta/runtime/nanbox.h>
#include <eta/runtime/error.h>
#include <eta/runtime/numeric_value.h>
#include <eta/runtime/vm/vm.h>

#include "tensor_ptr.h"
#include "nn_module_ptr.h"
#include "optimizer_ptr.h"
#include "torch_factory.h"

namespace eta::torch_bindings {

using namespace eta::runtime;
using namespace eta::runtime::nanbox;
using namespace eta::runtime::memory::heap;
using namespace eta::runtime::memory::intern;
using namespace eta::runtime::error;
using Args = const std::vector<LispVal>&;

/// Helpers

inline TensorPtr* get_tensor(Heap& heap, LispVal v) {
    if (!ops::is_boxed(v) || ops::tag(v) != Tag::HeapObject) return nullptr;
    return heap.try_get_as<ObjectKind::Tensor, TensorPtr>(ops::payload(v));
}

inline NNModulePtr* get_nn_module(Heap& heap, LispVal v) {
    if (!ops::is_boxed(v) || ops::tag(v) != Tag::HeapObject) return nullptr;
    return heap.try_get_as<ObjectKind::NNModule, NNModulePtr>(ops::payload(v));
}

inline OptimizerPtr* get_optimizer(Heap& heap, LispVal v) {
    if (!ops::is_boxed(v) || ops::tag(v) != Tag::HeapObject) return nullptr;
    return heap.try_get_as<ObjectKind::Optimizer, OptimizerPtr>(ops::payload(v));
}

/// Extract a double from a LispVal (fixnum or flonum).
inline std::optional<double> to_double(LispVal v, Heap& heap) {
    auto n = classify_numeric(v, heap);
    if (n.is_fixnum()) return static_cast<double>(n.int_val);
    if (n.is_flonum()) return n.float_val;
    return std::nullopt;
}

/// Extract an int64 from a LispVal fixnum.
inline std::optional<int64_t> to_int64(LispVal v, Heap& heap) {
    auto n = classify_numeric(v, heap);
    if (n.is_fixnum()) return n.int_val;
    if (n.is_flonum() && n.float_val == std::floor(n.float_val))
        return static_cast<int64_t>(n.float_val);
    return std::nullopt;
}

/// Build a shape vector from an Eta list of fixnums.
inline std::vector<int64_t> list_to_shape(LispVal v, Heap& heap) {
    std::vector<int64_t> shape;
    while (v != Nil) {
        if (!ops::is_boxed(v) || ops::tag(v) != Tag::HeapObject) break;
        auto* cons = heap.try_get_as<ObjectKind::Cons, types::Cons>(ops::payload(v));
        if (!cons) break;
        auto dim = to_int64(cons->car, heap);
        if (dim) shape.push_back(*dim);
        v = cons->cdr;
    }
    return shape;
}

/// Convert a flat Eta list of numbers to a 1-D tensor.
inline ::torch::Tensor list_to_tensor(LispVal v, Heap& heap) {
    std::vector<double> vals;
    while (v != Nil) {
        if (!ops::is_boxed(v) || ops::tag(v) != Tag::HeapObject) break;
        auto* cons = heap.try_get_as<ObjectKind::Cons, types::Cons>(ops::payload(v));
        if (!cons) break;
        auto d = to_double(cons->car, heap);
        if (d) vals.push_back(*d);
        v = cons->cdr;
    }
    return ::torch::tensor(vals, ::torch::kFloat64);
}

/// Flat tensor to Eta list.
inline LispVal tensor_to_list(const ::torch::Tensor& t, Heap& heap) {
    auto flat = t.flatten().to(::torch::kFloat64).contiguous();
    auto* data = flat.data_ptr<double>();
    int64_t n = flat.numel();
    LispVal result = Nil;
    /// Build list in reverse
    for (int64_t i = n - 1; i >= 0; --i) {
        auto enc = ops::encode(data[i]);
        if (!enc) continue;
        auto cell = memory::factory::make_cons(heap, *enc, result);
        if (!cell) break;
        result = *cell;
    }
    return result;
}

inline RuntimeError torch_error(const std::string& msg) {
    return RuntimeError{VMError{RuntimeErrorCode::TypeError, msg}};
}

/// Registration

inline void register_torch_primitives(BuiltinEnvironment& env, Heap& heap,
                                       [[maybe_unused]] InternTable& intern_table,
                                       [[maybe_unused]] vm::VM* vm = nullptr) {

    /// Tensor creation

    env.register_builtin("torch/tensor", 1, true, [&heap](Args args) -> std::expected<LispVal, RuntimeError> {
        if (args.empty()) return std::unexpected(torch_error("torch/tensor: requires at least one argument"));
        auto d = to_double(args[0], heap);
        if (d) {
            auto opts = ::torch::TensorOptions().dtype(::torch::kFloat64);
            bool requires_grad = false;
            /// Check for :requires-grad keyword (passed as extra args by convention)
            if (args.size() >= 2) {
                /// If second arg is True, enable requires_grad
                if (args[1] == True) requires_grad = true;
            }
            auto t = ::torch::tensor(*d, opts);
            if (requires_grad) t.set_requires_grad(true);
            return factory::make_tensor(heap, std::move(t));
        }
        auto t = list_to_tensor(args[0], heap);
        if (args.size() >= 2 && args[1] == True) t.set_requires_grad(true);
        return factory::make_tensor(heap, std::move(t));
    });

    env.register_builtin("torch/ones", 1, false, [&heap](Args args) -> std::expected<LispVal, RuntimeError> {
        auto shape = list_to_shape(args[0], heap);
        if (shape.empty()) return std::unexpected(torch_error("torch/ones: invalid shape"));
        return factory::make_tensor(heap, ::torch::ones(shape, ::torch::kFloat64));
    });

    /// (torch/zeros shape-list)
    env.register_builtin("torch/zeros", 1, false, [&heap](Args args) -> std::expected<LispVal, RuntimeError> {
        auto shape = list_to_shape(args[0], heap);
        if (shape.empty()) return std::unexpected(torch_error("torch/zeros: invalid shape"));
        return factory::make_tensor(heap, ::torch::zeros(shape, ::torch::kFloat64));
    });

    /// (torch/randn shape-list)
    env.register_builtin("torch/randn", 1, false, [&heap](Args args) -> std::expected<LispVal, RuntimeError> {
        auto shape = list_to_shape(args[0], heap);
        if (shape.empty()) return std::unexpected(torch_error("torch/randn: invalid shape"));
        return factory::make_tensor(heap, ::torch::randn(shape, ::torch::kFloat64));
    });

    /// (torch/arange start end step)
    env.register_builtin("torch/arange", 3, false, [&heap](Args args) -> std::expected<LispVal, RuntimeError> {
        auto s = to_double(args[0], heap), e = to_double(args[1], heap), st = to_double(args[2], heap);
        if (!s || !e || !st) return std::unexpected(torch_error("torch/arange: numeric args required"));
        return factory::make_tensor(heap, ::torch::arange(*s, *e, *st, ::torch::kFloat64));
    });

    /// (torch/linspace start end steps)
    env.register_builtin("torch/linspace", 3, false, [&heap](Args args) -> std::expected<LispVal, RuntimeError> {
        auto s = to_double(args[0], heap), e = to_double(args[1], heap);
        auto n = to_int64(args[2], heap);
        if (!s || !e || !n) return std::unexpected(torch_error("torch/linspace: numeric args required"));
        return factory::make_tensor(heap, ::torch::linspace(*s, *e, *n, ::torch::kFloat64));
    });

    env.register_builtin("torch/from-list", 1, false, [&heap](Args args) -> std::expected<LispVal, RuntimeError> {
        return factory::make_tensor(heap, list_to_tensor(args[0], heap));
    });

    /// Tensor predicates

    /// (torch/tensor? x)
    env.register_builtin("torch/tensor?", 1, false, [&heap](Args args) -> std::expected<LispVal, RuntimeError> {
        return get_tensor(heap, args[0]) ? True : False;
    });

    /// Arithmetic

    /// (torch/add a b)
    env.register_builtin("torch/add", 2, false, [&heap](Args args) -> std::expected<LispVal, RuntimeError> {
        auto* a = get_tensor(heap, args[0]);
        auto* b = get_tensor(heap, args[1]);
        if (!a || !b) return std::unexpected(torch_error("torch/add: tensor arguments required"));
        return factory::make_tensor(heap, a->tensor + b->tensor);
    });

    /// (torch/sub a b)
    env.register_builtin("torch/sub", 2, false, [&heap](Args args) -> std::expected<LispVal, RuntimeError> {
        auto* a = get_tensor(heap, args[0]);
        auto* b = get_tensor(heap, args[1]);
        if (!a || !b) return std::unexpected(torch_error("torch/sub: tensor arguments required"));
        return factory::make_tensor(heap, a->tensor - b->tensor);
    });

    /// (torch/mul a b)
    env.register_builtin("torch/mul", 2, false, [&heap](Args args) -> std::expected<LispVal, RuntimeError> {
        auto* a = get_tensor(heap, args[0]);
        auto* b = get_tensor(heap, args[1]);
        if (!a || !b) return std::unexpected(torch_error("torch/mul: tensor arguments required"));
        return factory::make_tensor(heap, a->tensor * b->tensor);
    });

    /// (torch/div a b)
    env.register_builtin("torch/div", 2, false, [&heap](Args args) -> std::expected<LispVal, RuntimeError> {
        auto* a = get_tensor(heap, args[0]);
        auto* b = get_tensor(heap, args[1]);
        if (!a || !b) return std::unexpected(torch_error("torch/div: tensor arguments required"));
        return factory::make_tensor(heap, a->tensor / b->tensor);
    });

    /// (torch/matmul a b)
    env.register_builtin("torch/matmul", 2, false, [&heap](Args args) -> std::expected<LispVal, RuntimeError> {
        auto* a = get_tensor(heap, args[0]);
        auto* b = get_tensor(heap, args[1]);
        if (!a || !b) return std::unexpected(torch_error("torch/matmul: tensor arguments required"));
        return factory::make_tensor(heap, ::torch::matmul(a->tensor, b->tensor));
    });

    /// (torch/dot a b)
    env.register_builtin("torch/dot", 2, false, [&heap](Args args) -> std::expected<LispVal, RuntimeError> {
        auto* a = get_tensor(heap, args[0]);
        auto* b = get_tensor(heap, args[1]);
        if (!a || !b) return std::unexpected(torch_error("torch/dot: tensor arguments required"));
        return factory::make_tensor(heap, ::torch::dot(a->tensor.flatten(), b->tensor.flatten()));
    });

    /// Unary ops

    /// (torch/neg t)
    env.register_builtin("torch/neg", 1, false, [&heap](Args args) -> std::expected<LispVal, RuntimeError> {
        auto* t = get_tensor(heap, args[0]);
        if (!t) return std::unexpected(torch_error("torch/neg: tensor required"));
        return factory::make_tensor(heap, -t->tensor);
    });

    /// (torch/abs t)
    env.register_builtin("torch/abs", 1, false, [&heap](Args args) -> std::expected<LispVal, RuntimeError> {
        auto* t = get_tensor(heap, args[0]);
        if (!t) return std::unexpected(torch_error("torch/abs: tensor required"));
        return factory::make_tensor(heap, ::torch::abs(t->tensor));
    });

    /// (torch/exp t)
    env.register_builtin("torch/exp", 1, false, [&heap](Args args) -> std::expected<LispVal, RuntimeError> {
        auto* t = get_tensor(heap, args[0]);
        if (!t) return std::unexpected(torch_error("torch/exp: tensor required"));
        return factory::make_tensor(heap, ::torch::exp(t->tensor));
    });

    /// (torch/log t)
    env.register_builtin("torch/log", 1, false, [&heap](Args args) -> std::expected<LispVal, RuntimeError> {
        auto* t = get_tensor(heap, args[0]);
        if (!t) return std::unexpected(torch_error("torch/log: tensor required"));
        return factory::make_tensor(heap, ::torch::log(t->tensor));
    });

    /// (torch/sqrt t)
    env.register_builtin("torch/sqrt", 1, false, [&heap](Args args) -> std::expected<LispVal, RuntimeError> {
        auto* t = get_tensor(heap, args[0]);
        if (!t) return std::unexpected(torch_error("torch/sqrt: tensor required"));
        return factory::make_tensor(heap, ::torch::sqrt(t->tensor));
    });

    /// (torch/relu t)
    env.register_builtin("torch/relu", 1, false, [&heap](Args args) -> std::expected<LispVal, RuntimeError> {
        auto* t = get_tensor(heap, args[0]);
        if (!t) return std::unexpected(torch_error("torch/relu: tensor required"));
        return factory::make_tensor(heap, ::torch::relu(t->tensor));
    });

    /// (torch/sigmoid t)
    env.register_builtin("torch/sigmoid", 1, false, [&heap](Args args) -> std::expected<LispVal, RuntimeError> {
        auto* t = get_tensor(heap, args[0]);
        if (!t) return std::unexpected(torch_error("torch/sigmoid: tensor required"));
        return factory::make_tensor(heap, ::torch::sigmoid(t->tensor));
    });

    /// (torch/tanh t)
    env.register_builtin("torch/tanh", 1, false, [&heap](Args args) -> std::expected<LispVal, RuntimeError> {
        auto* t = get_tensor(heap, args[0]);
        if (!t) return std::unexpected(torch_error("torch/tanh: tensor required"));
        return factory::make_tensor(heap, ::torch::tanh(t->tensor));
    });

    /// (torch/softmax t dim)
    env.register_builtin("torch/softmax", 2, false, [&heap](Args args) -> std::expected<LispVal, RuntimeError> {
        auto* t = get_tensor(heap, args[0]);
        auto dim = to_int64(args[1], heap);
        if (!t || !dim) return std::unexpected(torch_error("torch/softmax: (tensor, dim) required"));
        return factory::make_tensor(heap, ::torch::softmax(t->tensor, *dim));
    });

    /// Shape ops

    env.register_builtin("torch/shape", 1, false, [&heap](Args args) -> std::expected<LispVal, RuntimeError> {
        auto* t = get_tensor(heap, args[0]);
        if (!t) return std::unexpected(torch_error("torch/shape: tensor required"));
        auto sizes = t->tensor.sizes();
        LispVal result = Nil;
        for (int64_t i = static_cast<int64_t>(sizes.size()) - 1; i >= 0; --i) {
            auto enc = ops::encode(sizes[i]);
            if (!enc) return std::unexpected(torch_error("torch/shape: encode error"));
            auto cell = memory::factory::make_cons(heap, *enc, result);
            if (!cell) return std::unexpected(torch_error("torch/shape: allocation error"));
            result = *cell;
        }
        return result;
    });

    /// (torch/reshape t shape-list)
    env.register_builtin("torch/reshape", 2, false, [&heap](Args args) -> std::expected<LispVal, RuntimeError> {
        auto* t = get_tensor(heap, args[0]);
        if (!t) return std::unexpected(torch_error("torch/reshape: tensor required"));
        auto shape = list_to_shape(args[1], heap);
        return factory::make_tensor(heap, t->tensor.reshape(shape));
    });

    /// (torch/transpose t dim0 dim1)
    env.register_builtin("torch/transpose", 3, false, [&heap](Args args) -> std::expected<LispVal, RuntimeError> {
        auto* t = get_tensor(heap, args[0]);
        auto d0 = to_int64(args[1], heap), d1 = to_int64(args[2], heap);
        if (!t || !d0 || !d1) return std::unexpected(torch_error("torch/transpose: (tensor, dim0, dim1) required"));
        return factory::make_tensor(heap, t->tensor.transpose(*d0, *d1));
    });

    /// (torch/squeeze t)
    env.register_builtin("torch/squeeze", 1, false, [&heap](Args args) -> std::expected<LispVal, RuntimeError> {
        auto* t = get_tensor(heap, args[0]);
        if (!t) return std::unexpected(torch_error("torch/squeeze: tensor required"));
        return factory::make_tensor(heap, t->tensor.squeeze());
    });

    /// (torch/unsqueeze t dim)
    env.register_builtin("torch/unsqueeze", 2, false, [&heap](Args args) -> std::expected<LispVal, RuntimeError> {
        auto* t = get_tensor(heap, args[0]);
        auto d = to_int64(args[1], heap);
        if (!t || !d) return std::unexpected(torch_error("torch/unsqueeze: (tensor, dim) required"));
        return factory::make_tensor(heap, t->tensor.unsqueeze(*d));
    });

    /// (torch/cat tensor-list dim)
    env.register_builtin("torch/cat", 2, false, [&heap](Args args) -> std::expected<LispVal, RuntimeError> {
        std::vector<::torch::Tensor> tensors;
        LispVal lst = args[0];
        while (lst != Nil) {
            if (!ops::is_boxed(lst) || ops::tag(lst) != Tag::HeapObject) break;
            auto* cons = heap.try_get_as<ObjectKind::Cons, types::Cons>(ops::payload(lst));
            if (!cons) break;
            auto* tp = get_tensor(heap, cons->car);
            if (!tp) return std::unexpected(torch_error("torch/cat: list of tensors required"));
            tensors.push_back(tp->tensor);
            lst = cons->cdr;
        }
        auto dim = to_int64(args[1], heap);
        if (!dim) return std::unexpected(torch_error("torch/cat: dim required"));
        return factory::make_tensor(heap, ::torch::cat(tensors, *dim));
    });

    /// Reductions

    /// (torch/sum t)
    env.register_builtin("torch/sum", 1, false, [&heap](Args args) -> std::expected<LispVal, RuntimeError> {
        auto* t = get_tensor(heap, args[0]);
        if (!t) return std::unexpected(torch_error("torch/sum: tensor required"));
        return factory::make_tensor(heap, t->tensor.sum());
    });

    /// (torch/mean t)
    env.register_builtin("torch/mean", 1, false, [&heap](Args args) -> std::expected<LispVal, RuntimeError> {
        auto* t = get_tensor(heap, args[0]);
        if (!t) return std::unexpected(torch_error("torch/mean: tensor required"));
        return factory::make_tensor(heap, t->tensor.mean());
    });

    /// (torch/max t)
    env.register_builtin("torch/max", 1, false, [&heap](Args args) -> std::expected<LispVal, RuntimeError> {
        auto* t = get_tensor(heap, args[0]);
        if (!t) return std::unexpected(torch_error("torch/max: tensor required"));
        return factory::make_tensor(heap, t->tensor.max());
    });

    /// (torch/min t)
    env.register_builtin("torch/min", 1, false, [&heap](Args args) -> std::expected<LispVal, RuntimeError> {
        auto* t = get_tensor(heap, args[0]);
        if (!t) return std::unexpected(torch_error("torch/min: tensor required"));
        return factory::make_tensor(heap, t->tensor.min());
    });

    /// (torch/argmax t)
    env.register_builtin("torch/argmax", 1, false, [&heap](Args args) -> std::expected<LispVal, RuntimeError> {
        auto* t = get_tensor(heap, args[0]);
        if (!t) return std::unexpected(torch_error("torch/argmax: tensor required"));
        return factory::make_tensor(heap, t->tensor.argmax());
    });

    /// (torch/argmin t)
    env.register_builtin("torch/argmin", 1, false, [&heap](Args args) -> std::expected<LispVal, RuntimeError> {
        auto* t = get_tensor(heap, args[0]);
        if (!t) return std::unexpected(torch_error("torch/argmin: tensor required"));
        return factory::make_tensor(heap, t->tensor.argmin());
    });

    /// Conversion

    env.register_builtin("torch/item", 1, false, [&heap](Args args) -> std::expected<LispVal, RuntimeError> {
        auto* t = get_tensor(heap, args[0]);
        if (!t) return std::unexpected(torch_error("torch/item: tensor required"));
        if (t->tensor.numel() != 1)
            return std::unexpected(torch_error("torch/item: scalar tensor required"));
        return ops::encode(t->tensor.item<double>());
    });

    env.register_builtin("torch/to-list", 1, false, [&heap](Args args) -> std::expected<LispVal, RuntimeError> {
        auto* t = get_tensor(heap, args[0]);
        if (!t) return std::unexpected(torch_error("torch/to-list: tensor required"));
        return tensor_to_list(t->tensor, heap);
    });

    env.register_builtin("torch/numel", 1, false, [&heap](Args args) -> std::expected<LispVal, RuntimeError> {
        auto* t = get_tensor(heap, args[0]);
        if (!t) return std::unexpected(torch_error("torch/numel: tensor required"));
        return ops::encode(t->tensor.numel());
    });

    /// Autograd

    env.register_builtin("torch/requires-grad!", 2, false, [&heap](Args args) -> std::expected<LispVal, RuntimeError> {
        auto* t = get_tensor(heap, args[0]);
        if (!t) return std::unexpected(torch_error("torch/requires-grad!: tensor required"));
        t->tensor.set_requires_grad(args[1] != False && args[1] != Nil);
        return args[0];
    });

    env.register_builtin("torch/requires-grad?", 1, false, [&heap](Args args) -> std::expected<LispVal, RuntimeError> {
        auto* t = get_tensor(heap, args[0]);
        if (!t) return std::unexpected(torch_error("torch/requires-grad?: tensor required"));
        return t->tensor.requires_grad() ? True : False;
    });

    env.register_builtin("torch/detach", 1, false, [&heap](Args args) -> std::expected<LispVal, RuntimeError> {
        auto* t = get_tensor(heap, args[0]);
        if (!t) return std::unexpected(torch_error("torch/detach: tensor required"));
        return factory::make_tensor(heap, t->tensor.detach());
    });

    env.register_builtin("torch/backward", 1, false, [&heap](Args args) -> std::expected<LispVal, RuntimeError> {
        auto* t = get_tensor(heap, args[0]);
        if (!t) return std::unexpected(torch_error("torch/backward: tensor required"));
        t->tensor.backward();
        return Nil;
    });

    env.register_builtin("torch/grad", 1, false, [&heap](Args args) -> std::expected<LispVal, RuntimeError> {
        auto* t = get_tensor(heap, args[0]);
        if (!t) return std::unexpected(torch_error("torch/grad: tensor required"));
        auto g = t->tensor.grad();
        if (!g.defined()) return Nil;
        return factory::make_tensor(heap, g);
    });

    env.register_builtin("torch/zero-grad!", 1, false, [&heap](Args args) -> std::expected<LispVal, RuntimeError> {
        auto* t = get_tensor(heap, args[0]);
        if (!t) return std::unexpected(torch_error("torch/zero-grad!: tensor required"));
        if (t->tensor.grad().defined()) t->tensor.grad().zero_();
        return Nil;
    });

    /// NN Layer constructors

    env.register_builtin("nn/linear", 2, false, [&heap](Args args) -> std::expected<LispVal, RuntimeError> {
        auto in_f = to_int64(args[0], heap), out_f = to_int64(args[1], heap);
        if (!in_f || !out_f) return std::unexpected(torch_error("nn/linear: (in, out) required"));
        auto mod = ::torch::nn::Linear(::torch::nn::LinearOptions(*in_f, *out_f));
        /// Force to float64 to match Eta's default
        mod->to(::torch::kFloat64);
        auto name = "Linear(" + std::to_string(*in_f) + ", " + std::to_string(*out_f) + ")";
        return factory::make_nn_module(heap, mod.ptr(), std::move(name));
    });

    env.register_builtin("nn/sequential", 0, true, [&heap](Args args) -> std::expected<LispVal, RuntimeError> {
        auto seq = ::torch::nn::Sequential();
        for (size_t i = 0; i < args.size(); ++i) {
            auto* m = get_nn_module(heap, args[i]);
            if (!m) return std::unexpected(torch_error("nn/sequential: nn-module arguments required"));
            if (!m->forward_fn) return std::unexpected(torch_error("nn/sequential: module has no forward function"));
            /**
             * Wrap each sub-module's type-erased forward in a Functional node
             * because Sequential requires concrete module types with forward().
             */
            auto fn = m->forward_fn;
            seq->push_back("layer" + std::to_string(i),
                ::torch::nn::Functional([fn](::torch::Tensor t) { return fn(std::move(t)); }));
            /**
             * Also register the original module so its learnable parameters
             * (e.g. Linear weight/bias) are visible via parameters().
             * Functional wrappers are stateless and have no parameters.
             */
            if (!m->module->parameters().empty()) {
                seq->register_module("params" + std::to_string(i), m->module);
            }
        }
        seq->to(::torch::kFloat64);
        return factory::make_nn_module(heap, seq.ptr(), "Sequential");
    });

    env.register_builtin("nn/relu-layer", 0, false, [&heap]([[maybe_unused]] Args args) -> std::expected<LispVal, RuntimeError> {
        auto mod = ::torch::nn::Functional(::torch::relu);
        return factory::make_nn_module(heap, mod.ptr(), "ReLU");
    });

    /// (nn/sigmoid-layer)
    env.register_builtin("nn/sigmoid-layer", 0, false, [&heap]([[maybe_unused]] Args args) -> std::expected<LispVal, RuntimeError> {
        auto mod = ::torch::nn::Sigmoid();
        return factory::make_nn_module(heap, mod.ptr(), "Sigmoid");
    });

    /// (nn/dropout rate)
    env.register_builtin("nn/dropout", 1, false, [&heap](Args args) -> std::expected<LispVal, RuntimeError> {
        auto rate = to_double(args[0], heap);
        if (!rate) return std::unexpected(torch_error("nn/dropout: rate required"));
        auto mod = ::torch::nn::Dropout(::torch::nn::DropoutOptions(*rate));
        return factory::make_nn_module(heap, mod.ptr(), "Dropout(" + std::to_string(*rate) + ")");
    });

    /// NN forward / parameters

    env.register_builtin("nn/forward", 2, false, [&heap](Args args) -> std::expected<LispVal, RuntimeError> {
        auto* m = get_nn_module(heap, args[0]);
        auto* t = get_tensor(heap, args[1]);
        if (!m || !t) return std::unexpected(torch_error("nn/forward: (module, tensor) required"));
        if (!m->forward_fn) return std::unexpected(torch_error("nn/forward: module has no forward function"));
        auto output = m->forward_fn(t->tensor);
        return factory::make_tensor(heap, output);
    });

    env.register_builtin("nn/parameters", 1, false, [&heap](Args args) -> std::expected<LispVal, RuntimeError> {
        auto* m = get_nn_module(heap, args[0]);
        if (!m) return std::unexpected(torch_error("nn/parameters: nn-module required"));
        LispVal result = Nil;
        auto params = m->module->parameters();
        for (int64_t i = static_cast<int64_t>(params.size()) - 1; i >= 0; --i) {
            auto tv = factory::make_tensor(heap, params[i]);
            if (!tv) return std::unexpected(torch_error("nn/parameters: allocation failed"));
            auto cell = memory::factory::make_cons(heap, *tv, result);
            if (!cell) return std::unexpected(torch_error("nn/parameters: allocation failed"));
            result = *cell;
        }
        return result;
    });

    env.register_builtin("nn/train!", 1, false, [&heap](Args args) -> std::expected<LispVal, RuntimeError> {
        auto* m = get_nn_module(heap, args[0]);
        if (!m) return std::unexpected(torch_error("nn/train!: nn-module required"));
        m->module->train();
        return args[0];
    });

    env.register_builtin("nn/eval!", 1, false, [&heap](Args args) -> std::expected<LispVal, RuntimeError> {
        auto* m = get_nn_module(heap, args[0]);
        if (!m) return std::unexpected(torch_error("nn/eval!: nn-module required"));
        m->module->eval();
        return args[0];
    });

    env.register_builtin("nn/module?", 1, false, [&heap](Args args) -> std::expected<LispVal, RuntimeError> {
        return get_nn_module(heap, args[0]) ? True : False;
    });

    /// Loss functions

    env.register_builtin("nn/mse-loss", 2, false, [&heap](Args args) -> std::expected<LispVal, RuntimeError> {
        auto* p = get_tensor(heap, args[0]);
        auto* t = get_tensor(heap, args[1]);
        if (!p || !t) return std::unexpected(torch_error("nn/mse-loss: (prediction, target) required"));
        return factory::make_tensor(heap, ::torch::mse_loss(p->tensor, t->tensor));
    });

    env.register_builtin("nn/l1-loss", 2, false, [&heap](Args args) -> std::expected<LispVal, RuntimeError> {
        auto* p = get_tensor(heap, args[0]);
        auto* t = get_tensor(heap, args[1]);
        if (!p || !t) return std::unexpected(torch_error("nn/l1-loss: (prediction, target) required"));
        return factory::make_tensor(heap, ::torch::l1_loss(p->tensor, t->tensor));
    });

    env.register_builtin("nn/cross-entropy-loss", 2, false, [&heap](Args args) -> std::expected<LispVal, RuntimeError> {
        auto* p = get_tensor(heap, args[0]);
        auto* t = get_tensor(heap, args[1]);
        if (!p || !t) return std::unexpected(torch_error("nn/cross-entropy-loss: (input, target) required"));
        return factory::make_tensor(heap, ::torch::cross_entropy_loss(p->tensor, t->tensor));
    });

    /// Optimizers

    env.register_builtin("optim/sgd", 2, false, [&heap](Args args) -> std::expected<LispVal, RuntimeError> {
        auto* m = get_nn_module(heap, args[0]);
        auto lr = to_double(args[1], heap);
        if (!m || !lr) return std::unexpected(torch_error("optim/sgd: (module, lr) required"));
        auto opt = std::make_shared<::torch::optim::SGD>(
            m->module->parameters(), ::torch::optim::SGDOptions(*lr));
        return factory::make_optimizer(heap, std::move(opt), "SGD(lr=" + std::to_string(*lr) + ")");
    });

    env.register_builtin("optim/adam", 2, false, [&heap](Args args) -> std::expected<LispVal, RuntimeError> {
        auto* m = get_nn_module(heap, args[0]);
        auto lr = to_double(args[1], heap);
        if (!m || !lr) return std::unexpected(torch_error("optim/adam: (module, lr) required"));
        auto opt = std::make_shared<::torch::optim::Adam>(
            m->module->parameters(), ::torch::optim::AdamOptions(*lr));
        return factory::make_optimizer(heap, std::move(opt), "Adam(lr=" + std::to_string(*lr) + ")");
    });

    env.register_builtin("optim/step!", 1, false, [&heap](Args args) -> std::expected<LispVal, RuntimeError> {
        auto* o = get_optimizer(heap, args[0]);
        if (!o) return std::unexpected(torch_error("optim/step!: optimizer required"));
        o->optimizer->step();
        return Nil;
    });

    env.register_builtin("optim/zero-grad!", 1, false, [&heap](Args args) -> std::expected<LispVal, RuntimeError> {
        auto* o = get_optimizer(heap, args[0]);
        if (!o) return std::unexpected(torch_error("optim/zero-grad!: optimizer required"));
        o->optimizer->zero_grad();
        return Nil;
    });

    env.register_builtin("optim/optimizer?", 1, false, [&heap](Args args) -> std::expected<LispVal, RuntimeError> {
        return get_optimizer(heap, args[0]) ? True : False;
    });

    /// Serialization

    /// (torch/save t path)
    env.register_builtin("torch/save", 2, false, [&heap, &intern_table](Args args) -> std::expected<LispVal, RuntimeError> {
        auto* t = get_tensor(heap, args[0]);
        if (!t) return std::unexpected(torch_error("torch/save: tensor required"));
        if (!ops::is_boxed(args[1]) || ops::tag(args[1]) != Tag::String)
            return std::unexpected(torch_error("torch/save: path string required"));
        auto sv = intern_table.get_string(ops::payload(args[1]));
        if (!sv) return std::unexpected(torch_error("torch/save: invalid path"));
        ::torch::save(t->tensor, std::string(*sv));
        return Nil;
    });

    env.register_builtin("torch/load", 1, false, [&heap, &intern_table](Args args) -> std::expected<LispVal, RuntimeError> {
        if (!ops::is_boxed(args[0]) || ops::tag(args[0]) != Tag::String)
            return std::unexpected(torch_error("torch/load: path string required"));
        auto sv = intern_table.get_string(ops::payload(args[0]));
        if (!sv) return std::unexpected(torch_error("torch/load: invalid path"));
        ::torch::Tensor t;
        ::torch::load(t, std::string(*sv));
        return factory::make_tensor(heap, std::move(t));
    });

    env.register_builtin("torch/print", 1, false, [&heap](Args args) -> std::expected<LispVal, RuntimeError> {
        auto* t = get_tensor(heap, args[0]);
        if (!t) return std::unexpected(torch_error("torch/print: tensor required"));
        std::cout << t->tensor << std::endl;
        return Nil;
    });

    /// Device management

    env.register_builtin("torch/cuda-available?", 0, false, []([[maybe_unused]] Args args) -> std::expected<LispVal, RuntimeError> {
        return ::torch::cuda::is_available() ? True : False;
    });

    env.register_builtin("torch/cuda-device-count", 0, false, []([[maybe_unused]] Args args) -> std::expected<LispVal, RuntimeError> {
        return ops::encode(static_cast<int64_t>(::torch::cuda::device_count()));
    });

    env.register_builtin("torch/device", 1, false, [&heap, &intern_table](Args args) -> std::expected<LispVal, RuntimeError> {
        auto* t = get_tensor(heap, args[0]);
        if (!t) return std::unexpected(torch_error("torch/device: tensor required"));
        std::ostringstream oss;
        oss << t->tensor.device();
        auto id = intern_table.intern(oss.str());
        if (!id) return std::unexpected(torch_error("torch/device: intern failed"));
        return ops::box(Tag::String, *id);
    });

    ///   device-string is e.g. "cpu", "cuda", "cuda:0", "cuda:1"
    env.register_builtin("torch/to-device", 2, false, [&heap, &intern_table](Args args) -> std::expected<LispVal, RuntimeError> {
        auto* t = get_tensor(heap, args[0]);
        if (!t) return std::unexpected(torch_error("torch/to-device: tensor required as first arg"));
        if (!ops::is_boxed(args[1]) || ops::tag(args[1]) != Tag::String)
            return std::unexpected(torch_error("torch/to-device: device string required as second arg"));
        auto sv = intern_table.get_string(ops::payload(args[1]));
        if (!sv) return std::unexpected(torch_error("torch/to-device: invalid device string"));
        try {
            auto device = ::torch::Device(std::string(*sv));
            return factory::make_tensor(heap, t->tensor.to(device));
        } catch (const c10::Error& e) {
            return std::unexpected(torch_error(std::string("torch/to-device: ") + e.what()));
        }
    });

    env.register_builtin("nn/to-device", 2, false, [&heap, &intern_table](Args args) -> std::expected<LispVal, RuntimeError> {
        auto* m = get_nn_module(heap, args[0]);
        if (!m) return std::unexpected(torch_error("nn/to-device: nn-module required as first arg"));
        if (!ops::is_boxed(args[1]) || ops::tag(args[1]) != Tag::String)
            return std::unexpected(torch_error("nn/to-device: device string required as second arg"));
        auto sv = intern_table.get_string(ops::payload(args[1]));
        if (!sv) return std::unexpected(torch_error("nn/to-device: invalid device string"));
        try {
            auto device = ::torch::Device(std::string(*sv));
            m->module->to(device);
            return args[0];
        } catch (const c10::Error& e) {
            return std::unexpected(torch_error(std::string("nn/to-device: ") + e.what()));
        }
    });
}


} ///< namespace eta::torch_bindings

