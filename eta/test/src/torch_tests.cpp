#include <boost/test/unit_test.hpp>

#include <eta/runtime/memory/heap.h>
#include <eta/runtime/memory/mark_sweep_gc.h>
#include <eta/runtime/factory.h>
#include <eta/runtime/nanbox.h>

#include <eta/torch/tensor_ptr.h>
#include <eta/torch/nn_module_ptr.h>
#include <eta/torch/optimizer_ptr.h>
#include <eta/torch/torch_factory.h>
#include <eta/torch/torch_primitives.h>

using namespace eta::runtime::memory;
using namespace eta::runtime::memory::heap;
using namespace eta::runtime::memory::gc;
using namespace eta::runtime::nanbox;
using namespace eta::runtime::nanbox::ops;
using namespace eta::runtime::memory::factory;
using namespace eta::torch_bindings;
namespace tf = eta::torch_bindings::factory;

namespace {
    template <typename T, typename E>
    T expect_ok(const std::expected<T,E>& r) {
        BOOST_REQUIRE(r.has_value());
        return *r;
    }
}

BOOST_AUTO_TEST_SUITE(torch_tests)

// ═══════════════════════════════════════════════════════════════════════════
// TensorPtr heap lifecycle
// ═══════════════════════════════════════════════════════════════════════════

BOOST_AUTO_TEST_CASE(tensor_allocate_and_retrieve) {
    Heap heap(1ull << 22);
    auto t = torch::ones({3, 4}, torch::kFloat64);
    auto val = expect_ok(tf::make_tensor(heap, t));

    BOOST_TEST(ops::is_boxed(val));
    BOOST_TEST(ops::tag(val) == Tag::HeapObject);

    auto* tp = heap.try_get_as<ObjectKind::Tensor, TensorPtr>(ops::payload(val));
    BOOST_REQUIRE(tp != nullptr);
    BOOST_TEST(tp->tensor.sizes() == std::vector<int64_t>({3, 4}));
    BOOST_TEST(tp->tensor.dtype() == torch::kFloat64);
}

BOOST_AUTO_TEST_CASE(tensor_gc_collects_unreferenced) {
    Heap heap(1ull << 22);
    MarkSweepGC gc;

    // Allocate tensors but don't root them
    for (int i = 0; i < 5; ++i) {
        expect_ok(tf::make_tensor(heap, torch::randn({10})));
    }

    std::vector<LispVal> roots;
    GCStats stats{};
    gc.collect(heap, roots.begin(), roots.end(), &stats);
    BOOST_TEST(stats.objects_freed == 5);
}

BOOST_AUTO_TEST_CASE(tensor_gc_retains_rooted) {
    Heap heap(1ull << 22);
    MarkSweepGC gc;

    auto val = expect_ok(tf::make_tensor(heap, torch::ones({2, 2})));
    // Also allocate an unreferenced tensor
    expect_ok(tf::make_tensor(heap, torch::zeros({3})));

    std::vector<LispVal> roots = {val};
    GCStats stats{};
    gc.collect(heap, roots.begin(), roots.end(), &stats);
    BOOST_TEST(stats.objects_freed == 1);  // only the unreferenced one

    // Rooted tensor is still valid
    auto* tp = heap.try_get_as<ObjectKind::Tensor, TensorPtr>(ops::payload(val));
    BOOST_REQUIRE(tp != nullptr);
    BOOST_TEST(tp->tensor.numel() == 4);
}

BOOST_AUTO_TEST_CASE(tensor_deallocate) {
    Heap heap(1ull << 22);
    auto val = expect_ok(tf::make_tensor(heap, torch::ones({5})));
    auto id = ops::payload(val);
    auto r = heap.deallocate(id);
    BOOST_REQUIRE(r.has_value());

    // Should be gone
    HeapEntry entry;
    BOOST_TEST(!heap.try_get(id, entry));
}

// ═══════════════════════════════════════════════════════════════════════════
// Tensor creation primitives
// ═══════════════════════════════════════════════════════════════════════════

