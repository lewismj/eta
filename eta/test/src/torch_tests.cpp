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

// TensorPtr heap lifecycle

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

// Tensor creation primitives

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

// Tensor arithmetic

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

// Autograd

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

// NN Modules

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

// Optimizers

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

// Loss functions

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

// Primitive registration (smoke test)

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

// Device management

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

// Device management builtin names

BOOST_AUTO_TEST_CASE(device_builtin_names_registered) {
    BuiltinEnvironment env;
    register_torch_builtin_names(env);

    BOOST_TEST(env.lookup("torch/cuda-available?").has_value());
    BOOST_TEST(env.lookup("torch/cuda-device-count").has_value());
    BOOST_TEST(env.lookup("torch/device").has_value());
    BOOST_TEST(env.lookup("torch/to-device").has_value());
    BOOST_TEST(env.lookup("nn/to-device").has_value());
}

// Primitive invocation through BuiltinSpec (integration smoke tests)

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

// End-to-end: training loop

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
    BOOST_TEST(final_loss < 0.2);  // Should converge reasonably well
}

// nn/sequential via Eta primitive (the gap that caused the sequential bug)

namespace {
    // Helper: count elements in an Eta cons-list
    int count_list(Heap& heap, LispVal lst) {
        int n = 0;
        while (lst != Nil) {
            if (!ops::is_boxed(lst) || ops::tag(lst) != Tag::HeapObject) break;
            auto* c = heap.try_get_as<ObjectKind::Cons, types::Cons>(ops::payload(lst));
            if (!c) break;
            ++n;
            lst = c->cdr;
        }
        return n;
    }
}

BOOST_AUTO_TEST_CASE(prim_nn_sequential_forward_via_env) {
    Heap heap(1ull << 22);
    InternTable intern;
    BuiltinEnvironment env;
    register_torch_primitives(env, heap, intern, nullptr);

    // Lookup all needed primitives
    auto lin_idx  = env.lookup("nn/linear");
    auto relu_idx = env.lookup("nn/relu-layer");
    auto seq_idx  = env.lookup("nn/sequential");
    auto fwd_idx  = env.lookup("nn/forward");
    auto par_idx  = env.lookup("nn/parameters");
    auto mod_idx  = env.lookup("nn/module?");
    BOOST_REQUIRE(lin_idx && relu_idx && seq_idx && fwd_idx && par_idx && mod_idx);

    auto mk = [](int64_t v) { return expect_ok(ops::encode(v)); };

    // Create: Linear(4, 8), ReLU, Linear(8, 2)
    auto lin1 = expect_ok(env.specs()[*lin_idx].func({mk(4), mk(8)}));
    auto relu = expect_ok(env.specs()[*relu_idx].func({}));
    auto lin2 = expect_ok(env.specs()[*lin_idx].func({mk(8), mk(2)}));

    // Build sequential through the Eta primitive — this is the exact path the VM takes
    auto seq = expect_ok(env.specs()[*seq_idx].func({lin1, relu, lin2}));

    // Verify it's a module
    auto is_mod = expect_ok(env.specs()[*mod_idx].func({seq}));
    BOOST_TEST(is_mod == True);

    // Forward pass
    auto input = expect_ok(tf::make_tensor(heap, torch::randn({1, 4}, torch::kFloat64)));
    auto output = expect_ok(env.specs()[*fwd_idx].func({seq, input}));
    auto* tp = heap.try_get_as<ObjectKind::Tensor, TensorPtr>(ops::payload(output));
    BOOST_REQUIRE(tp);
    BOOST_TEST(tp->tensor.sizes() == std::vector<int64_t>({1, 2}));

    // Verify parameters are accessible (weight+bias from both linears = 4 tensors)
    auto params = expect_ok(env.specs()[*par_idx].func({seq}));
    BOOST_TEST(count_list(heap, params) == 4);
}

BOOST_AUTO_TEST_CASE(prim_nn_sequential_single_layer) {
    Heap heap(1ull << 22);
    InternTable intern;
    BuiltinEnvironment env;
    register_torch_primitives(env, heap, intern, nullptr);

    auto mk = [](int64_t v) { return expect_ok(ops::encode(v)); };
    auto lin_idx = *env.lookup("nn/linear");
    auto seq_idx = *env.lookup("nn/sequential");
    auto fwd_idx = *env.lookup("nn/forward");

    auto l1  = expect_ok(env.specs()[lin_idx].func({mk(3), mk(2)}));
    auto seq = expect_ok(env.specs()[seq_idx].func({l1}));

    auto input  = expect_ok(tf::make_tensor(heap, torch::randn({1, 3}, torch::kFloat64)));
    auto output = expect_ok(env.specs()[fwd_idx].func({seq, input}));
    auto* tp = get_tensor(heap, output);
    BOOST_REQUIRE(tp);
    BOOST_TEST(tp->tensor.sizes() == std::vector<int64_t>({1, 2}));
}

