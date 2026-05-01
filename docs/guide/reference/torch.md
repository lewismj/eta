# Neural Networks with libtorch

[← Back to README](../../../README.md) · [AAD](aad.md) ·
[European Greeks](european.md) · [SABR](sabr.md) ·
[Modules & Stdlib](modules.md) · [Project Status](../../next-steps.md)

---

## Overview

Eta integrates with **libtorch** — the C++ backend of PyTorch — to
provide GPU-accelerated tensor operations, automatic differentiation, and
a neural-network API directly from Eta code.  The integration is
implemented as native C++ bindings exposed through the `std.torch` module,
so there is no FFI overhead or marshalling boundary beyond a thin wrapper.

```bash
etai examples/torch.eta
```

> [!NOTE]
> libtorch is a required dependency.  If not found by `find_package(Torch)`
> the build system downloads the official pre-built CPU archive automatically
> (~200 MB).  Pass `-DETA_TORCH_BACKEND=cu126` to select a CUDA variant (~2 GB).

---

## What is a Neural Network?

A neural network is a function approximator composed of **layers** of
simple operations.  Each layer applies a linear transformation followed by
a non-linear **activation function**.  By adjusting the parameters
(weights and biases) through **gradient descent**, the network learns to
map inputs to desired outputs.

```mermaid
flowchart LR
    X["Input\nx"] --> L1["Linear Layer\nw·x + b"]
    L1 --> A1["Activation\n(ReLU / σ)"]
    A1 --> L2["Linear Layer\nw·x + b"]
    L2 --> Y["Output\nŷ"]

    style X  fill:#2d2d2d,stroke:#58a6ff,color:#c9d1d9
    style L1 fill:#1a1a2e,stroke:#58a6ff,color:#c9d1d9
    style A1 fill:#16213e,stroke:#79c0ff,color:#c9d1d9
    style L2 fill:#1a1a2e,stroke:#58a6ff,color:#c9d1d9
    style Y  fill:#0f3460,stroke:#56d364,color:#c9d1d9
```

| Concept | Explanation |
|---------|-------------|
| **Tensor** | An n-dimensional array of numbers — the fundamental data structure for inputs, weights, and outputs. |
| **Linear Layer** | Computes `y = W·x + b` where **W** (weights) and **b** (bias) are learnable parameters. |
| **Activation** | A non-linear function (e.g. ReLU, sigmoid) applied element-wise, giving the network the power to learn non-linear mappings. |
| **Loss Function** | Measures how far the network's prediction is from the target (e.g. MSE, cross-entropy). |
| **Backpropagation** | Computes ∂loss/∂parameter for every parameter in one backward pass through the computation graph. |
| **Optimizer** | Updates parameters using the computed gradients (e.g. SGD, Adam). |

---

## Training Loop

Training a neural network repeats a fixed cycle until the loss converges:

```mermaid
flowchart TD
    ZG["1. Zero Gradients\n(optim-zero-grad!)"]
    FW["2. Forward Pass\n(forward model input)"]
    LS["3. Compute Loss\n(mse-loss pred target)"]
    BW["4. Backward Pass\n(backward loss)"]
    UP["5. Update Weights\n(step! optimizer)"]

    ZG --> FW --> LS --> BW --> UP
    UP -->|"next epoch"| ZG

    style ZG fill:#2d2d2d,stroke:#58a6ff,color:#c9d1d9
    style FW fill:#1a1a2e,stroke:#58a6ff,color:#c9d1d9
    style LS fill:#16213e,stroke:#79c0ff,color:#c9d1d9
    style BW fill:#0f3460,stroke:#56d364,color:#c9d1d9
    style UP fill:#0f3460,stroke:#f78166,color:#c9d1d9
```

Eta's `train-step!` helper wraps all five steps into a single call:

```scheme
(define loss (train-step! model optimizer loss-fn input target))
```

---

## Architecture — How Eta Talks to libtorch

