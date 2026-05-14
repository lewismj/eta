# std.causal.mediation

[← std.causal](../causal.md) · [← Stdlib Reference](../README.md) · [Language Guide](../../../language_guide.md)

Mediation effect estimators.

```scheme
(import std.causal.mediation)
```

| Symbol | Description |
| --- | --- |
| `(mediation-formula data y x m treatment-for-y treatment-for-m)` | Cross-world mediation mean: sum over `m` of `E[Y \| X=treatment-for-y, M=m] * P(M=m \| X=treatment-for-m)`. |
| `(do:nde g y x m data x* x1)` | Natural direct effect: `E[Y(x1, M(x*))] - E[Y(x*, M(x*))]`. |
| `(do:nie g y x m data x* x1)` | Natural indirect effect: `E[Y(x1, M(x1))] - E[Y(x1, M(x*))]`. |
| `(do:cde g y x m data m-val x* x1)` | Controlled direct effect at fixed mediator value(s) `m-val`. |

---

**Source:** [`stdlib/std/causal/`](../../../../stdlib/std/causal/)