BOOST_AUTO_TEST_CASE(prim_nn_sequential_deep_network) {
    // 5-layer network mirroring causal_demo: 2 → 32 → ReLU → 16 → ReLU → 1
    Heap heap(1ull << 22);
    InternTable intern;
    BuiltinEnvironment env;
    register_torch_primitives(env, heap, intern, nullptr);

    auto mk = [](int64_t v) { return expect_ok(ops::encode(v)); };
    auto lin_idx  = *env.lookup("nn/linear");
    auto relu_idx = *env.lookup("nn/relu-layer");
    auto seq_idx  = *env.lookup("nn/sequential");
    auto fwd_idx  = *env.lookup("nn/forward");
    auto par_idx  = *env.lookup("nn/parameters");

    auto l1 = expect_ok(env.specs()[lin_idx].func({mk(2), mk(32)}));
    auto r1 = expect_ok(env.specs()[relu_idx].func({}));
    auto l2 = expect_ok(env.specs()[lin_idx].func({mk(32), mk(16)}));
    auto r2 = expect_ok(env.specs()[relu_idx].func({}));
    auto l3 = expect_ok(env.specs()[lin_idx].func({mk(16), mk(1)}));

    auto seq = expect_ok(env.specs()[seq_idx].func({l1, r1, l2, r2, l3}));

    // Forward with batch of 5
    auto input  = expect_ok(tf::make_tensor(heap, torch::randn({5, 2}, torch::kFloat64)));
    auto output = expect_ok(env.specs()[fwd_idx].func({seq, input}));
    auto* tp = get_tensor(heap, output);
    BOOST_REQUIRE(tp);
    BOOST_TEST(tp->tensor.sizes() == std::vector<int64_t>({5, 1}));

    // 3 linear layers × (weight + bias) = 6 parameter tensors
    auto params = expect_ok(env.specs()[par_idx].func({seq}));
    BOOST_TEST(count_list(heap, params) == 6);
}

BOOST_AUTO_TEST_CASE(prim_nn_sequential_with_sigmoid) {
    Heap heap(1ull << 22);
    InternTable intern;
    BuiltinEnvironment env;
    register_torch_primitives(env, heap, intern, nullptr);

    auto mk = [](int64_t v) { return expect_ok(ops::encode(v)); };
    auto lin_idx = *env.lookup("nn/linear");
    auto sig_idx = *env.lookup("nn/sigmoid-layer");
    auto seq_idx = *env.lookup("nn/sequential");
    auto fwd_idx = *env.lookup("nn/forward");

    auto l1  = expect_ok(env.specs()[lin_idx].func({mk(4), mk(2)}));
    auto sig = expect_ok(env.specs()[sig_idx].func({}));
    auto seq = expect_ok(env.specs()[seq_idx].func({l1, sig}));

    auto input  = expect_ok(tf::make_tensor(heap, torch::randn({1, 4}, torch::kFloat64)));
    auto output = expect_ok(env.specs()[fwd_idx].func({seq, input}));
    auto* tp = get_tensor(heap, output);
    BOOST_REQUIRE(tp);
    // Sigmoid outputs are in (0, 1)
    BOOST_TEST(tp->tensor.min().item<double>() >= 0.0);
    BOOST_TEST(tp->tensor.max().item<double>() <= 1.0);
}

BOOST_AUTO_TEST_CASE(prim_nn_sequential_with_dropout) {
    Heap heap(1ull << 22);
    InternTable intern;
    BuiltinEnvironment env;
    register_torch_primitives(env, heap, intern, nullptr);

    auto mk     = [](int64_t v)  { return expect_ok(ops::encode(v)); };
    auto mk_dbl = [](double v)   { return expect_ok(ops::encode(v)); };
    auto lin_idx  = *env.lookup("nn/linear");
    auto drop_idx = *env.lookup("nn/dropout");
    auto seq_idx  = *env.lookup("nn/sequential");
    auto fwd_idx  = *env.lookup("nn/forward");
    auto par_idx  = *env.lookup("nn/parameters");

    auto l1   = expect_ok(env.specs()[lin_idx].func({mk(4), mk(4)}));
    auto drop = expect_ok(env.specs()[drop_idx].func({mk_dbl(0.5)}));
    auto l2   = expect_ok(env.specs()[lin_idx].func({mk(4), mk(1)}));
    auto seq  = expect_ok(env.specs()[seq_idx].func({l1, drop, l2}));

    // Forward pass should work — just verify it produces valid output
    auto input = expect_ok(tf::make_tensor(heap, torch::ones({1, 4}, torch::kFloat64)));
    auto out = expect_ok(env.specs()[fwd_idx].func({seq, input}));
    auto* tp = get_tensor(heap, out);
    BOOST_REQUIRE(tp);
    BOOST_TEST(tp->tensor.sizes() == std::vector<int64_t>({1, 1}));

    // Parameters should come from the two linear layers (dropout has none)
    auto params = expect_ok(env.specs()[par_idx].func({seq}));
    BOOST_TEST(count_list(heap, params) == 4);  // l1 weight+bias + l2 weight+bias
}

// nn/train! and nn/eval! via primitives

