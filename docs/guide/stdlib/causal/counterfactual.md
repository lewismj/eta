# std.causal.counterfactual

[← std.causal](../causal.md) · [← Stdlib Reference](../README.md) · [Language Guide](../../../language_guide.md)

Counterfactual graph transforms and queries.

```scheme
(import std.causal.counterfactual)
```

| Symbol | Description |
| --- | --- |
| `(twin-network g intervened-vars)` | Build a twin network duplicating `g` into a counterfactual world with primed names; incoming edges to `intervened-vars` are removed in the counterfactual world. |
| `(id* g gamma)` | Pragmatic ID* entry point for conjunctions of events `Y_X`. Delegates to `id`. |
| `(idc* g gamma delta)` | Pragmatic IDC* entry point for conditional counterfactuals. Delegates to `idc`. |
| `(do:ett g y x x*)` | Effect of treatment on the treated: `P(Y_x \| X = x*)`. |

---

**Source:** [`stdlib/std/causal/`](../../../../stdlib/std/causal/)

