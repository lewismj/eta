# Portfolio DSL Refactor Proposal

[Back to Portfolio](portfolio.md) | [Examples](examples.md) | [Fact Tables](fact-table.md)

---

## Objective

Make `examples/portfolio.eta` easier to read and maintain by introducing
small hygienic macros (`define-syntax` + `syntax-rules`) that remove
repetitive structural code while preserving current behavior.

This proposal is intentionally staged:

1. Mechanical refactors first (low risk, no semantic change).
2. Domain-facing DSL forms second (higher abstraction, controlled scope).

---

## Evaluation Summary

| Suggestion | Fit | Recommendation |
|------------|-----|----------------|
| Unified association-list construction (`dict`, `dict-from`) | Excellent | Adopt now |
| Reporting DSL for repeated `display`/`println` patterns | Strong | Adopt now (small surface) |
| Fact-table row abstraction (`with-fact-row`) | Good, with caveat | Adopt adapted form (index-based bindings) |
| Mini causal query DSL (`causal-query`) | Strong | Adopt now (thin wrapper over `do:identify-details*`) |
| Loop macros (`dotimes`, `for-range`) | Good | Adopt selectively in presentation / extraction loops |

---

## Important Constraint: Fact Table Access

Current `std.fact_table` exposes:

- `(fact-table-ref ft row-idx col-idx)`

It does **not** currently expose symbol-based lookup
`(fact-table-ref ft row-idx 'macro_growth)`.

So a safe `with-fact-row` macro should bind names to explicit numeric
column indices, not symbolic names.

---

## Proposed Macro Set (V1)

These macros are intentionally small and transparent.

```scheme
;; Association-list construction
(define-syntax dict
  (syntax-rules ()
    ((_ (k v) ...)
     (list (cons 'k v) ...))))

(define-syntax dict-from
  (syntax-rules ()
    ((_ v ...)
     (dict (v v) ...))))

;; Reporting helpers
(define-syntax report
  (syntax-rules (=>)
    ((_ label => value)
     (begin (display "  ") (display label) (display ": ") (println value)))
    ((_ line)
     (begin (display "  ") (println line)))))

;; Index-based fact row bindings (compatible with current API)
(define-syntax with-fact-row
  (syntax-rules ()
    ((_ table row ((name col) ...) body ...)
     (let ((name (fact-table-ref table row col)) ...)
       body ...))))

;; Simple index loop
(define-syntax dotimes
  (syntax-rules ()
    ((_ (i n) body ...)
     (letrec ((loop (lambda (i)
                      (when (< i n)
                        body ...
                        (loop (+ i 1))))))
       (loop 0)))))
```

---

## Mechanical Refactor Targets

### 1) Replace `(list (cons ...))` report builders

Current pattern appears repeatedly in stage reports and run artifacts.
Refactor to `dict` / `dict-from`.

Before:

```scheme
(list
  (cons 'mode mode)
  (cons 'lambda-effective lam-mode)
  (cons 'tau-effective tau-mode))
```

After:

```scheme
(dict
  (mode mode)
  (lambda-effective lam-mode)
  (tau-effective tau-mode))
```

### 2) Replace repetitive output blocks

Before:

```scheme
(display "  Distinct actions:        ")
(println distinct-actions)
```

After:

```scheme
(report "Distinct actions" => distinct-actions)
```

### 3) Replace row extraction boilerplate

Before:

```scheme
(let ((beta (fact-table-ref universe i 1))
      (macro (fact-table-ref universe i 2))
      (sent (fact-table-ref universe i 4)))
  ...)
```

After:

```scheme
(with-fact-row universe i
  ((beta 1) (macro 2) (sent 4))
  ...)
```

### 4) Replace manual counter loops where intent is index iteration

Before: repeated `letrec` loop shells.

After:

```scheme
(dotimes (i 3)
  ...)
```

---

## Causal Query DSL (Thin Layer)

Wrap the recurring `do:identify-details*` + `assoc-ref` extraction.

```scheme
(define-syntax causal-query
  (syntax-rules ()
    ((_ dag outcome treatment)
     (let ((details (do:identify-details dag 'outcome 'treatment)))
       (dict
         (status (assoc-ref 'status details))
         (chosen-z (assoc-ref 'chosen-z details))
         (formula (assoc-ref 'result details)))))
    ((_ dag outcome treatment observed)
     (let ((details (do:identify-details-observed dag 'outcome 'treatment observed)))
       (dict
         (status (assoc-ref 'status details))
         (chosen-z (assoc-ref 'chosen-z details))
         (formula (assoc-ref 'result details)))))))
```

This keeps causal semantics explicit while reducing extraction noise.

---

## Business-Facing DSL Direction (V2)

After V1 stabilizes, introduce a higher-level facade for business users.
Goal: express *intent* (policy, constraints, scenarios) in a compact form
without hiding the underlying causal/CLP mechanics.

Illustrative shape:

```scheme
(portfolio-program
  (universe universe)
  (causal-model market-dag asset_return macro_growth)
  (constraints constraint-spec)
  (objective (risk-aversion 2.0) (mode uncertainty-penalty))
  (scenarios (base 0.5) (boom 0.8) (recession 0.1) (rate-hike 0.35))
  (dynamic-control
    (horizon 8)
    (initial-state (macro 0.5) (liquidity 0.75) (crowding 0.35))
    (policy adaptive-guarded)))
```

Expected expansion target:

- `run-pipeline` for static artifact.
- `run-pipeline-dynamic` for sequential artifact.
- Existing report printers unchanged at first.

---

## Guardrails

1. Keep numerically sensitive kernels as functions, not macros.
2. Use macros for shape/boilerplate, not hidden control flow.
3. Preserve current key names in emitted artifacts for compatibility.
4. Add regression tests around report structure and key presence.

---

## Suggested Rollout Plan

1. Add V1 macros near the top of `examples/portfolio.eta` (or in a small helper module).
2. Refactor only stage report builders and printing blocks first.
3. Refactor fact-table row extraction using index-based `with-fact-row`.
4. Refactor causal metadata extraction with `causal-query`.
5. Add/adjust snapshot expectations for the emitted artifact shape.
6. Only then prototype the V2 `portfolio-program` facade.

This sequence gives maximum readability gain with minimal behavior risk.

---

## Implementation Status (2026-04-20)

Implemented in `examples/portfolio.eta`:

- `dict`
- `dict-from`
- `report`
- `dotimes`
- mechanical conversion of stage report/object builders to `dict` forms

Deferred for now:

- `with-fact-row`
- `causal-query`

Reason: current macro hygiene behavior in Eta can rewrite introduced symbols
in ways that break imported-name/key lookups for these two patterns. The
underlying functionality remains implemented with direct function calls.


