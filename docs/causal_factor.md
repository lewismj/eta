# End-to-End Causal Pipeline — Symbolic → Causal → Logic → Neural

[← Back to README](../README.md) · [Examples](examples.md) ·
[Causal](causal.md) · [Logic](logic.md) · [CLP](clp.md) ·
[AAD](aad.md) · [Torch](torch.md)

---

## Overview

[`examples/causal_demo.eta`](../examples/causal_demo.eta) demonstrates an
end-to-end causal inference pipeline, combining symbolic algebra, causal
reasoning, logic search, and neural estimation in a single program.
Rather than isolated features, each stage feeds the next — illustrating
how the language supports composable reasoning across paradigms.

This example serves as a **reference pipeline**, showing how multiple
paradigms interoperate within a single Eta program.

| Section | Pillar               | What it does |
|---------|----------------------|--------------|
| **§1** | Symbolic Processing  | Define a factor model as S-expressions, differentiate it, simplify with fixed-point rewriting |
| **§2** | Causal Reasoning     | Encode a DAG of market variables; derive the back-door adjustment formula with `do:identify` |
| **§3** | Logic & CLP          | Use `findall` + backtracking to discover all valid adjustment sets; validate with `clp(Z)` domains |
| **§4** | libTorch Integration | Train a neural network to learn E[return \| beta, sector] |
| **§5** | Neural Causal Estimation (ATE) | Plug NN predictions into the causal formula to compute the Average Treatment Effect |


---

## Running the Example 

The example requires a release bundle with **torch support**.

### Compile & run (recommended — better performance)

```console
etac -O examples/causal_demo.eta 
etai causal_demo.etac
```

`etac -O` runs the full compilation pipeline with optimization passes
(constant folding, dead code elimination) and serializes compact bytecode.
`etai` then loads the `.etac` directly — skipping lex, parse, expand,
link, and analysis — for faster startup.

### Interpret directly

```console
etai examples/causal_demo.eta
```

---

## The Question

> *"Does increasing a portfolio's market-beta exposure causally increase
>  returns, or is the relationship driven by sector composition?"*

A naive regression of `stock-return` on `market-beta` conflates two
paths:

```
sector ──→ market-beta ──→ stock-return     (causal path)
sector ──────────────────→ stock-return     (direct sector effect)
```

`sector` is a **confounder** — it influences both the exposure and the
outcome.  The example uses Pearl's do-calculus to derive the correct
adjustment, logic search to validate it, and a neural network to
estimate the causal effect numerically.

---

## §1 — Symbolic Factor Model & Differentiation

### What it does

The factor model is defined as a quoted S-expression tree:

```scheme
(define factor-model
  '(+ (+ (+ alpha (* beta market))
          (* gamma sector))
      (* delta (* beta sector))))
```

This represents the structural equation:

```
return = α + β·market + γ·sector + δ·β·sector
```

A self-contained symbolic differentiator computes `∂return/∂beta` and
`∂return/∂sector` using the sum and product rules.  Three small
helpers destructure any S-expression node into its operator and
operands:

```scheme
(defun op (e) (car e))
(defun a1 (e) (car (cdr e)))
(defun a2 (e) (car (cdr (cdr e))))
```

The **differentiator** pattern-matches on `+` and `*` nodes,
applying the sum rule `d(a+b) = da + db` and the product rule
`d(a·b) = da·b + a·db`:

```scheme
(defun diff (e v)
  (cond
    ((number? e) 0)
    ((symbol? e) (if (eq? e v) 1 0))
    ((eq? (op e) '+)
      (list '+ (diff (a1 e) v) (diff (a2 e) v)))
    ((eq? (op e) '*)
      (list '+ (list '* (diff (a1 e) v) (a2 e))
               (list '* (a1 e) (diff (a2 e) v))))
    (#t 0)))
```

The **algebraic simplifier** eliminates identity elements (`0 + x → x`,
`1 * x → x`) and folds constants (`3 + 4 → 7`):