BOOST_AUTO_TEST_CASE(prim_nn_train_eval_mode) {
    Heap heap(1ull << 22);
    InternTable intern;
    BuiltinEnvironment env;
    register_torch_primitives(env, heap, intern, nullptr);

    auto mk_dbl   = [](double v) { return expect_ok(ops::encode(v)); };
    auto drop_idx  = *env.lookup("nn/dropout");
    auto train_idx = *env.lookup("nn/train!");
    auto eval_idx  = *env.lookup("nn/eval!");
    auto fwd_idx   = *env.lookup("nn/forward");

    auto drop = expect_ok(env.specs()[drop_idx].func({mk_dbl(0.5)}));

    // In eval mode, dropout should be identity
    (void) env.specs()[eval_idx].func({drop});
    auto input = expect_ok(tf::make_tensor(heap, torch::ones({100}, torch::kFloat64)));
    auto out_eval = expect_ok(env.specs()[fwd_idx].func({drop, input}));
    auto* tp = get_tensor(heap, out_eval);
    BOOST_REQUIRE(tp);
    BOOST_TEST(std::abs(tp->tensor.sum().item<double>() - 100.0) < 1e-10);

    // Switch back to train mode — output should differ from identity
    // (statistically, with p=0.5 on 100 elements, sum should be ~100 after
    //  scaling but individual elements are zeroed; just verify it runs)
    (void) env.specs()[train_idx].func({drop});
    auto out_train = expect_ok(env.specs()[fwd_idx].func({drop, input}));
    BOOST_REQUIRE(get_tensor(heap, out_train));
}

// Tensor creation primitives via env (arange, linspace, zeros, randn)

BOOST_AUTO_TEST_CASE(prim_torch_zeros_via_env) {
    Heap heap(1ull << 22);
    InternTable intern;
    BuiltinEnvironment env;
    register_torch_primitives(env, heap, intern, nullptr);

    auto mk = [](int64_t v) { return expect_ok(ops::encode(v)); };
    auto two  = mk(2);
    auto tail = expect_ok(make_cons(heap, mk(3), Nil));
    auto shape = expect_ok(make_cons(heap, two, tail));

    auto result = expect_ok(env.specs()[*env.lookup("torch/zeros")].func({shape}));
    auto* tp = get_tensor(heap, result);
    BOOST_REQUIRE(tp);
    BOOST_TEST(tp->tensor.sizes() == std::vector<int64_t>({2, 3}));
    BOOST_TEST(tp->tensor.sum().item<double>() == 0.0);
}

BOOST_AUTO_TEST_CASE(prim_torch_arange_via_env) {
    Heap heap(1ull << 22);
    InternTable intern;
    BuiltinEnvironment env;
    register_torch_primitives(env, heap, intern, nullptr);

    auto mk_dbl = [](double v) { return expect_ok(ops::encode(v)); };
    auto result = expect_ok(env.specs()[*env.lookup("torch/arange")].func(
        {mk_dbl(0.0), mk_dbl(5.0), mk_dbl(1.0)}));
    auto* tp = get_tensor(heap, result);
    BOOST_REQUIRE(tp);
    BOOST_TEST(tp->tensor.numel() == 5);
    BOOST_TEST(tp->tensor[0].item<double>() == 0.0);
    BOOST_TEST(tp->tensor[4].item<double>() == 4.0);
}

BOOST_AUTO_TEST_CASE(prim_torch_linspace_via_env) {
    Heap heap(1ull << 22);
    InternTable intern;
    BuiltinEnvironment env;
    register_torch_primitives(env, heap, intern, nullptr);

    auto mk = [](double v) { return expect_ok(ops::encode(v)); };
    auto mk_i = [](int64_t v) { return expect_ok(ops::encode(v)); };
    auto result = expect_ok(env.specs()[*env.lookup("torch/linspace")].func(
        {mk(0.0), mk(1.0), mk_i(5)}));
    auto* tp = get_tensor(heap, result);
    BOOST_REQUIRE(tp);
    BOOST_TEST(tp->tensor.numel() == 5);
    BOOST_TEST(std::abs(tp->tensor[0].item<double>() - 0.0) < 1e-10);
    BOOST_TEST(std::abs(tp->tensor[4].item<double>() - 1.0) < 1e-10);
}

// Arithmetic primitives via env (sub, mul, div, dot)

BOOST_AUTO_TEST_CASE(prim_torch_sub_via_env) {
    Heap heap(1ull << 22);
    InternTable intern;
    BuiltinEnvironment env;
    register_torch_primitives(env, heap, intern, nullptr);

    auto a = expect_ok(tf::make_tensor(heap, torch::tensor({5.0, 3.0}, torch::kFloat64)));
    auto b = expect_ok(tf::make_tensor(heap, torch::tensor({2.0, 1.0}, torch::kFloat64)));
    auto result = expect_ok(env.specs()[*env.lookup("torch/sub")].func({a, b}));
    auto* tp = get_tensor(heap, result);
    BOOST_REQUIRE(tp);
    BOOST_TEST(tp->tensor[0].item<double>() == 3.0);
    BOOST_TEST(tp->tensor[1].item<double>() == 2.0);
}

BOOST_AUTO_TEST_CASE(prim_torch_mul_via_env) {
    Heap heap(1ull << 22);
    InternTable intern;
    BuiltinEnvironment env;
    register_torch_primitives(env, heap, intern, nullptr);

    auto a = expect_ok(tf::make_tensor(heap, torch::tensor({2.0, 3.0}, torch::kFloat64)));
    auto b = expect_ok(tf::make_tensor(heap, torch::tensor({4.0, 5.0}, torch::kFloat64)));
    auto result = expect_ok(env.specs()[*env.lookup("torch/mul")].func({a, b}));
    auto* tp = get_tensor(heap, result);
    BOOST_REQUIRE(tp);
    BOOST_TEST(tp->tensor[0].item<double>() == 8.0);
    BOOST_TEST(tp->tensor[1].item<double>() == 15.0);
}

