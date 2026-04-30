# Counterfactual Inference - `std.causal.counterfactual`

[<- Causal Reference](./causal.md)

This module adds pragmatic counterfactual APIs on top of Eta's ADMG
identification engine.

## Import

```scheme
(import std.causal.counterfactual)
```

## API

| Function | Description |
| -------- | ----------- |
| `(twin-network g intervened-vars)` | Duplicates `g` into a counterfactual world with primed node names and intervention-aware edge surgery |
| `(id* g gamma)` | Counterfactual identification entry point for conjunctions of `Y_X` events |
| `(idc* g gamma delta)` | Conditional counterfactual identification entry point |
| `(do:ett g y x x*)` | Effect of treatment on the treated query `P(Y_x | X=x*)` |

## Event shapes

`gamma` is a list of counterfactual events:

```scheme
'((y x))            ; Y_x
'((y (x z)))        ; Y_{x,z}
```

`delta` is a list of conditioning assignments where the first element is
the variable name:

```scheme
'((x 0))
'((x 0) (z 1))
```

## Example

```scheme
(define g '((x -> m) (m -> y) (x <-> y)))

;; Counterfactual identification
(id* g '((y x)))

;; Effect of treatment on the treated
(do:ett g 'y 'x 0)
```

## Notes

- `id*` and `idc*` normalize counterfactual syntax, then delegate to
  `std.causal.identify` (`id` and `idc`) for estimand construction.
- `twin-network` removes incoming causal mechanisms for intervened
  variables in the counterfactual copy and keeps cross-world latent
  links for non-intervened variables.