```scheme
(defun simplify (e)
  (cond
    ((atom? e) e)
    ((eq? (op e) '+)
      (let ((sa (simplify (a1 e))) (sb (simplify (a2 e))))
        (cond
          ((and (number? sa) (= sa 0)) sb)
          ((and (number? sb) (= sb 0)) sa)
          ((and (number? sa) (number? sb)) (+ sa sb))
          (#t (list '+ sa sb)))))
    ((eq? (op e) '*)
      (let ((sa (simplify (a1 e))) (sb (simplify (a2 e))))
        (cond
          ((and (number? sa) (= sa 0)) 0)
          ((and (number? sb) (= sb 0)) 0)
          ((and (number? sa) (= sa 1)) sb)
          ((and (number? sb) (= sb 1)) sa)
          ((and (number? sa) (number? sb)) (* sa sb))
          (#t (list '* sa sb)))))
    (#t e)))
```

`simplify*` wraps the one-step simplifier in a **fixed-point loop** —
keep simplifying until the expression is stable:

```scheme
(defun simplify* (e)
  (let ((s (simplify e)))
    (if (equal? s e) s (simplify* s))))
```

The convenience function `D` composes differentiation with
simplification, and the two sensitivities are computed:

```scheme
(defun D (expr var) (simplify* (diff expr var)))

(define dR/dbeta   (D factor-model 'beta))
(define dR/dsector (D factor-model 'sector))
```

### Expected output

```
  Symbolic sensitivities:
    dReturn/dBeta   = (+ market (* delta sector))
    dReturn/dSector = (+ gamma (* delta beta))

  Insight: the beta sensitivity depends on sector (interaction term)
  → confounding! Motivates causal analysis in §2.
```

The key insight is that `∂return/∂beta` contains `sector` — the beta
sensitivity is **not constant** across sectors.  This interaction term
is exactly the confounding that motivates causal analysis.

### How the VM handles it

*Under the hood:*

The factor model is a **quoted list** — the compiler emits `LoadConst`
instructions that push NaN-boxed `ConsPtr` heap objects.  No
evaluation happens until `diff` pattern-matches on the tree by calling
`car`, `cdr`, `eq?` (which compile to `Car`, `Cdr`, `Eq` opcodes).

`simplify*` is a **fixed-point loop**: it calls `simplify`, compares
the result with `equal?` (deep structural equality, O(n) tree walk),
and loops until no further reductions fire.  The compiler recognises
the self-tail-call and emits a `Jump` back to the function entry
instead of a `Call` + `Return` pair — so the fixed-point iteration
runs in constant stack space.

---

## §2 — Causal DAG & Do-Calculus Identification

### What it does

Encode the causal structure as a DAG in `std.causal`'s edge-list
format:

```scheme
(define finance-dag
  '((sector      -> market-beta)
    (sector      -> stock-return)
    (market-beta -> stock-return)))
```

Then invoke the do-calculus engine:

```scheme
(define formula (do:identify finance-dag 'stock-return 'market-beta))
```

`do:identify` searches for a minimal back-door adjustment set Z ⊆
non-descendants(X) that satisfies the back-door criterion:

1. **No descendant condition** — no member of Z is a descendant of X.
2. **Blocking condition** — Z blocks all back-door paths from X to Y.

The result is converted to a human-readable string, and the raw
adjustment set is extracted for use in §3:

```scheme
(define formula-str
  (do:adjustment-formula->string formula 'stock-return 'market-beta))

(define z-set
  (if (and (pair? formula) (eq? (car formula) 'adjust))
      (car (cdr formula))
      '()))
```

### Expected output

```
  Query:   P(stock-return | do(market-beta))
  Result:  P(stock-return | do(market-beta)) = Σ_{sector} P(stock-return | market-beta, sector) · P(sector)
  Adjustment set Z = (sector)
```

The formula tells us: to estimate the causal effect of beta on returns,
we must **stratify by sector** — computing `E[return | beta, sector]`
within each sector and then averaging over the sector distribution.

### How the VM handles it

*Under the hood:*

The DAG is a list of 3-element lists `(from -> to)` — plain heap
`ConsPtr` chains.  `do:identify` is a pure Eta function that:

1. Calls `dag:parents` to find parents of X (= `(sector)`).
2. Calls `dag:non-descendants` to enumerate candidate adjustment
   nodes.
3. Tries each candidate set with `dag:satisfies-backdoor?`, which
   uses `dag:has-path?` — a DFS reachability test that checks
   whether a directed path exists from a source to a target while
   avoiding forbidden nodes.

All graph traversal is implemented as recursive Eta functions over
lists — no FFI or special VM support.  The VM's tail-call optimization
keeps the DFS stack bounded.

---

## §3 — Logic Search & CLP Validation

### What it does