BOOST_AUTO_TEST_CASE(prim_torch_div_via_env) {
    Heap heap(1ull << 22);
    InternTable intern;
    BuiltinEnvironment env;
    register_torch_primitives(env, heap, intern, nullptr);

    auto a = expect_ok(tf::make_tensor(heap, torch::tensor({6.0, 9.0}, torch::kFloat64)));
    auto b = expect_ok(tf::make_tensor(heap, torch::tensor({2.0, 3.0}, torch::kFloat64)));
    auto result = expect_ok(env.specs()[*env.lookup("torch/div")].func({a, b}));
    auto* tp = get_tensor(heap, result);
    BOOST_REQUIRE(tp);
    BOOST_TEST(tp->tensor[0].item<double>() == 3.0);
    BOOST_TEST(tp->tensor[1].item<double>() == 3.0);
}

BOOST_AUTO_TEST_CASE(prim_torch_matmul_via_env) {
    Heap heap(1ull << 22);
    InternTable intern;
    BuiltinEnvironment env;
    register_torch_primitives(env, heap, intern, nullptr);

    auto a = expect_ok(tf::make_tensor(heap, torch::ones({2, 3}, torch::kFloat64)));
    auto b = expect_ok(tf::make_tensor(heap, torch::ones({3, 4}, torch::kFloat64)));
    auto result = expect_ok(env.specs()[*env.lookup("torch/matmul")].func({a, b}));
    auto* tp = get_tensor(heap, result);
    BOOST_REQUIRE(tp);
    BOOST_TEST(tp->tensor.sizes() == std::vector<int64_t>({2, 4}));
    BOOST_TEST(tp->tensor[0][0].item<double>() == 3.0);
}

BOOST_AUTO_TEST_CASE(prim_torch_dot_via_env) {
    Heap heap(1ull << 22);
    InternTable intern;
    BuiltinEnvironment env;
    register_torch_primitives(env, heap, intern, nullptr);

    auto a = expect_ok(tf::make_tensor(heap, torch::tensor({1.0, 2.0, 3.0}, torch::kFloat64)));
    auto b = expect_ok(tf::make_tensor(heap, torch::tensor({4.0, 5.0, 6.0}, torch::kFloat64)));
    auto result = expect_ok(env.specs()[*env.lookup("torch/dot")].func({a, b}));
    auto* tp = get_tensor(heap, result);
    BOOST_REQUIRE(tp);
    BOOST_TEST(tp->tensor.item<double>() == 32.0);  // 1*4 + 2*5 + 3*6
}

// Unary ops via env

BOOST_AUTO_TEST_CASE(prim_unary_ops_via_env) {
    Heap heap(1ull << 22);
    InternTable intern;
    BuiltinEnvironment env;
    register_torch_primitives(env, heap, intern, nullptr);

    auto t_val = expect_ok(tf::make_tensor(heap, torch::tensor({-2.0, 0.0, 3.0}, torch::kFloat64)));

    // neg
    auto neg_r = expect_ok(env.specs()[*env.lookup("torch/neg")].func({t_val}));
    BOOST_TEST(get_tensor(heap, neg_r)->tensor[0].item<double>() == 2.0);

    // abs
    auto abs_r = expect_ok(env.specs()[*env.lookup("torch/abs")].func({t_val}));
    BOOST_TEST(get_tensor(heap, abs_r)->tensor[0].item<double>() == 2.0);

    // relu
    auto relu_r = expect_ok(env.specs()[*env.lookup("torch/relu")].func({t_val}));
    BOOST_TEST(get_tensor(heap, relu_r)->tensor[0].item<double>() == 0.0);
    BOOST_TEST(get_tensor(heap, relu_r)->tensor[2].item<double>() == 3.0);

    // sigmoid — output in (0, 1)
    auto sig_r = expect_ok(env.specs()[*env.lookup("torch/sigmoid")].func({t_val}));
    auto* sig_tp = get_tensor(heap, sig_r);
    for (int i = 0; i < 3; ++i) {
        BOOST_TEST(sig_tp->tensor[i].item<double>() > 0.0);
        BOOST_TEST(sig_tp->tensor[i].item<double>() < 1.0);
    }

    // tanh — output in (-1, 1)
    auto tanh_r = expect_ok(env.specs()[*env.lookup("torch/tanh")].func({t_val}));
    auto* tanh_tp = get_tensor(heap, tanh_r);
    for (int i = 0; i < 3; ++i) {
        BOOST_TEST(tanh_tp->tensor[i].item<double>() > -1.0);
        BOOST_TEST(tanh_tp->tensor[i].item<double>() < 1.0);
    }

    // exp
    auto z = expect_ok(tf::make_tensor(heap, torch::zeros({2}, torch::kFloat64)));
    auto exp_r = expect_ok(env.specs()[*env.lookup("torch/exp")].func({z}));
    BOOST_TEST(std::abs(get_tensor(heap, exp_r)->tensor[0].item<double>() - 1.0) < 1e-10);

    // log
    auto ones_val = expect_ok(tf::make_tensor(heap, torch::ones({2}, torch::kFloat64)));
    auto log_r = expect_ok(env.specs()[*env.lookup("torch/log")].func({ones_val}));
    BOOST_TEST(std::abs(get_tensor(heap, log_r)->tensor[0].item<double>()) < 1e-10);

    // sqrt
    auto sq_val = expect_ok(tf::make_tensor(heap, torch::tensor({4.0, 9.0}, torch::kFloat64)));
    auto sqrt_r = expect_ok(env.specs()[*env.lookup("torch/sqrt")].func({sq_val}));
    BOOST_TEST(std::abs(get_tensor(heap, sqrt_r)->tensor[0].item<double>() - 2.0) < 1e-10);
    BOOST_TEST(std::abs(get_tensor(heap, sqrt_r)->tensor[1].item<double>() - 3.0) < 1e-10);
}