```mermaid
flowchart TD
    ETA["Eta Program\n(import std.torch)"]
    STD["std.torch Module\n(Eta wrappers)"]
    PRIM["C++ Primitives\n(torch_primitives.h)"]
    LT["libtorch\n(ATen / Autograd / NN)"]
    CPU["CPU"]
    GPU["CUDA / ROCm\n(optional)"]

    ETA --> STD --> PRIM --> LT
    LT --> CPU
    LT --> GPU

    style ETA  fill:#2d2d2d,stroke:#58a6ff,color:#c9d1d9
    style STD  fill:#1a1a2e,stroke:#58a6ff,color:#c9d1d9
    style PRIM fill:#16213e,stroke:#79c0ff,color:#c9d1d9
    style LT   fill:#0f3460,stroke:#56d364,color:#c9d1d9
    style CPU  fill:#2d2d2d,stroke:#58a6ff,color:#c9d1d9
    style GPU  fill:#1a1a2e,stroke:#f78166,color:#c9d1d9
```

Three new heap object kinds manage native libtorch resources through the
garbage collector:

| Heap Kind | Wraps | GC Behaviour |
|-----------|-------|--------------|
| **`TensorPtr`** | `torch::Tensor` | Reference-counted via libtorch's `intrusive_ptr`; destructor releases storage. |
| **`NNModulePtr`** | `torch::nn::Module` | Shared pointer to the module; destructor releases parameters. |
| **`OptimizerPtr`** | `torch::optim::Optimizer` | Shared pointer to the optimizer state. |

---

## Quick Start — Tensor Basics

```scheme
(module tensor-demo
  (import std.core)
  (import std.io)
  (import std.torch)
  (begin
    ;; Create tensors
    (define a (ones '(3)))           ;; [1, 1, 1]
    (define b (from-list '(2.0 3.0 4.0)))

    ;; Arithmetic
    (tensor-print (t+ a b))         ;; [3, 4, 5]
    (tensor-print (t* a b))         ;; [2, 3, 4]

    ;; Reductions
    (println (item (tsum b)))       ;; 9.0
    (println (item (mean b)))       ;; 3.0

    ;; Matrix multiply
    (define m1 (ones '(2 3)))
    (define m2 (ones '(3 4)))
    (println (shape (matmul m1 m2)))  ;; (2 4)
  ))
```

---

## Autograd — Automatic Differentiation

libtorch's autograd engine records operations on tensors that have
`requires-grad` enabled, building a computation graph that is traversed
in reverse by `backward` to compute gradients.

```scheme
;; y = x² at x = 3  →  dy/dx = 2x = 6
(define x (from-list '(3.0)))
(requires-grad! x #t)
(define y (t* x x))
(backward y)
(println (item (grad x)))          ;; 6.0
```

```scheme
;; Multivariate: f(x,y) = x·y at (2, 3)
;;   → ∂f/∂x = 3,  ∂f/∂y = 2
(define xa (from-list '(2.0)))
(define ya (from-list '(3.0)))
(requires-grad! xa #t)
(requires-grad! ya #t)
(backward (t* xa ya))
(println (item (grad xa)))         ;; 3.0
(println (item (grad ya)))         ;; 2.0
```

---

## Building a Neural Network

### Single Linear Layer

```scheme
(define layer (linear 4 2))       ;; 4 inputs → 2 outputs
(println (module? layer))          ;; #t
(println (length (parameters layer)))  ;; 2  (weight + bias)

(define out (forward layer (randn '(1 4))))
(println (shape out))              ;; (1 2)
```

### Multi-Layer Network

Stack layers with `sequential`:

```scheme
(define net (sequential
              (linear 4 8)
              (relu-layer)
              (linear 8 2)))

(define out (forward net (randn '(1 4))))
(println (shape out))              ;; (1 2)
```

This builds the following network:

```mermaid
flowchart LR
    IN["Input\n(1 × 4)"] --> L1["Linear\n4 → 8"]
    L1 --> R["ReLU"]
    R --> L2["Linear\n8 → 2"]
    L2 --> OUT["Output\n(1 × 2)"]

    style IN  fill:#2d2d2d,stroke:#58a6ff,color:#c9d1d9
    style L1  fill:#1a1a2e,stroke:#58a6ff,color:#c9d1d9
    style R   fill:#16213e,stroke:#79c0ff,color:#c9d1d9
    style L2  fill:#1a1a2e,stroke:#58a6ff,color:#c9d1d9
    style OUT fill:#0f3460,stroke:#56d364,color:#c9d1d9
```

---

## Complete Training Example — Learning y = 2x + 1

This example trains a single linear layer to approximate the function
**y = 2x + 1** using gradient descent with MSE loss.