**Logic search with `findall`:** instead of relying on the single
adjustment set returned by `do:identify`, we use `findall` with
backtracking to **exhaustively enumerate** all valid back-door
adjustment sets.

First, the candidate adjustment sets are built from the DAG's
non-descendants of X:

```scheme
(define all-nodes (dag:nodes finance-dag))
(define non-desc  (dag:non-descendants finance-dag 'market-beta))

(define candidates
  (map* (lambda (n) (list n)) non-desc))
```

Then `findall` tests each candidate against the back-door criterion:

```scheme
(let* ((z  (logic-var))
       (valid-sets
         (findall
           (lambda () (deref-lvar z))
           (map* (lambda (cand)
                   (lambda ()
                     (and (== z cand)
                          (dag:satisfies-backdoor?
                            finance-dag 'market-beta 'stock-return cand))))
                 candidates))))
  (println valid-sets))
```

Each candidate is a singleton set drawn from the non-descendants of X.
`findall` iterates over the branch list: for each branch it snapshots
the trail (`trail-mark`), attempts unification (`== z cand`), runs the
back-door check, records the result if successful, then **unwinds the
trail** — restoring `z` to its unbound state for the next candidate.

**CLP validation:** the sector weights in the adjustment formula are
probabilities, so each must lie in (0, 1).  We express this as a
`clp(Z)` domain constraint on integer percentages.  The `unify`
calls commit the concrete weights only if they satisfy the domain:

```scheme
(let* ((w-tech    (logic-var))
       (w-energy  (logic-var))
       (w-finance (logic-var)))
  (clp:domain w-tech    1 99)
  (clp:domain w-energy  1 99)
  (clp:domain w-finance 1 99)
  (if (and (unify w-tech 33)
           (unify w-energy 33)
           (unify w-finance 34))
      (display "✓ All weights are valid probabilities in (0,1)\n")
      (display "✗ Domain violation\n")))
```

If any weight fell outside [1, 99] — e.g. a degenerate stratum with
0% or 100% — the VM's forward-checker would reject the unification
immediately.

### Expected output

```
  Valid back-door adjustment sets (via findall):
    ((sector))

  CLP validation of adjustment weights:
    P(tech)=33%, P(energy)=33%, P(finance)=34%
    ✓ All weights are valid probabilities in (0,1)
```

### How the VM handles it

*Under the hood:*

**Logic variables** are heap-allocated objects with tag `LogicVar`.
`(logic-var)` compiles to the `MakeLogicVar` opcode, which allocates
a fresh cell on the GC heap.

**Unification** (`== z cand`) compiles to the `Unify` opcode.  The
VM's `unify()` method performs Robinson's structural unification:
- Walk both sides to their root (pointer chasing through bound vars).
- If one side is unbound, bind it and **push the binding onto the
  trail** (a stack of `(variable, old-value)` pairs).
- If both sides are ground, test structural equality.

**Trail management:** `(trail-mark)` → `TrailMark` opcode — pushes the
current trail stack pointer.  `(unwind-trail mark)` → `UnwindTrail`
opcode — pops and unbinds every trail entry back to the saved mark.
This gives `findall` its backtracking: each branch is tried in
isolation.

**CLP forward checking:** `(clp:domain w-tech 1 99)` calls the C++
primitive `%clp-domain-z!`, which attaches a `ConstraintStore::Domain`
to the logic variable.  When `(unify w-tech 33)` fires, the VM's
`unify()` method checks the domain **before** committing the binding:
if the ground value 33 is outside [1, 99], unification returns `#f`
immediately — no search is wasted.

---

## §4 — Neural Network Training (libtorch)

### What it does

Train a neural network to learn the conditional expectation
`E[return | beta, sector]` — the function that the back-door formula
requires.

**Data:** 60 observations (20 per sector) generated from a known DGP:

```
return = 1.5·beta + 0.8·sector_code + 0.3·beta·sector_code + noise
```

Sector encoding: tech = 1.0, energy = 0.0, finance = −1.0.

Each row in the training set is a 3-element list
`(beta  sector_code  return)`.  A representative slice:

