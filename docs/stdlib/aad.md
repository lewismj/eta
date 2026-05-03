# std.aad

Helpers for tape-based reverse-mode automatic differentiation.

```scheme
(import std.aad)
```

## Gradients

| Symbol | Description |
| --- | --- |
| `(grad f xs)` | Gradient of scalar-valued `f` at point `xs`. |
| `(check-grad f xs [tol])` | Numerical vs. analytical gradient check; returns boolean. |
| `(check-grad-report f xs [tol])` | Same as above, returns a diagnostic record. |
| `(with-checkpoint thunk)` | Run `thunk` inside a tape checkpoint. |

## Differentiable primitives

These work on AAD nodes and forward to plain numbers when possible.

| Symbol | Description |
| --- | --- |
| `(ad-abs x)` | Subgradient absolute value. |
| `(ad-max x y)` | Subgradient maximum. |
| `(ad-min x y)` | Subgradient minimum. |
| `(ad-relu x)` | ReLU activation. |
| `(ad-clamp x lo hi)` | Clamp with subgradients. |
| `(softplus x)` | Smooth approximation of ReLU. |
| `(smooth-abs x epsilon)` | Smooth absolute value. |
| `(smooth-clamp x lo hi epsilon)` | Smooth clamp. |