BOOST_AUTO_TEST_CASE(prim_softmax_via_env) {
    Heap heap(1ull << 22);
    InternTable intern;
    BuiltinEnvironment env;
    register_torch_primitives(env, heap, intern, nullptr);

    auto t_val = expect_ok(tf::make_tensor(heap, torch::tensor({1.0, 2.0, 3.0}, torch::kFloat64)));
    auto dim = expect_ok(ops::encode(int64_t(0)));
    auto result = expect_ok(env.specs()[*env.lookup("torch/softmax")].func({t_val, dim}));
    auto* tp = get_tensor(heap, result);
    BOOST_REQUIRE(tp);
    // Softmax sums to 1.0
    BOOST_TEST(std::abs(tp->tensor.sum().item<double>() - 1.0) < 1e-10);
    // All elements positive
    BOOST_TEST(tp->tensor.min().item<double>() > 0.0);
}

// Shape ops via env

BOOST_AUTO_TEST_CASE(prim_shape_ops_via_env) {
    Heap heap(1ull << 22);
    InternTable intern;
    BuiltinEnvironment env;
    register_torch_primitives(env, heap, intern, nullptr);

    auto mk_i = [](int64_t v) { return expect_ok(ops::encode(v)); };

    // Create a (2, 3) tensor
    auto t_val = expect_ok(tf::make_tensor(heap, torch::ones({2, 3}, torch::kFloat64)));

    // shape
    auto shape_r = expect_ok(env.specs()[*env.lookup("torch/shape")].func({t_val}));
    auto shape_vec = list_to_shape(shape_r, heap);
    BOOST_TEST(shape_vec.size() == 2);
    BOOST_TEST(shape_vec[0] == 2);
    BOOST_TEST(shape_vec[1] == 3);

    // reshape to (3, 2)
    auto new_shape = expect_ok(make_cons(heap, mk_i(2), Nil));
    new_shape = expect_ok(make_cons(heap, mk_i(3), new_shape));
    auto reshaped = expect_ok(env.specs()[*env.lookup("torch/reshape")].func({t_val, new_shape}));
    auto* rp = get_tensor(heap, reshaped);
    BOOST_REQUIRE(rp);
    BOOST_TEST(rp->tensor.sizes() == std::vector<int64_t>({3, 2}));

    // transpose
    auto transposed = expect_ok(env.specs()[*env.lookup("torch/transpose")].func(
        {t_val, mk_i(0), mk_i(1)}));
    auto* trp = get_tensor(heap, transposed);
    BOOST_REQUIRE(trp);
    BOOST_TEST(trp->tensor.sizes() == std::vector<int64_t>({3, 2}));

    // unsqueeze
    auto unsq = expect_ok(env.specs()[*env.lookup("torch/unsqueeze")].func({t_val, mk_i(0)}));
    auto* up = get_tensor(heap, unsq);
    BOOST_REQUIRE(up);
    BOOST_TEST(up->tensor.sizes() == std::vector<int64_t>({1, 2, 3}));

    // squeeze
    auto squeezed = expect_ok(env.specs()[*env.lookup("torch/squeeze")].func({unsq}));
    auto* sp = get_tensor(heap, squeezed);
    BOOST_REQUIRE(sp);
    BOOST_TEST(sp->tensor.sizes() == std::vector<int64_t>({2, 3}));
}

BOOST_AUTO_TEST_CASE(prim_torch_cat_via_env) {
    Heap heap(1ull << 22);
    InternTable intern;
    BuiltinEnvironment env;
    register_torch_primitives(env, heap, intern, nullptr);

    auto a = expect_ok(tf::make_tensor(heap, torch::ones({2, 3}, torch::kFloat64)));
    auto b = expect_ok(tf::make_tensor(heap, torch::zeros({2, 3}, torch::kFloat64)));
    // Build list (a b)
    auto lst = expect_ok(make_cons(heap, b, Nil));
    lst = expect_ok(make_cons(heap, a, lst));
    auto dim = expect_ok(ops::encode(int64_t(0)));
    auto result = expect_ok(env.specs()[*env.lookup("torch/cat")].func({lst, dim}));
    auto* tp = get_tensor(heap, result);
    BOOST_REQUIRE(tp);
    BOOST_TEST(tp->tensor.sizes() == std::vector<int64_t>({4, 3}));
    BOOST_TEST(tp->tensor.sum().item<double>() == 6.0);  // 2*3 ones + 2*3 zeros
}

// Reduction primitives via env