```scheme
(module train-demo
  (import std.core)
  (import std.io)
  (import std.torch)
  (begin
    ;; ── Training data ────────────────────────────────────────────
    (define train-x (reshape (from-list '(1.0 2.0 3.0 4.0 5.0)) '(5 1)))
    (define train-y (reshape (from-list '(3.0 5.0 7.0 9.0 11.0)) '(5 1)))

    ;; ── Model: y ≈ w·x + b ──────────────────────────────────────
    (define model (linear 1 1))
    (define opt   (sgd model 0.01))

    ;; ── Training loop ────────────────────────────────────────────
    (display "epoch |   loss\n")
    (display "------+-----------\n")
    (letrec ((loop (lambda (epoch)
               (if (> epoch 200) 'done
                   (let ((loss (train-step! model opt mse-loss
                                            train-x train-y)))
                     (if (= (modulo epoch 50) 0)
                         (begin (display epoch)
                                (display "   |  ")
                                (println loss)))
                     (loop (+ epoch 1)))))))
      (loop 1))

    ;; ── Test predictions ─────────────────────────────────────────
    (define test-x (reshape (from-list '(6.0 7.0 10.0)) '(3 1)))
    (println (to-list (reshape (forward model test-x) '(3))))
    ;; Expected: values close to (13.0 15.0 21.0)
  ))
```

**What happens during training:**

```mermaid
sequenceDiagram
    participant TL as Training Loop
    participant Optim as Optimizer (SGD)
    participant Model as Linear(1,1)
    participant AG as Autograd

    TL->>Optim: zero gradients
    TL->>Model: forward(train-x) → pred
    TL->>TL: mse-loss(pred, train-y) → loss
    TL->>AG: backward(loss)
    AG-->>Model: ∂loss/∂w, ∂loss/∂b
    TL->>Optim: step! (w -= lr·∂loss/∂w)
    Note over TL: Repeat for each epoch
```

---

## Optimizers

Eta exposes two optimizers from libtorch:

| Optimizer | Constructor | Description |
|-----------|------------|-------------|
| **SGD** | `(sgd model lr)` | Stochastic gradient descent — simple, robust, well-understood. |
| **Adam** | `(adam model lr)` | Adaptive moment estimation — maintains per-parameter learning rates; often converges faster. |

```scheme
;; SGD with learning rate 0.01
(define opt (sgd model 0.01))

;; Adam with learning rate 0.1
(define opt (adam model 0.1))
```

Both optimizers support the same interface:

```scheme
(optim-zero-grad! opt)   ;; clear accumulated gradients
(step! opt)              ;; apply one parameter update
(optimizer? opt)         ;; → #t
```

---

## Loss Functions

| Function | Eta Name | Formula |
|----------|----------|---------|
| **Mean Squared Error** | `mse-loss` | `(1/n) Σ (pred − target)²` |
| **L1 (Mean Absolute Error)** | `l1-loss` | `(1/n) Σ \|pred − target\|` |
| **Cross-Entropy** | `cross-entropy-loss` | `−Σ target · log(pred)` |

```scheme
(define pred   (from-list '(1.0 2.0 3.0)))
(define target (from-list '(2.0 2.0 2.0)))
(println (item (mse-loss pred target)))   ;; 0.666…
(println (item (l1-loss pred target)))    ;; 0.666…
```

---

## Device Management (GPU)

Tensors and modules can be moved between CPU and GPU:

```scheme
(println (gpu-available?))       ;; #t if CUDA is present
(println (gpu-count))            ;; number of GPUs

;; Move a tensor to GPU and back
(define t (ones '(3 3)))
(define t-gpu (to-gpu t))
(println (device t-gpu))         ;; "cuda:0"
(define t-cpu (to-cpu t-gpu))

;; Move an entire model to GPU
(nn-to-gpu model)
```

---

## `std.torch` API Reference

### Tensor Creation

| Function | Signature | Description |
|----------|-----------|-------------|
| `tensor` | `(val [requires-grad?])` | Scalar or list → tensor |
| `ones` | `(shape)` | Tensor filled with 1.0 |
| `zeros` | `(shape)` | Tensor filled with 0.0 |
| `randn` | `(shape)` | Tensor from normal distribution |
| `manual-seed` | `(seed)` | Set torch RNG seed for reproducible sampling |
| `arange` | `(start end step)` | Range tensor |
| `linspace` | `(start end n)` | Evenly spaced points |
| `fact-table->tensor` | `(ft col-idxs)` | Convert live fact-table rows and selected columns to a `float64` tensor |
| `from-list` | `(list)` | Flat list → 1-D tensor |