BOOST_AUTO_TEST_CASE(torch_ones_creates_correct_shape) {
    Heap heap(1ull << 22);
    InternTable intern;

    // Build shape list (3 4) as Eta cons-list
    auto four = expect_ok(ops::encode(int64_t(4)));
    auto three = expect_ok(ops::encode(int64_t(3)));
    auto tail = expect_ok(make_cons(heap, four, Nil));
    auto shape_list = expect_ok(make_cons(heap, three, tail));

    auto shape_vec = list_to_shape(shape_list, heap);
    BOOST_TEST(shape_vec.size() == 2);
    BOOST_TEST(shape_vec[0] == 3);
    BOOST_TEST(shape_vec[1] == 4);

    auto t = torch::ones(shape_vec, torch::kFloat64);
    BOOST_TEST(t.sizes() == std::vector<int64_t>({3, 4}));
    BOOST_TEST(t.sum().item<double>() == 12.0);
}

BOOST_AUTO_TEST_CASE(torch_from_list_roundtrip) {
    Heap heap(1ull << 22);

    // Build list (1.0 2.0 3.0)
    auto v3 = expect_ok(ops::encode(3.0));
    auto v2 = expect_ok(ops::encode(2.0));
    auto v1 = expect_ok(ops::encode(1.0));
    auto l3 = expect_ok(make_cons(heap, v3, Nil));
    auto l2 = expect_ok(make_cons(heap, v2, l3));
    auto l1 = expect_ok(make_cons(heap, v1, l2));

    auto t = list_to_tensor(l1, heap);
    BOOST_TEST(t.numel() == 3);
    BOOST_TEST(t[0].item<double>() == 1.0);
    BOOST_TEST(t[1].item<double>() == 2.0);
    BOOST_TEST(t[2].item<double>() == 3.0);

    // Convert back
    auto back = tensor_to_list(t, heap);
    BOOST_TEST(back != Nil);
    auto* c1 = heap.try_get_as<ObjectKind::Cons, types::Cons>(ops::payload(back));
    BOOST_REQUIRE(c1);
    BOOST_TEST(std::bit_cast<double>(c1->car) == 1.0);
}

// ═══════════════════════════════════════════════════════════════════════════
// Tensor arithmetic
// ═══════════════════════════════════════════════════════════════════════════

BOOST_AUTO_TEST_CASE(tensor_add) {
    auto a = torch::ones({3}, torch::kFloat64);
    auto b = torch::ones({3}, torch::kFloat64) * 2.0;
    auto c = a + b;
    BOOST_TEST(c[0].item<double>() == 3.0);
    BOOST_TEST(c[1].item<double>() == 3.0);
    BOOST_TEST(c[2].item<double>() == 3.0);
}

BOOST_AUTO_TEST_CASE(tensor_matmul) {
    auto a = torch::ones({2, 3}, torch::kFloat64);
    auto b = torch::ones({3, 4}, torch::kFloat64);
    auto c = torch::matmul(a, b);
    BOOST_TEST(c.sizes() == std::vector<int64_t>({2, 4}));
    BOOST_TEST(c[0][0].item<double>() == 3.0);
}

BOOST_AUTO_TEST_CASE(tensor_unary_ops) {
    auto t = torch::tensor({1.0, 4.0, 9.0}, torch::kFloat64);
    auto s = torch::sqrt(t);
    BOOST_TEST(std::abs(s[0].item<double>() - 1.0) < 1e-10);
    BOOST_TEST(std::abs(s[1].item<double>() - 2.0) < 1e-10);
    BOOST_TEST(std::abs(s[2].item<double>() - 3.0) < 1e-10);

    auto e = torch::exp(torch::zeros({2}, torch::kFloat64));
    BOOST_TEST(std::abs(e[0].item<double>() - 1.0) < 1e-10);
}

// ═══════════════════════════════════════════════════════════════════════════
// Autograd
// ═══════════════════════════════════════════════════════════════════════════

BOOST_AUTO_TEST_CASE(autograd_basic_grad) {
    auto x = torch::tensor(2.0, torch::TensorOptions().dtype(torch::kFloat64).requires_grad(true));
    auto y = x * x;  // y = x^2
    y.backward();
    BOOST_TEST(std::abs(x.grad().item<double>() - 4.0) < 1e-10);  // dy/dx = 2x = 4
}