BOOST_AUTO_TEST_CASE(prim_reductions_via_env) {
    Heap heap(1ull << 22);
    InternTable intern;
    BuiltinEnvironment env;
    register_torch_primitives(env, heap, intern, nullptr);

    auto t_val = expect_ok(tf::make_tensor(heap, torch::tensor({1.0, 5.0, 3.0, 2.0}, torch::kFloat64)));

    // sum
    auto sum_r = expect_ok(env.specs()[*env.lookup("torch/sum")].func({t_val}));
    BOOST_TEST(get_tensor(heap, sum_r)->tensor.item<double>() == 11.0);

    // mean
    auto mean_r = expect_ok(env.specs()[*env.lookup("torch/mean")].func({t_val}));
    BOOST_TEST(std::abs(get_tensor(heap, mean_r)->tensor.item<double>() - 2.75) < 1e-10);

    // max
    auto max_r = expect_ok(env.specs()[*env.lookup("torch/max")].func({t_val}));
    BOOST_TEST(get_tensor(heap, max_r)->tensor.item<double>() == 5.0);

    // min
    auto min_r = expect_ok(env.specs()[*env.lookup("torch/min")].func({t_val}));
    BOOST_TEST(get_tensor(heap, min_r)->tensor.item<double>() == 1.0);

    // argmax → index 1 (value 5.0)
    auto amax_r = expect_ok(env.specs()[*env.lookup("torch/argmax")].func({t_val}));
    BOOST_TEST(get_tensor(heap, amax_r)->tensor.item<int64_t>() == 1);

    // argmin → index 0 (value 1.0)
    auto amin_r = expect_ok(env.specs()[*env.lookup("torch/argmin")].func({t_val}));
    BOOST_TEST(get_tensor(heap, amin_r)->tensor.item<int64_t>() == 0);
}

// Conversion primitives via env (numel, to-list)

BOOST_AUTO_TEST_CASE(prim_torch_numel_via_env) {
    Heap heap(1ull << 22);
    InternTable intern;
    BuiltinEnvironment env;
    register_torch_primitives(env, heap, intern, nullptr);

    auto t_val = expect_ok(tf::make_tensor(heap, torch::ones({3, 4}, torch::kFloat64)));
    auto result = expect_ok(env.specs()[*env.lookup("torch/numel")].func({t_val}));
    auto decoded = ops::decode<int64_t>(result);
    BOOST_REQUIRE(decoded.has_value());
    BOOST_TEST(*decoded == 12);
}

BOOST_AUTO_TEST_CASE(prim_torch_to_list_via_env) {
    Heap heap(1ull << 22);
    InternTable intern;
    BuiltinEnvironment env;
    register_torch_primitives(env, heap, intern, nullptr);

    auto t_val = expect_ok(tf::make_tensor(heap, torch::tensor({1.0, 2.0, 3.0}, torch::kFloat64)));
    auto result = expect_ok(env.specs()[*env.lookup("torch/to-list")].func({t_val}));
    BOOST_TEST(count_list(heap, result) == 3);
}

// Autograd primitives via env (requires-grad!, requires-grad?, detach, zero-grad!)

BOOST_AUTO_TEST_CASE(prim_autograd_full_cycle_via_env) {
    Heap heap(1ull << 22);
    InternTable intern;
    BuiltinEnvironment env;
    register_torch_primitives(env, heap, intern, nullptr);

    // Create a tensor and enable grad tracking
    auto t_val = expect_ok(tf::make_tensor(heap, torch::tensor(4.0, torch::kFloat64)));
    auto rg_set = expect_ok(env.specs()[*env.lookup("torch/requires-grad!")].func({t_val, True}));
    BOOST_TEST(rg_set == t_val);  // returns same handle

    // requires-grad? should be true
    auto rg_q = expect_ok(env.specs()[*env.lookup("torch/requires-grad?")].func({t_val}));
    BOOST_TEST(rg_q == True);

    // y = x * x, backward, check grad = 2*4 = 8
    auto* xp = get_tensor(heap, t_val);
    auto y_t = xp->tensor * xp->tensor;
    auto y_val = expect_ok(tf::make_tensor(heap, y_t));
    (void) env.specs()[*env.lookup("torch/backward")].func({y_val});
    auto gr = expect_ok(env.specs()[*env.lookup("torch/grad")].func({t_val}));
    BOOST_TEST(std::abs(get_tensor(heap, gr)->tensor.item<double>() - 8.0) < 1e-10);

    // zero-grad!
    (void) env.specs()[*env.lookup("torch/zero-grad!")].func({t_val});
    auto gr2 = expect_ok(env.specs()[*env.lookup("torch/grad")].func({t_val}));
    BOOST_TEST(std::abs(get_tensor(heap, gr2)->tensor.item<double>()) < 1e-10);

    // detach
    auto det = expect_ok(env.specs()[*env.lookup("torch/detach")].func({t_val}));
    auto rg_det = expect_ok(env.specs()[*env.lookup("torch/requires-grad?")].func({det}));
    BOOST_TEST(rg_det == False);
}

// Loss functions via env (L1, cross-entropy)

BOOST_AUTO_TEST_CASE(prim_l1_loss_via_env) {
    Heap heap(1ull << 22);
    InternTable intern;
    BuiltinEnvironment env;
    register_torch_primitives(env, heap, intern, nullptr);

    auto pred   = expect_ok(tf::make_tensor(heap, torch::tensor({1.0, 2.0, 3.0}, torch::kFloat64)));
    auto target = expect_ok(tf::make_tensor(heap, torch::tensor({2.0, 2.0, 2.0}, torch::kFloat64)));
    auto result = expect_ok(env.specs()[*env.lookup("nn/l1-loss")].func({pred, target}));
    auto* tp = get_tensor(heap, result);
    BOOST_REQUIRE(tp);
    // L1 = (|1-2| + |2-2| + |3-2|) / 3 = 2/3
    BOOST_TEST(std::abs(tp->tensor.item<double>() - 2.0/3.0) < 1e-10);
}

