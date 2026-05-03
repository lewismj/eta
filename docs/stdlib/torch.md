# std.torch

libtorch bindings: tensors, neural-network modules, optimizers, and
device management. Available only when Eta is built with
`-DETA_BUILD_TORCH=ON`.

```scheme
(import std.torch)
```

## Tensor construction

| Symbol | Description |
| --- | --- |
| `(tensor data shape)` | Build a tensor from nested data. |
| `(ones shape)` | Tensor of ones. |
| `(zeros shape)` | Tensor of zeros. |
| `(randn shape)` | Standard-normal samples. |
| `(arange start end [step])` | 1-D range tensor. |
| `(linspace start end n)` | Linearly spaced 1-D tensor. |
| `(from-list xs)` | Build a tensor from a flat list. |
| `(manual-seed n)` | Set the global RNG seed. |
| `(tensor? x)` | True when `x` is a tensor. |

## Element-wise arithmetic

| Symbol | Description |
| --- | --- |
| `(t+ a b)` | Addition. |
| `(t- a b)` | Subtraction. |
| `(t* a b)` | Hadamard product. |
| `(t/ a b)` | Element-wise division. |
| `(neg a)` | Negation. |
| `(tabs a)` | Absolute value. |
| `(texp a)` | Exponential. |
| `(tlog a)` | Natural log. |
| `(tsqrt a)` | Square root. |

## Linear algebra

| Symbol | Description |
| --- | --- |
| `(matmul a b)` | Matrix multiplication. |
| `(dot a b)` | Dot product. |
| `(cholesky a)` | Cholesky factor. |
| `(matrix-exp a)` | Matrix exponential. |
| `(mvnormal mean cov n)` | Sample multivariate normals. |
| `(column-l2-norm m)` | L2 norm per column. |

`torch:column-l2-norm` is exported as a long-name alias.

## Activations

| Symbol | Description |
| --- | --- |
| `(relu x)` | ReLU. |
| `(sigmoid x)` | Sigmoid. |
| `(ttanh x)` | Hyperbolic tangent. |
| `(softmax x dim)` | Softmax along dimension `dim`. |

## Shape

| Symbol | Description |
| --- | --- |
| `(shape t)` | Shape as a list. |
| `(reshape t shape)` | Reshape (no copy when possible). |
| `(transpose t dim0 dim1)` | Swap two dimensions. |
| `(squeeze t [dim])` | Drop unit dimensions. |
| `(unsqueeze t dim)` | Insert a unit dimension. |
| `(cat tensors dim)` | Concatenate along `dim`. |
| `(fact-table->tensor table cols)` | Build a tensor from selected columns. |

## Reductions

| Symbol | Description |
| --- | --- |
| `(tsum t [dim])` | Sum (full or per dim). |
| `(mean t [dim])` | Mean. |
| `(tmax t [dim])` | Max. |
| `(tmin t [dim])` | Min. |
| `(argmax t [dim])` | Index of maximum. |
| `(argmin t [dim])` | Index of minimum. |
| `(item t)` | Scalar value of a 0-d tensor. |
| `(to-list t)` | Convert tensor to nested lists. |
| `(numel t)` | Number of elements. |

## Autograd

| Symbol | Description |
| --- | --- |
| `(requires-grad! t flag)` | Toggle gradient tracking. |
| `(requires-grad? t)` | Read the flag. |
| `(detach t)` | Detach from the autograd graph. |
| `(backward loss)` | Backpropagate from a scalar loss. |
| `(grad t)` | Gradient tensor accumulated on `t`. |
| `(zero-grad! t)` | Clear gradient. |

## Modules

| Symbol | Description |
| --- | --- |
| `(linear in-features out-features)` | Linear layer. |
| `(sequential layers)` | Chain modules. |
| `(relu-layer)` | ReLU as a module. |
| `(sigmoid-layer)` | Sigmoid as a module. |
| `(dropout p)` | Dropout layer. |
| `(forward module input)` | Run the forward pass. |
| `(parameters module)` | List of trainable parameters. |
| `(train! module)` | Set training mode. |
| `(eval! module)` | Set eval mode. |
| `(module? x)` | True when `x` is a module. |

## Losses

| Symbol | Description |
| --- | --- |
| `(mse-loss pred target)` | Mean-squared error. |
| `(l1-loss pred target)` | Mean absolute error. |
| `(cross-entropy-loss logits targets)` | Cross-entropy. |

## Optimizers

| Symbol | Description |
| --- | --- |
| `(sgd params lr . opts)` | Build an SGD optimizer. |
| `(adam params lr . opts)` | Build an Adam optimizer. |
| `(step! optimizer)` | Apply one optimizer step. |
| `(optim-zero-grad! optimizer)` | Zero parameter gradients. |
| `(optimizer? x)` | True when `x` is an optimizer. |
| `(train-step! optimizer loss-thunk)` | Convenience: zero, forward+backward, step. |

## Persistence

| Symbol | Description |
| --- | --- |
| `(tensor-save t path)` | Save a tensor to a file. |
| `(tensor-load path)` | Load a tensor from a file. |
| `(tensor-print t)` | Print a tensor. |

## Devices

| Symbol | Description |
| --- | --- |
| `(gpu-available?)` | True when CUDA is available. |
| `(gpu-count)` | Number of CUDA devices. |
| `(device t)` | Device that holds `t`. |
| `(to-device t device)` | Move tensor to a named device. |
| `(to-gpu t)` | Move tensor to GPU 0. |
| `(to-cpu t)` | Move tensor to CPU. |
| `(nn-to-device m device)` | Move a module to a device. |
| `(nn-to-gpu m)` | Move a module to GPU 0. |
| `(nn-to-cpu m)` | Move a module to CPU. |