BOOST_AUTO_TEST_CASE(autograd_chain_rule) {
    auto x = torch::tensor(3.0, torch::TensorOptions().dtype(torch::kFloat64).requires_grad(true));
    auto y = torch::sin(x);  // y = sin(x)
    y.backward();
    auto expected = std::cos(3.0);
    BOOST_TEST(std::abs(x.grad().item<double>() - expected) < 1e-10);
}

BOOST_AUTO_TEST_CASE(autograd_requires_grad_heap) {
    Heap heap(1ull << 22);
    auto t = torch::tensor(5.0, torch::TensorOptions().dtype(torch::kFloat64).requires_grad(true));
    auto val = expect_ok(tf::make_tensor(heap, t));
    auto* tp = heap.try_get_as<ObjectKind::Tensor, TensorPtr>(ops::payload(val));
    BOOST_REQUIRE(tp);
    BOOST_TEST(tp->tensor.requires_grad());
}

// ═══════════════════════════════════════════════════════════════════════════
// NN Modules
// ═══════════════════════════════════════════════════════════════════════════

BOOST_AUTO_TEST_CASE(nn_linear_creates_module) {
    Heap heap(1ull << 22);
    auto mod = torch::nn::Linear(torch::nn::LinearOptions(4, 2));
    mod->to(torch::kFloat64);
    auto val = expect_ok(tf::make_nn_module(heap, mod.ptr(), "Linear(4, 2)"));

    auto* mp = heap.try_get_as<ObjectKind::NNModule, NNModulePtr>(ops::payload(val));
    BOOST_REQUIRE(mp);
    BOOST_TEST(mp->name == "Linear(4, 2)");
    BOOST_TEST(mp->module->parameters().size() == 2);  // weight + bias
}

BOOST_AUTO_TEST_CASE(nn_linear_forward) {
    auto mod = torch::nn::Linear(torch::nn::LinearOptions(3, 2));
    mod->to(torch::kFloat64);
    auto input = torch::randn({1, 3}, torch::kFloat64);
    auto output = mod->forward(input);
    BOOST_TEST(output.sizes() == std::vector<int64_t>({1, 2}));
}

BOOST_AUTO_TEST_CASE(nn_sequential_forward) {
    auto seq = torch::nn::Sequential(
        torch::nn::Linear(torch::nn::LinearOptions(4, 8)),
        torch::nn::Functional(torch::relu),
        torch::nn::Linear(torch::nn::LinearOptions(8, 2))
    );
    seq->to(torch::kFloat64);
    auto input = torch::randn({1, 4}, torch::kFloat64);
    auto output = seq->forward(input);
    BOOST_TEST(output.sizes() == std::vector<int64_t>({1, 2}));
}

BOOST_AUTO_TEST_CASE(nn_module_gc_collects) {
    Heap heap(1ull << 22);
    MarkSweepGC gc;

    auto mod = torch::nn::Linear(torch::nn::LinearOptions(2, 2));
    expect_ok(tf::make_nn_module(heap, mod.ptr(), "Linear"));

    std::vector<LispVal> roots;
    GCStats stats{};
    gc.collect(heap, roots.begin(), roots.end(), &stats);
    BOOST_TEST(stats.objects_freed == 1);
}

// ═══════════════════════════════════════════════════════════════════════════
// Optimizers
// ═══════════════════════════════════════════════════════════════════════════

BOOST_AUTO_TEST_CASE(optimizer_sgd_step) {
    auto model = torch::nn::Linear(torch::nn::LinearOptions(2, 1));
    model->to(torch::kFloat64);
    auto opt = torch::optim::SGD(model->parameters(), torch::optim::SGDOptions(0.01));

    auto input = torch::randn({1, 2}, torch::kFloat64);
    auto target = torch::tensor({1.0}, torch::kFloat64);
    auto pred = model->forward(input);
    auto loss = torch::mse_loss(pred, target);
    loss.backward();

    // Capture weight before step
    auto w_before = model->parameters()[0].clone();
    opt.step();
    auto w_after = model->parameters()[0];

    // Weights should have changed
    BOOST_TEST(!torch::allclose(w_before, w_after));
}