```scheme
(define train-data
  (list
    ;; Technology (sector_code = 1.0) — DGP: return = 1.8*beta + 0.8
    '(1.10  1.0  2.81) '(1.10  1.0  2.76) '(1.15  1.0  2.88) '(1.15  1.0  2.93)
    '(1.20  1.0  2.99) '(1.20  1.0  2.93) ...
    ;; Energy (sector_code = 0.0)   — DGP: return = 1.5*beta
    '(0.70  0.0  1.08) '(0.70  0.0  1.02) '(0.75  0.0  1.15) '(0.75  0.0  1.10)
    '(0.80  0.0  1.23) '(0.80  0.0  1.17) ...
    ;; Finance (sector_code = -1.0) — DGP: return = 1.2*beta - 0.8
    '(0.90 -1.0  0.31) '(0.90 -1.0  0.25) '(0.95 -1.0  0.37) '(0.95 -1.0  0.31)
    '(1.00 -1.0  0.43) '(1.00 -1.0  0.37) ...))
```

The raw list-of-lists is reshaped into libtorch tensors using
`foldl`, `map*`, `from-list`, and `reshape`:

```scheme
;; Build input tensor [N, 2] and target tensor [N, 1]
(define input-list
  (foldl (lambda (acc row) (append acc (list (car row) (car (cdr row)))))
         '() train-data))
(define target-list
  (map* (lambda (row) (car (cdr (cdr row)))) train-data))

(define n-samples (length train-data))
(define X (reshape (from-list input-list) (list n-samples 2)))
(define Y (reshape (from-list target-list) (list n-samples 1)))
```

**Network architecture:**

```scheme
(define net (sequential (linear 2 32)
                        (relu-layer)
                        (linear 32 16)
                        (relu-layer)
                        (linear 16 1)))
(define opt (adam net 0.001))
```

This is a 2 → 32 → ReLU → 16 → ReLU → 1 feedforward network trained
with Adam (lr=0.001) using MSE loss.  `(train! net)` is called before
training to ensure proper gradient flow, and `(eval! net)` is called
before inference.

**Training loop:**

```scheme
(train! net)
(letrec ((loop (lambda (epoch)
           (if (> epoch 8000) 'done
               (let ((loss (train-step! net opt mse-loss X Y)))
                 (if (= (modulo epoch 200) 0)
                     (begin (display epoch) (display "   |  ") (println loss)))
                 (loop (+ epoch 1)))))))
  (loop 1))
(eval! net)
```

`train-step!` performs: zero-grad → forward → loss → backward → step
in one call.


### How the VM handles it

*Under the hood:*

**Tensor objects** are heap-allocated `TensorPtr` values — the VM
stores a `std::shared_ptr<at::Tensor>` inside a GC-managed heap
object.  When the GC traces reachable objects, tensor pointers
participate in the mark phase; when collected, the destructor releases
the libtorch tensor memory.

**`nn/sequential`** creates an `NNModulePtr` heap object wrapping a
`torch::nn::Sequential` container.  Each `nn/linear` and `nn/relu-layer`
call produces a sub-module pointer that is registered with the
sequential container.

**`optim/adam`** creates an `OptimizerPtr` heap object wrapping a
`torch::optim::Adam` instance.  The optimizer holds references to the
module's parameters (also tensors on the GC heap).

**`train-step!`** is a pure Eta function (defined in `std.torch`) that
sequences five C++ primitive calls:
1. `optim/zero-grad!` — clears accumulated gradients
2. `nn/forward` — runs the forward pass (returns a `TensorPtr`)
3. The `loss-fn` — here `nn/mse-loss` (returns a scalar `TensorPtr`)
4. `torch/backward` — triggers libtorch's autograd backward pass
5. `optim/step!` — updates parameters using the Adam rule

Each of these compiles to a `CallBuiltin` opcode that invokes the
corresponding C++ function registered in `core_primitives.h`.

---

## §5 — Neural Causal Estimation (ATE)

### What it does

Plug the trained NN into the do-calculus adjustment formula:

```
E[return | do(beta=x)] = Σ_s  E_NN[return | beta=x, sector=s] · P(s)
```

`nn-predict` creates a fresh input tensor, runs the forward pass, and
extracts a scalar:

```scheme
(defun nn-predict (beta-val sector-code)
  (let* ((inp (reshape (from-list (list beta-val sector-code)) '(1 2)))
         (out (forward net inp)))
    (item out)))
```

The back-door adjustment averages the NN prediction over all three
sectors (equal-weighted):

