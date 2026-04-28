# CLP(R) — Constraint Logic Programming over Reals

[← Back to Language Guide](./language_guide.md)

`std.clpr` adds **continuous-domain** constraint solving to Eta's logic
engine. Where `std.clp` (CLP(FD)) reasons over integers and `std.clpb`
over booleans, `std.clpr` reasons over real intervals and posts linear
(or quadratic) constraints to a Fourier–Motzkin / QP oracle.

> **Cousins:** [`clp.md`](./reference/clp.md) (CLP(FD)), [`clpb.md`](./reference/clpb.md)
> (CLP(B)).
> **Worked example:** [`examples/portfolio-lp.eta`](../../examples/portfolio-lp.eta).

---

## Mental model

A real CLP variable has an interval domain `[lo, hi]` (closed,
half-open, or open). Posting a constraint:

1. tightens variable bounds via interval propagation, and
2. records the linear / quadratic relation in the oracle for global
   feasibility checking.

`(clp:r-minimize obj)` and `(clp:r-maximize obj)` then solve the LP;
`(clp:rq-minimize obj)` and `(clp:rq-maximize obj)` solve the QP. All
posting and propagation is undone on backtracking — the oracle hooks
into the same trail used by `std.logic`.

---

## API at a glance

### Domain constructors

| Form                                        | Domain                       |
| :------------------------------------------ | :--------------------------- |
| `(clp:real v lo hi)`                        | `v ∈ [lo, hi]` (closed)      |
| `(clp:real-open v lo hi)`                   | `v ∈ (lo, hi)`               |
| `(clp:real-half-open v lo hi lo-open?)`     | mixed bounds                 |
| `(clp:domain-r v)`                          | retrieve domain object       |

Predicates / accessors: `clp:domain-r?`, `clp:domain-r-lo`,
`clp:domain-r-hi`, `clp:domain-r-lo-open?`, `clp:domain-r-hi-open?`.

### Linear-expression constructors

```scheme
(clp:r+        e1 e2 …)        ; sum
(clp:r-        e1 e2 …)        ; difference
(clp:r*scalar  k e)            ; scalar multiply
```

These build expression trees that the constraint posting forms unfold
into the oracle's coefficient form.

### Constraint posting

| Form              | Meaning           |
| :---------------- | :---------------- |
| `(clp:r=  a b)`   | `a = b`           |
| `(clp:r<= a b)`   | `a ≤ b`           |
| `(clp:r<  a b)`   | `a < b` (strict)  |
| `(clp:r>= a b)`   | `a ≥ b`           |
| `(clp:r>  a b)`   | `a > b` (strict)  |

### Optimisation

| Form                         | Result                                                |
| :--------------------------- | :---------------------------------------------------- |
| `(clp:r-minimize  obj)`      | LP minimisation                                       |
| `(clp:r-maximize  obj)`      | LP maximisation                                       |
| `(clp:rq-minimize obj)`      | QP minimisation (objective contains `clp:r*scalar`-style quadratic terms) |
| `(clp:rq-maximize obj)`      | QP maximisation                                       |

Each returns one of:

- `#f` — infeasible
- `'clp.r.unbounded` — feasible but unbounded
- `(opt . witness)` — optimal value plus the variable bindings

### Queries

| Form                  | Returns                              |
| :-------------------- | :----------------------------------- |
| `(clp:r-bounds v)`    | Current `(lo . hi)` of variable      |
| `(clp:r-feasible?)`   | `#t` if the current store is feasible|

---

## Worked example — small LP

Maximise `3x + 5y` subject to `x + y ≤ 4`, `x ≥ 0`, `y ≥ 0`,
`y ≤ 3`:

```scheme
(import std.logic)
(import std.clpr)

(let ((x (logic-var))
      (y (logic-var)))
  (clp:real x 0 1.0e9)
  (clp:real y 0 3)
  (clp:r<= (clp:r+ x y) 4)
  (clp:r-maximize (clp:r+ (clp:r*scalar 3 x)
                          (clp:r*scalar 5 y))))
;; => (17 . ((x . 1.0) (y . 3.0)))
```

`x = 1, y = 3` saturates both `x + y ≤ 4` and `y ≤ 3`, giving the
optimum 17.

---

## Quadratic example — minimum-variance portfolio

Quadratic terms are introduced by passing a quadratic expression to
`clp:rq-minimize`. The objective minimises portfolio variance subject
to a budget constraint:

```scheme
;; minimise wᵀΣw  subject to  Σ wᵢ = 1,  wᵢ ≥ 0
(clp:r= (clp:r+ w1 w2 w3) 1)
(for-each (lambda (w) (clp:real w 0 1)) (list w1 w2 w3))
(clp:rq-minimize (portfolio-variance (list w1 w2 w3) Σ))
```

See [`examples/portfolio-lp.eta`](../../examples/portfolio-lp.eta) for
a complete run, and [`featured_examples/portfolio.md`](../featured_examples/portfolio.md)
for the larger causal-portfolio engine that combines `std.clpr` with
`std.causal` and `std.aad`.

---

## Composition with the logic engine

CLP(R) variables are ordinary logic variables with a `clp.r`
attribute. They can:

- be unified with `==` (binding to a number narrows the domain to a
  point, posting an equality);
- appear inside `findall` / `run1` searches — backtracking restores
  both the bindings and the oracle state;
- co-exist with `std.freeze` (`(freeze v thunk)`) and `dif`.

> [!IMPORTANT]
> Strict inequalities (`clp:r<`, `clp:r>`) interact with the oracle
> through epsilon perturbation and may yield witnesses that touch the
> boundary; if you need strict feasibility, post a small slack term.

---

## Related

- [`clp.md`](./reference/clp.md), [`clpb.md`](./reference/clpb.md), [`logic.md`](./reference/logic.md)
- [`featured_examples/portfolio.md`](../featured_examples/portfolio.md)
- [Eigen](./eigen.md) (the underlying numerics)