BOOST_AUTO_TEST_CASE(optimizer_adam_heap) {
    Heap heap(1ull << 22);

    auto mod = torch::nn::Linear(torch::nn::LinearOptions(3, 1));
    mod->to(torch::kFloat64);
    auto opt = std::make_shared<torch::optim::Adam>(
        mod->parameters(), torch::optim::AdamOptions(0.001));

    auto val = expect_ok(tf::make_optimizer(heap, opt, "Adam"));
    auto* op = heap.try_get_as<ObjectKind::Optimizer, OptimizerPtr>(ops::payload(val));
    BOOST_REQUIRE(op);
    BOOST_TEST(op->name == "Adam");
}

// ═══════════════════════════════════════════════════════════════════════════
// Loss functions
// ═══════════════════════════════════════════════════════════════════════════

BOOST_AUTO_TEST_CASE(mse_loss_correct) {
    auto pred = torch::tensor({1.0, 2.0, 3.0}, torch::kFloat64);
    auto target = torch::tensor({1.0, 2.0, 3.0}, torch::kFloat64);
    auto loss = torch::mse_loss(pred, target);
    BOOST_TEST(std::abs(loss.item<double>()) < 1e-10);
}

BOOST_AUTO_TEST_CASE(mse_loss_nonzero) {
    auto pred = torch::tensor({1.0, 2.0, 3.0}, torch::kFloat64);
    auto target = torch::tensor({2.0, 2.0, 2.0}, torch::kFloat64);
    auto loss = torch::mse_loss(pred, target);
    // MSE = ((1-2)^2 + (2-2)^2 + (3-2)^2) / 3 = 2/3
    BOOST_TEST(std::abs(loss.item<double>() - 2.0/3.0) < 1e-10);
}

// ═══════════════════════════════════════════════════════════════════════════
// Primitive registration (smoke test)
// ═══════════════════════════════════════════════════════════════════════════

BOOST_AUTO_TEST_CASE(register_torch_primitives_smoke) {
    Heap heap(1ull << 22);
    InternTable intern;
    BuiltinEnvironment env;

    // Should not throw
    register_torch_primitives(env, heap, intern, nullptr);

    // Check some names are registered
    auto idx = env.lookup("torch/ones");
    BOOST_TEST(idx.has_value());
    idx = env.lookup("nn/linear");
    BOOST_TEST(idx.has_value());
    idx = env.lookup("optim/adam");
    BOOST_TEST(idx.has_value());
}

BOOST_AUTO_TEST_CASE(register_torch_builtin_names_smoke) {
    BuiltinEnvironment env;
    register_torch_builtin_names(env);

    auto idx = env.lookup("torch/tensor");
    BOOST_TEST(idx.has_value());
    idx = env.lookup("nn/forward");
    BOOST_TEST(idx.has_value());
    idx = env.lookup("optim/step!");
    BOOST_TEST(idx.has_value());
}

// ═══════════════════════════════════════════════════════════════════════════
// Device management
// ═══════════════════════════════════════════════════════════════════════════

BOOST_AUTO_TEST_CASE(cuda_available_returns_bool) {
    Heap heap(1ull << 22);
    InternTable intern;
    BuiltinEnvironment env;
    register_torch_primitives(env, heap, intern, nullptr);

    auto idx = env.lookup("torch/cuda-available?");
    BOOST_REQUIRE(idx.has_value());
    const auto& spec = env.specs()[*idx];
    BOOST_TEST(spec.arity == 0);
    BOOST_TEST(!spec.has_rest);

    // Call the primitive
    std::vector<LispVal> args;
    auto result = spec.func(args);
    BOOST_REQUIRE(result.has_value());
    // Result must be #t or #f — we can't know which, but it must be one of them
    BOOST_TEST((*result == True || *result == False));
}