```scheme
(defun nn-adjusted-effect (beta-val)
  (/ (+ (+ (nn-predict beta-val  1.0)    ; tech
            (nn-predict beta-val  0.0))   ; energy
         (nn-predict beta-val -1.0))      ; finance
     3.0))
```

The ATE is the difference in causally-adjusted expected returns
between beta=1.2 and beta=0.9:

```scheme
(define beta-high 1.2)
(define beta-low  0.9)
(define e-high (nn-adjusted-effect beta-high))
(define e-low  (nn-adjusted-effect beta-low))
(define ate    (- e-high e-low))
```

### Expected output

```
  Back-door adjustment formula (from do-calculus):
    P(stock-return | do(market-beta)) = Σ_{sector} P(stock-return | market-beta, sector) · P(sector)

  Neural network predictions:
    E[return | do(beta=1.2)] = 1.78...
    E[return | do(beta=0.9)] = 1.33...

  ATE(beta: 0.9 → 1.2) = 0.45...

  True ATE (from DGP) = 1.5 * (1.2 - 0.9) = 0.45
```

### Interpretation

The **true ATE** from the DGP is `1.5 × (1.2 − 0.9) = 0.45`.  Since
`E[sector_code] = (1 + 0 + (−1))/3 = 0` (balanced design), the
interaction term vanishes in expectation and the ATE equals the direct
beta coefficient times the beta increment.

A naive regression that ignores confounding would overestimate the
effect for tech (because tech has both high beta *and* high base
returns) and underestimate it for finance.  The back-door adjustment
— whether implemented with sample means or a neural network —
correctly isolates the causal contribution of beta.

### Output

```console
==========================================================
  Causal Neural Factor Analysis
==========================================================

==========================================================
 S1. Symbolic Factor Model & Differentiation
==========================================================

  Factor model:
    return = alpha + beta*market + gamma*sector + delta*beta*sector
  As S-expression:
    (+ (+ (+ alpha (* beta market)) (* gamma sector)) (* delta (* beta sector)))

  Symbolic sensitivities:
    dReturn/dBeta   = (+ market (* delta sector))
    dReturn/dSector = (+ gamma (* delta beta))

  Insight: the beta sensitivity depends on sector (interaction term)
  => confounding! Motivates causal analysis in S2.

==========================================================
 S2. Causal DAG & Do-Calculus Identification
==========================================================

  DAG: ((sector -> market-beta) (sector -> stock-return) (market-beta -> stock-return))

    sector ---> market-beta ---> stock-return
      |                                ^
      +--------------------------------+

  Query:   P(stock-return | do(market-beta))
  Result:  P(stock-return | do(market-beta)) = Σ_{sector} P(stock-return | market-beta, sector) · P(sector)
  Adjustment set Z = (sector)

==========================================================
 S3. Logic Search & CLP Validation
==========================================================

  All nodes:          (sector market-beta stock-return)
  Non-descendants(X): (sector)

  Valid back-door adjustment sets (via findall):
    ((sector))

  CLP validation of adjustment weights:
    P(tech)=33%, P(energy)=33%, P(finance)=34%
    OK All weights are valid probabilities in (0,1)

==========================================================
 S4. Neural Network Training (libtorch)
==========================================================

  Training data: 60 samples
  Input shape:   (60 2)
  Target shape:  (60 1)

  Network: Sequential(Linear(2,32), ReLU, Linear(32,16), ReLU, Linear(16,1))
  Optimizer: Adam (lr=0.001)

  epoch  |   MSE loss
  -------+-----------
   200   |  0.00832449
   400   |  0.0049296
   600   |  0.00370432
   800   |  0.00304642
  1000   |  0.00259096
  1200   |  0.00226816
  1400   |  0.00203511
  1600   |  0.00186111
  1800   |  0.00172263
  2000   |  0.00160628
  2200   |  0.0015028
  2400   |  0.00140962
  2600   |  0.00132846
  2800   |  0.00125462
  3000   |  0.00118558
  3200   |  0.00111896
  3400   |  0.00105133
  3600   |  0.000990518
  3800   |  0.000942627
  4000   |  0.000912301
  4200   |  0.000894276
  4400   |  0.000884723
  4600   |  0.000878624
  4800   |  0.000873752
  5000   |  0.000870427
  5200   |  0.0008682
  5400   |  0.000866691
  5600   |  0.000865705
  5800   |  0.000864577
  6000   |  0.000864077
  6200   |  0.000863842
  6400   |  0.000863738
  6600   |  0.000863695
  6800   |  0.000863679
  7000   |  0.000863674
  7200   |  0.000863673
  7400   |  0.000863672
  7600   |  0.000863672
  7800   |  0.000863672
  8000   |  0.000863672

==========================================================
 S5. Neural Back-Door ATE Estimation
==========================================================

  Back-door adjustment formula (from do-calculus):
    P(stock-return | do(market-beta)) = Σ_{sector} P(stock-return | market-beta, sector) · P(sector)

  Per-sector NN predictions (E[return | beta, sector]):
                      beta=0.9     beta=1.2
    Tech    (s= 1):   2.43894    2.96839
    Energy  (s= 0):   1.35    1.80871
    Finance (s=-1):   0.280001    0.64

  Causal estimates (averaged over sectors via back-door formula):
    E[return | do(beta=1.2)] = 1.8057
    E[return | do(beta=0.9)]  = 1.35631

  ATE(beta: 0.9 -> 1.2) = 0.449388

  True ATE (from DGP) = 1.5 * (1.2 - 0.9) = 0.45
  (NN estimate should be close with sufficient training)

==========================================================
  Pipeline Summary
----------------------------------------------------------
  S1. Symbolic diff  => beta sensitivity depends on sector
  S2. Do-calculus    => P(return|do(beta)) = Σ_s P(ret|beta,s)·P(s)
  S3. findall + CLP  => {sector} is the unique valid adjustment set
  S4. libtorch NN    => learned E[return | beta, sector]
  S5. Neural ATE     => causal effect estimated via NN + formula
==========================================================
  Done.
==========================================================

```