BOOST_AUTO_TEST_CASE(prim_mse_loss_via_env) {
    Heap heap(1ull << 22);
    InternTable intern;
    BuiltinEnvironment env;
    register_torch_primitives(env, heap, intern, nullptr);

    auto pred   = expect_ok(tf::make_tensor(heap, torch::tensor({1.0, 2.0, 3.0}, torch::kFloat64)));
    auto target = expect_ok(tf::make_tensor(heap, torch::tensor({1.0, 2.0, 3.0}, torch::kFloat64)));
    auto result = expect_ok(env.specs()[*env.lookup("nn/mse-loss")].func({pred, target}));
    BOOST_TEST(std::abs(get_tensor(heap, result)->tensor.item<double>()) < 1e-10);
}

BOOST_AUTO_TEST_CASE(prim_cross_entropy_loss_via_env) {
    Heap heap(1ull << 22);
    InternTable intern;
    BuiltinEnvironment env;
    register_torch_primitives(env, heap, intern, nullptr);

    // logits for 2-sample, 3-class classification
    auto logits  = expect_ok(tf::make_tensor(heap, torch::tensor({{2.0, 1.0, 0.1}, {0.1, 0.2, 3.0}}, torch::kFloat64)));
    auto targets = expect_ok(tf::make_tensor(heap, torch::tensor({0, 2}, torch::kLong)));
    auto result  = expect_ok(env.specs()[*env.lookup("nn/cross-entropy-loss")].func({logits, targets}));
    auto* tp = get_tensor(heap, result);
    BOOST_REQUIRE(tp);
    BOOST_TEST(tp->tensor.item<double>() > 0.0);  // loss should be positive
}

// Optimizer primitives via env (sgd, adam, step!, zero-grad!, optimizer?)

BOOST_AUTO_TEST_CASE(prim_optimizer_full_cycle_via_env) {
    Heap heap(1ull << 22);
    InternTable intern;
    BuiltinEnvironment env;
    register_torch_primitives(env, heap, intern, nullptr);

    auto mk = [](int64_t v) { return expect_ok(ops::encode(v)); };
    auto mk_dbl = [](double v) { return expect_ok(ops::encode(v)); };

    // Create a linear layer and both optimizer types
    auto lin = expect_ok(env.specs()[*env.lookup("nn/linear")].func({mk(2), mk(1)}));

    auto sgd_opt = expect_ok(env.specs()[*env.lookup("optim/sgd")].func({lin, mk_dbl(0.01)}));
    BOOST_TEST(expect_ok(env.specs()[*env.lookup("optim/optimizer?")].func({sgd_opt})) == True);
    BOOST_TEST(expect_ok(env.specs()[*env.lookup("optim/optimizer?")].func({lin})) == False);

    auto adam_opt = expect_ok(env.specs()[*env.lookup("optim/adam")].func({lin, mk_dbl(0.001)}));
    BOOST_TEST(expect_ok(env.specs()[*env.lookup("optim/optimizer?")].func({adam_opt})) == True);

    // zero-grad! and step! should not crash
    auto zg = env.specs()[*env.lookup("optim/zero-grad!")].func({sgd_opt});
    BOOST_TEST(zg.has_value());
    auto st = env.specs()[*env.lookup("optim/step!")].func({sgd_opt});
    BOOST_TEST(st.has_value());
}

// nn/linear via env

BOOST_AUTO_TEST_CASE(prim_nn_linear_forward_via_env) {
    Heap heap(1ull << 22);
    InternTable intern;
    BuiltinEnvironment env;
    register_torch_primitives(env, heap, intern, nullptr);

    auto mk = [](int64_t v) { return expect_ok(ops::encode(v)); };
    auto lin = expect_ok(env.specs()[*env.lookup("nn/linear")].func({mk(4), mk(2)}));

    // module? should be true
    BOOST_TEST(expect_ok(env.specs()[*env.lookup("nn/module?")].func({lin})) == True);

    // parameters: weight (4×2) + bias (2) = 2 tensors
    auto params = expect_ok(env.specs()[*env.lookup("nn/parameters")].func({lin}));
    BOOST_TEST(count_list(heap, params) == 2);

    // forward
    auto input  = expect_ok(tf::make_tensor(heap, torch::randn({1, 4}, torch::kFloat64)));
    auto output = expect_ok(env.specs()[*env.lookup("nn/forward")].func({lin, input}));
    auto* tp = get_tensor(heap, output);
    BOOST_REQUIRE(tp);
    BOOST_TEST(tp->tensor.sizes() == std::vector<int64_t>({1, 2}));
}

// Tensor predicate via env