BOOST_AUTO_TEST_CASE(cuda_device_count_returns_fixnum) {
    Heap heap(1ull << 22);
    InternTable intern;
    BuiltinEnvironment env;
    register_torch_primitives(env, heap, intern, nullptr);

    auto idx = env.lookup("torch/cuda-device-count");
    BOOST_REQUIRE(idx.has_value());

    std::vector<LispVal> args;
    auto result = env.specs()[*idx].func(args);
    BOOST_REQUIRE(result.has_value());
    // Should be a non-negative fixnum
    auto decoded = ops::decode<int64_t>(*result);
    BOOST_REQUIRE(decoded.has_value());
    BOOST_TEST(*decoded >= 0);
}

BOOST_AUTO_TEST_CASE(device_returns_cpu_string) {
    Heap heap(1ull << 22);
    InternTable intern;
    BuiltinEnvironment env;
    register_torch_primitives(env, heap, intern, nullptr);

    // Create a tensor on CPU
    auto t_val = expect_ok(tf::make_tensor(heap, torch::ones({2}, torch::kFloat64)));

    auto idx = env.lookup("torch/device");
    BOOST_REQUIRE(idx.has_value());

    std::vector<LispVal> args = {t_val};
    auto result = env.specs()[*idx].func(args);
    BOOST_REQUIRE(result.has_value());
    // Should be a string tagged value
    BOOST_TEST(ops::is_boxed(*result));
    BOOST_TEST(ops::tag(*result) == Tag::String);
    // Resolve the string — should contain "cpu"
    auto sv = intern.get_string(ops::payload(*result));
    BOOST_REQUIRE(sv.has_value());
    BOOST_TEST(std::string(*sv).find("cpu") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(to_device_cpu_roundtrip) {
    Heap heap(1ull << 22);
    InternTable intern;
    BuiltinEnvironment env;
    register_torch_primitives(env, heap, intern, nullptr);

    // Create a tensor
    auto t_val = expect_ok(tf::make_tensor(heap, torch::ones({3}, torch::kFloat64)));
    // Intern the device string "cpu"
    auto cpu_id = expect_ok(intern.intern("cpu"));
    auto cpu_str = ops::box(Tag::String, cpu_id);

    auto idx = env.lookup("torch/to-device");
    BOOST_REQUIRE(idx.has_value());

    std::vector<LispVal> args = {t_val, cpu_str};
    auto result = env.specs()[*idx].func(args);
    BOOST_REQUIRE(result.has_value());
    // Result should be a new tensor on CPU
    auto* tp = heap.try_get_as<ObjectKind::Tensor, TensorPtr>(ops::payload(*result));
    BOOST_REQUIRE(tp != nullptr);
    BOOST_TEST(tp->tensor.device().is_cpu());
    BOOST_TEST(tp->tensor.numel() == 3);
}

BOOST_AUTO_TEST_CASE(to_device_invalid_device_returns_error) {
    Heap heap(1ull << 22);
    InternTable intern;
    BuiltinEnvironment env;
    register_torch_primitives(env, heap, intern, nullptr);

    auto t_val = expect_ok(tf::make_tensor(heap, torch::ones({2}, torch::kFloat64)));
    auto bad_id = expect_ok(intern.intern("not_a_device"));
    auto bad_str = ops::box(Tag::String, bad_id);

    auto idx = env.lookup("torch/to-device");
    BOOST_REQUIRE(idx.has_value());

    std::vector<LispVal> args = {t_val, bad_str};
    auto result = env.specs()[*idx].func(args);
    // Should fail gracefully with an error
    BOOST_TEST(!result.has_value());
}

BOOST_AUTO_TEST_CASE(nn_to_device_cpu) {
    Heap heap(1ull << 22);
    InternTable intern;
    BuiltinEnvironment env;
    register_torch_primitives(env, heap, intern, nullptr);

    // Create an nn module on CPU
    auto mod = torch::nn::Linear(torch::nn::LinearOptions(3, 2));
    mod->to(torch::kFloat64);
    auto mod_val = expect_ok(tf::make_nn_module(heap, mod.ptr(), "Linear(3,2)"));

    auto cpu_id = expect_ok(intern.intern("cpu"));
    auto cpu_str = ops::box(Tag::String, cpu_id);

    auto idx = env.lookup("nn/to-device");
    BOOST_REQUIRE(idx.has_value());

    std::vector<LispVal> args = {mod_val, cpu_str};
    auto result = env.specs()[*idx].func(args);
    BOOST_REQUIRE(result.has_value());
    // nn/to-device returns the same module handle
    BOOST_TEST(*result == mod_val);
    // Parameters should still be on CPU
    auto* mp = heap.try_get_as<ObjectKind::NNModule, NNModulePtr>(ops::payload(*result));
    BOOST_REQUIRE(mp);
    BOOST_TEST(mp->module->parameters()[0].device().is_cpu());
}

// ═══════════════════════════════════════════════════════════════════════════
// Device management builtin names
// ═══════════════════════════════════════════════════════════════════════════

BOOST_AUTO_TEST_CASE(device_builtin_names_registered) {
    BuiltinEnvironment env;
    register_torch_builtin_names(env);

    BOOST_TEST(env.lookup("torch/cuda-available?").has_value());
    BOOST_TEST(env.lookup("torch/cuda-device-count").has_value());
    BOOST_TEST(env.lookup("torch/device").has_value());
    BOOST_TEST(env.lookup("torch/to-device").has_value());
    BOOST_TEST(env.lookup("nn/to-device").has_value());
}

// ═══════════════════════════════════════════════════════════════════════════
// Primitive invocation through BuiltinSpec (integration smoke tests)
// ═══════════════════════════════════════════════════════════════════════════

BOOST_AUTO_TEST_CASE(prim_torch_ones_via_env) {
    Heap heap(1ull << 22);
    InternTable intern;
    BuiltinEnvironment env;
    register_torch_primitives(env, heap, intern, nullptr);

    // Build shape list (2 3)
    auto three = expect_ok(ops::encode(int64_t(3)));
    auto two   = expect_ok(ops::encode(int64_t(2)));
    auto tail  = expect_ok(make_cons(heap, three, Nil));
    auto shape = expect_ok(make_cons(heap, two, tail));

    auto idx = env.lookup("torch/ones");
    BOOST_REQUIRE(idx.has_value());

    std::vector<LispVal> args = {shape};
    auto result = env.specs()[*idx].func(args);
    BOOST_REQUIRE(result.has_value());

    auto* tp = heap.try_get_as<ObjectKind::Tensor, TensorPtr>(ops::payload(*result));
    BOOST_REQUIRE(tp);
    BOOST_TEST(tp->tensor.sizes() == std::vector<int64_t>({2, 3}));
    BOOST_TEST(tp->tensor.sum().item<double>() == 6.0);
}

BOOST_AUTO_TEST_CASE(prim_torch_add_via_env) {
    Heap heap(1ull << 22);
    InternTable intern;
    BuiltinEnvironment env;
    register_torch_primitives(env, heap, intern, nullptr);

    auto a_val = expect_ok(tf::make_tensor(heap, torch::ones({3}, torch::kFloat64)));
    auto b_val = expect_ok(tf::make_tensor(heap, torch::ones({3}, torch::kFloat64) * 2.0));

    auto idx = env.lookup("torch/add");
    BOOST_REQUIRE(idx.has_value());

    std::vector<LispVal> args = {a_val, b_val};
    auto result = env.specs()[*idx].func(args);
    BOOST_REQUIRE(result.has_value());

    auto* tp = heap.try_get_as<ObjectKind::Tensor, TensorPtr>(ops::payload(*result));
    BOOST_REQUIRE(tp);
    BOOST_TEST(tp->tensor[0].item<double>() == 3.0);
}

BOOST_AUTO_TEST_CASE(prim_torch_item_via_env) {
    Heap heap(1ull << 22);
    InternTable intern;
    BuiltinEnvironment env;
    register_torch_primitives(env, heap, intern, nullptr);

    auto t_val = expect_ok(tf::make_tensor(heap, torch::tensor(42.0, torch::kFloat64)));

    auto idx = env.lookup("torch/item");
    BOOST_REQUIRE(idx.has_value());

    std::vector<LispVal> args = {t_val};
    auto result = env.specs()[*idx].func(args);
    BOOST_REQUIRE(result.has_value());
    // Should be a raw double (not boxed) — 42.0
    BOOST_TEST(std::bit_cast<double>(*result) == 42.0);
}

BOOST_AUTO_TEST_CASE(prim_torch_backward_and_grad_via_env) {
    Heap heap(1ull << 22);
    InternTable intern;
    BuiltinEnvironment env;
    register_torch_primitives(env, heap, intern, nullptr);

    // x = 3.0 with requires_grad
    auto x_t = torch::tensor(3.0, torch::TensorOptions().dtype(torch::kFloat64).requires_grad(true));
    auto x_val = expect_ok(tf::make_tensor(heap, x_t));

    // y = x * x  (done in C++ since torch ops aren't Eta primitives for this)
    auto* xp = heap.try_get_as<ObjectKind::Tensor, TensorPtr>(ops::payload(x_val));
    auto y_t = xp->tensor * xp->tensor;
    auto y_val = expect_ok(tf::make_tensor(heap, y_t));

    // backward(y)
    auto bw_idx = env.lookup("torch/backward");
    BOOST_REQUIRE(bw_idx.has_value());
    std::vector<LispVal> bw_args = {y_val};
    auto bw_res = env.specs()[*bw_idx].func(bw_args);
    BOOST_REQUIRE(bw_res.has_value());

    // grad(x) should be 2*3 = 6
    auto gr_idx = env.lookup("torch/grad");
    BOOST_REQUIRE(gr_idx.has_value());
    std::vector<LispVal> gr_args = {x_val};
    auto gr_res = env.specs()[*gr_idx].func(gr_args);
    BOOST_REQUIRE(gr_res.has_value());

    auto* gp = heap.try_get_as<ObjectKind::Tensor, TensorPtr>(ops::payload(*gr_res));
    BOOST_REQUIRE(gp);
    BOOST_TEST(std::abs(gp->tensor.item<double>() - 6.0) < 1e-10);
}

BOOST_AUTO_TEST_CASE(builtin_names_and_primitives_count_match) {
    // Verify both registration functions produce the same number of entries
    BuiltinEnvironment names_env;
    register_torch_builtin_names(names_env);

    Heap heap(1ull << 22);
    InternTable intern;
    BuiltinEnvironment prim_env;
    register_torch_primitives(prim_env, heap, intern, nullptr);

    BOOST_TEST(names_env.size() == prim_env.size());

    // And every name in one should match the other
    for (size_t i = 0; i < names_env.size(); ++i) {
        BOOST_TEST(names_env.specs()[i].name == prim_env.specs()[i].name);
        BOOST_TEST(names_env.specs()[i].arity == prim_env.specs()[i].arity);
        BOOST_TEST(names_env.specs()[i].has_rest == prim_env.specs()[i].has_rest);
    }
}

// ═══════════════════════════════════════════════════════════════════════════
// End-to-end: training loop
// ═══════════════════════════════════════════════════════════════════════════

BOOST_AUTO_TEST_CASE(training_loop_converges) {
    // Simple regression: y = 2x + 1
    auto model = torch::nn::Linear(torch::nn::LinearOptions(1, 1));
    model->to(torch::kFloat64);
    auto opt = torch::optim::SGD(model->parameters(), torch::optim::SGDOptions(0.01));

    auto x = torch::tensor({{1.0}, {2.0}, {3.0}, {4.0}}, torch::kFloat64);
    auto y = torch::tensor({{3.0}, {5.0}, {7.0}, {9.0}}, torch::kFloat64);

    double initial_loss = 0, final_loss = 0;
    for (int epoch = 0; epoch < 200; ++epoch) {
        opt.zero_grad();
        auto pred = model->forward(x);
        auto loss = torch::mse_loss(pred, y);
        if (epoch == 0) initial_loss = loss.item<double>();
        if (epoch == 199) final_loss = loss.item<double>();
        loss.backward();
        opt.step();
    }

    BOOST_TEST(final_loss < initial_loss);
    BOOST_TEST(final_loss < 0.1);  // Should converge reasonably well
}

BOOST_AUTO_TEST_SUITE_END()

