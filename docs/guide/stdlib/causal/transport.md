# std.causal.transport

[← std.causal](../causal.md) · [← Stdlib Reference](../README.md) · [Language Guide](../../../language_guide.md)

Transportability and selection-bias helpers.

```scheme
(import std.causal.transport)
```

| Symbol | Description |
| --- | --- |
| `(s-node? n)` | True when `n` is an S-node marker (a symbol whose name starts with `S`). |
| `(do:sBD? g x y z s-nodes)` | Selection back-door criterion: `Z` satisfies the back-door for `(X, Y)` and `Y` is d-separated from each S-node given `X` and `Z`. |
| `(do:transport g* g y x)` | sID-style transport query. Returns an estimand, a transport adjustment, or a `(fail (transport ...))` witness. |

Bidirected edges are expanded to latent parents for d-separation checks.

---

**Source:** [`stdlib/std/causal/`](../../../../stdlib/std/causal/)