### Arithmetic

| Function | Description |
|----------|-------------|
| `t+`, `t-`, `t*`, `t/` | Element-wise arithmetic |
| `matmul` | Matrix multiplication |
| `dot` | Dot product (flattened) |

### Linear Algebra and Sampling

| Function | Signature | Description |
|----------|-----------|-------------|
| `cholesky` | `(cov)` | Cholesky factor of a square SPD covariance matrix |
| `matrix-exp` | `(t)` | Matrix exponential of a square matrix tensor |
| `mvnormal` | `(mean cov)` | Draw one sample from `N(mean, cov)` |

### Unary Operations

| Function | Description |
|----------|-------------|
| `neg`, `tabs` | Negation, absolute value |
| `texp`, `tlog`, `tsqrt` | exp, log, √ |
| `relu`, `sigmoid`, `ttanh` | Activation functions |
| `softmax` | `(t dim)` — softmax along dimension |

### Shape Operations

| Function | Signature | Description |
|----------|-----------|-------------|
| `shape` | `(t)` | Shape as a list of fixnums |
| `reshape` | `(t shape)` | Reshape tensor |
| `transpose` | `(t d0 d1)` | Swap two dimensions |
| `squeeze` / `unsqueeze` | `(t [dim])` | Remove / add size-1 dimension |
| `cat` | `(tensor-list dim)` | Concatenate along dimension |

### Reductions

| Function | Description |
|----------|-------------|
| `column-l2-norm` / `torch:column-l2-norm` | `(t axis)` — L2 norm along the selected axis |
| `tsum`, `mean` | Sum, mean of all elements |
| `tmax`, `tmin` | Maximum, minimum |
| `argmax`, `argmin` | Index of max / min |

### Conversion

| Function | Description |
|----------|-------------|
| `item` | Scalar tensor → Eta number |
| `to-list` | Flat tensor → Eta list |
| `numel` | Number of elements |

### Autograd

| Function | Description |
|----------|-------------|
| `requires-grad!` | `(t flag)` — enable/disable gradient tracking |
| `requires-grad?` | `(t)` — query gradient tracking |
| `detach` | `(t)` — detach from computation graph |
| `backward` | `(t)` — run backpropagation |
| `grad` | `(t)` — read accumulated gradient |
| `zero-grad!` | `(t)` — reset gradient to zero |

### Neural Network Layers

| Function | Description |
|----------|-------------|
| `linear` | `(in out)` — fully connected layer |
| `sequential` | `(layer ...)` — stack layers |
| `relu-layer` | `()` — ReLU activation module |
| `sigmoid-layer` | `()` — Sigmoid activation module |
| `dropout` | `(rate)` — dropout regularisation |
| `forward` | `(module tensor)` — forward pass |
| `parameters` | `(module)` — list of parameter tensors |
| `train!` / `eval!` | `(module)` — set training / evaluation mode |

### Serialization

| Function | Description |
|----------|-------------|
| `tensor-save` | `(t path)` — save tensor to file |
| `tensor-load` | `(path)` — load tensor from file |
| `tensor-print` | `(t)` — display tensor to stdout |

---

## Building with Torch Support

### CMake Options

```bash
cmake -B build                                       # CPU-only (auto-downloads libtorch)
cmake -B build -DETA_TORCH_BACKEND=cu126             # CUDA 12.6
```

| Option | Default | Description |
|--------|---------|-------------|
| `ETA_TORCH_BACKEND` | `cpu` | libtorch variant: `cpu`, `cu126`, `cu128`, or `cu130` |
| `ETA_LIBTORCH_VER` | `2.11.0` | libtorch version to download |

libtorch is downloaded automatically if not found by `find_package(Torch)`.
The build system fetches the official pre-built archive from
`download.pytorch.org`.  The download is cached in the build directory so
re-configures do not re-download.

### Manual libtorch

If you prefer to manage libtorch yourself:

```bash
cmake -B build -DTorch_DIR=/path/to/libtorch/share/cmake/Torch
```