BOOST_AUTO_TEST_CASE(prim_tensor_predicate_via_env) {
    Heap heap(1ull << 22);
    InternTable intern;
    BuiltinEnvironment env;
    register_torch_primitives(env, heap, intern, nullptr);

    auto t_val = expect_ok(tf::make_tensor(heap, torch::ones({2}, torch::kFloat64)));
    auto r1 = expect_ok(env.specs()[*env.lookup("torch/tensor?")].func({t_val}));
    BOOST_TEST(r1 == True);

    // A non-tensor (fixnum) should return false
    auto fixnum = expect_ok(ops::encode(int64_t(42)));
    auto r2 = expect_ok(env.specs()[*env.lookup("torch/tensor?")].func({fixnum}));
    BOOST_TEST(r2 == False);
}

// End-to-end: sequential training loop via Eta primitives

BOOST_AUTO_TEST_CASE(prim_sequential_training_converges_via_env) {
    // Pin random seed so weight initialisation is deterministic and training
    // reliably converges within the epoch budget.
    ::torch::manual_seed(42);

    Heap heap(1ull << 24);  // 16 MB — training allocates many tensors
    InternTable intern;
    BuiltinEnvironment env;
    register_torch_primitives(env, heap, intern, nullptr);

    auto mk  = [](int64_t v) { return expect_ok(ops::encode(v)); };
    auto mk_dbl = [](double v) { return expect_ok(ops::encode(v)); };

    auto lin_idx  = *env.lookup("nn/linear");
    auto relu_idx = *env.lookup("nn/relu-layer");
    auto seq_idx  = *env.lookup("nn/sequential");
    auto fwd_idx  = *env.lookup("nn/forward");
    auto adam_idx = *env.lookup("optim/adam");
    auto zg_idx   = *env.lookup("optim/zero-grad!");
    auto step_idx = *env.lookup("optim/step!");
    auto mse_idx  = *env.lookup("nn/mse-loss");
    auto bw_idx   = *env.lookup("torch/backward");
    auto item_idx = *env.lookup("torch/item");

    // Network: 1 → 8 → ReLU → 1
    auto l1  = expect_ok(env.specs()[lin_idx].func({mk(1), mk(8)}));
    auto r1  = expect_ok(env.specs()[relu_idx].func({}));
    auto l2  = expect_ok(env.specs()[lin_idx].func({mk(8), mk(1)}));
    auto net = expect_ok(env.specs()[seq_idx].func({l1, r1, l2}));

    auto opt = expect_ok(env.specs()[adam_idx].func({net, mk_dbl(0.01)}));

    auto x = expect_ok(tf::make_tensor(heap,
        torch::tensor({{1.0},{2.0},{3.0},{4.0}}, torch::kFloat64)));
    auto y = expect_ok(tf::make_tensor(heap,
        torch::tensor({{3.0},{5.0},{7.0},{9.0}}, torch::kFloat64)));

    double first_loss = 0, last_loss = 0;
    for (int epoch = 0; epoch < 300; ++epoch) {
        (void) env.specs()[zg_idx].func({opt});
        auto pred = expect_ok(env.specs()[fwd_idx].func({net, x}));
        auto loss = expect_ok(env.specs()[mse_idx].func({pred, y}));
        (void) env.specs()[bw_idx].func({loss});
        (void) env.specs()[step_idx].func({opt});
        auto lv = expect_ok(env.specs()[item_idx].func({loss}));
        double d = std::bit_cast<double>(lv);
        if (epoch == 0) first_loss = d;
        if (epoch == 299) last_loss = d;
    }
    BOOST_TEST(last_loss < first_loss);
    BOOST_TEST(last_loss < 1.0);
}

// Error paths — wrong argument types should return errors, not crash

BOOST_AUTO_TEST_CASE(prim_error_paths) {
    Heap heap(1ull << 22);
    InternTable intern;
    BuiltinEnvironment env;
    register_torch_primitives(env, heap, intern, nullptr);

    auto fixnum = expect_ok(ops::encode(int64_t(42)));

    // torch/add with non-tensor args
    auto r1 = env.specs()[*env.lookup("torch/add")].func({fixnum, fixnum});
    BOOST_TEST(!r1.has_value());

    // nn/forward with non-module first arg
    auto t_val = expect_ok(tf::make_tensor(heap, torch::ones({2}, torch::kFloat64)));
    auto r2 = env.specs()[*env.lookup("nn/forward")].func({fixnum, t_val});
    BOOST_TEST(!r2.has_value());

    // nn/forward with non-tensor second arg
    auto mk = [](int64_t v) { return expect_ok(ops::encode(v)); };
    auto lin = expect_ok(env.specs()[*env.lookup("nn/linear")].func({mk(2), mk(1)}));
    auto r3 = env.specs()[*env.lookup("nn/forward")].func({lin, fixnum});
    BOOST_TEST(!r3.has_value());

    // torch/item on non-scalar tensor
    auto multi = expect_ok(tf::make_tensor(heap, torch::ones({3}, torch::kFloat64)));
    auto r4 = env.specs()[*env.lookup("torch/item")].func({multi});
    BOOST_TEST(!r4.has_value());

    // optim/sgd with non-module
    auto r5 = env.specs()[*env.lookup("optim/sgd")].func({fixnum, expect_ok(ops::encode(0.01))});
    BOOST_TEST(!r5.has_value());

    // nn/sequential with non-module arg
    auto r6 = env.specs()[*env.lookup("nn/sequential")].func({fixnum});
    BOOST_TEST(!r6.has_value());
}

BOOST_AUTO_TEST_SUITE_END()