### How the VM handles it

*Under the hood:*

`nn-predict` creates a fresh `[1, 2]` input tensor (`from-list` →
`reshape` → `TensorPtr`), runs `nn/forward` (libtorch forward pass),
and extracts a scalar with `torch/item` (returns a NaN-boxed `double`).

The arithmetic `(/ (+ (+ a b) c) 3.0)` operates on plain NaN-boxed
doubles — no tape, no tensors — since these are ordinary Eta numbers
at this point.  The VM executes `Add`, `Add`, `Div` opcodes in the
standard fast path.

---

## Pipeline Flow

```
  ┌──────────────────────┐
  │ §1 Symbolic Diff     │  factor model → ∂R/∂β depends on sector
  └──────────┬───────────┘
             │  insight: confounding
             ▼
  ┌──────────────────────┐
  │ §2 Do-Calculus       │  DAG → back-door formula: Σ_s P(R|β,s)·P(s)
  └──────────┬───────────┘
             │  formula + adjustment set Z
             ▼
  ┌──────────────────────┐
  │ §3 findall + CLP     │  exhaustive search → {sector} is unique valid Z
  └──────────┬───────────┘
             │  validated formula
             ▼
  ┌──────────────────────┐
  │ §4 libtorch NN       │  learn E[R | β, s] from data
  └──────────┬───────────┘
             │  trained NN
             ▼
  ┌──────────────────────┐
  │ §5 Neural ATE        │  NN + formula → ATE ≈ 0.45
  └──────────────────────┘
```

---

## Source Locations

| Component                             | File                                                                                                    |
|---------------------------------------|---------------------------------------------------------------------------------------------------------|
| **Causal Demo**                       | [`examples/causal_demo.eta`](../examples/causal_demo.eta)                                               |
| Causal DAG utilities & do-calculus    | [`stdlib/std/causal.eta`](../stdlib/std/causal.eta)                                                     |
| Logic programming (findall, membero)  | [`stdlib/std/logic.eta`](../stdlib/std/logic.eta)                                                       |
| CLP(Z) / CLP(FD) domains              | [`stdlib/std/clp.eta`](../stdlib/std/clp.eta)                                                           |
| libtorch wrappers (NN, optim, tensor) | [`stdlib/std/torch.eta`](../stdlib/std/torch.eta)                                                       |
| VM execution engine                   | [`eta/core/src/eta/runtime/vm/vm.cpp`](../eta/core/src/eta/runtime/vm/vm.cpp)                           |
| Constraint store (forward checking)   | [`eta/core/src/eta/runtime/clp/constraint_store.h`](../eta/core/src/eta/runtime/clp/constraint_store.h) |
| Bytecode compiler (`etac`)            | [`docs/compiler.md`](compiler.md)                                                                       |