---

## Test Suite

The `examples/torch_tests/` directory contains a comprehensive integration
test suite that exercises every `std.torch` primitive through the full Eta
pipeline (lex → parse → expand → link → emit → execute).  Each file is a
standalone module that uses the `std.test` framework and prints a
pass/fail summary.

| File | Coverage |
|------|----------|
| [`tensor_creation.eta`](../../../examples/torch_tests/tensor_creation.eta) | `tensor`, `ones`, `zeros`, `randn`, `arange`, `linspace`, `from-list`, `tensor?` |
| [`arithmetic.eta`](../../../examples/torch_tests/arithmetic.eta) | `t+`, `t-`, `t*`, `t/`, `matmul`, `dot`, `neg`, `tabs`, `texp`, `tlog`, `tsqrt`, `relu`, `sigmoid`, `ttanh`, `softmax` |
| [`shape_ops.eta`](../../../examples/torch_tests/shape_ops.eta) | `shape`, `reshape`, `transpose`, `squeeze`, `unsqueeze`, `cat`, `numel` |
| [`reductions.eta`](../../../examples/torch_tests/reductions.eta) | `tsum`, `mean`, `tmax`, `tmin`, `argmax`, `argmin` |
| [`autograd.eta`](../../../examples/torch_tests/autograd.eta) | `requires-grad!`, `requires-grad?`, `backward`, `grad`, `zero-grad!`, `detach` |
| [`nn_layers.eta`](../../../examples/torch_tests/nn_layers.eta) | `linear`, `sequential`, `relu-layer`, `sigmoid-layer`, `dropout`, `forward`, `parameters`, `module?`, `train!`, `eval!` |
| [`loss_functions.eta`](../../../examples/torch_tests/loss_functions.eta) | `mse-loss`, `l1-loss` |
| [`optimizers.eta`](../../../examples/torch_tests/optimizers.eta) | `sgd`, `adam`, `step!`, `optim-zero-grad!`, `optimizer?` |
| [`device_info.eta`](../../../examples/torch_tests/device_info.eta) | `gpu-available?`, `gpu-count`, `device`, `to-device`, `to-cpu` |
| [`training.eta`](../../../examples/torch_tests/training.eta) | `train-step!`, SGD/Adam convergence, sequential network training |

The stdlib test suite also includes [`stdlib/tests/torch.test.eta`](../../../stdlib/tests/torch.test.eta),
which validates `cholesky` and `mvnormal` via the Eta test runner.

### Running the tests

Run all torch tests at once:

```bash
# All tests (Linux / macOS)
for f in examples/torch_tests/*.eta; do echo "── $f"; etai "$f"; done

# All tests (Windows PowerShell)
Get-ChildItem examples\torch_tests\*.eta | ForEach-Object { Write-Host "── $_"; etai $_ }
```

Run a single test file:

```bash
etai examples/torch_tests/tensor_creation.eta
```

The C++ unit tests in `eta/test/src/torch_tests.cpp` mirror these Eta tests
at the primitive level, verifying heap lifecycle, GC behavior, error paths,
and end-to-end training convergence directly against the C++ API.

```bash
# Run C++ torch tests
ctest --test-dir build -R torch
```

---

## Implementation Notes

The integration is structured as three layers:

1. **`eta/torch/`** — C++ header-only library
   ([`torch_primitives.h`](../../../eta/torch/src/eta/torch/torch_primitives.h))
   that registers ~60 builtins with the VM's `BuiltinEnvironment`.

2. **`stdlib/std/torch.eta`** — Eta wrapper module that re-exports the
   builtins under clean names (`t+`, `matmul`, `forward`, etc.) and
   provides the `train-step!` helper.

3. **`examples/torch.eta`** — End-to-end example covering tensor creation,
   autograd, network construction, and training with both SGD and Adam.

4. **`examples/torch_tests/`** — 10-file integration test suite covering
   every exported function in `std.torch`.

The C++ primitives handle type marshalling between NaN-boxed `LispVal`
values and libtorch's `torch::Tensor` / `torch::nn::Module` /
`torch::optim::Optimizer` types through three dedicated heap object kinds
(`TensorPtr`, `NNModulePtr`, `OptimizerPtr`), each with a destructor that
runs when the GC collects the wrapper.

