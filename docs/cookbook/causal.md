---
title: "Cookbook — Causal"
---

# Causal

Causal reasoning with DAGs, Pearl's do-calculus, back-door adjustment,
CLP-validated adjustment sets, and counterfactual estimation.

**Run with:**
```bash
etai cookbook/causal/causal_demo.eta
```

Or compile first for better performance:
```bash
etac -O cookbook/causal/causal_demo.eta
etai causal_demo.etac
```

---

## Contents

- [Causal DAG Example](#causal-dag-example)
- [Do-Calculus Rules](#do-calculus-rules)
- [Causal — Neural Pipeline](#causal--neural-pipeline)

---

## Causal DAG Example

`cookbook/do-calculus/dag.eta` · [source](https://github.com/lewismj/eta/blob/main/cookbook/do-calculus/dag.eta)

Building and querying a DAG with `std.causal`.

```scheme
(module dag-demo
  (import std.io std.causal)
  (begin

    ;; Define a DAG over market variables
    ;; sentiment -> macro, sentiment -> return
    ;; macro -> return, beta -> return, sector -> return
    (define g
      (causal:dag
        '((sentiment macro)
          (sentiment return)
          (macro     return)
          (beta      return)
          (sector    return))))

    ;; Query the DAG structure
    (println (causal:parents   g 'return))
    ;; => (macro beta sector sentiment)

    (println (causal:ancestors g 'return))
    ;; => (sentiment macro beta sector)

    ;; Does X have a causal path to Y?
    (println (causal:connected? g 'beta    'return))  ; => #t
    (println (causal:connected? g 'return  'beta))    ; => #f

    ;; d-separation: is 'beta d-separated from 'sector given 'return?
    (println (causal:d-separated? g 'beta 'sector '(return)))  ; => ...
  ))
```

---

## Do-Calculus Rules

`cookbook/do-calculus/do-rules.eta` · [source](https://github.com/lewismj/eta/blob/main/cookbook/do-calculus/do-rules.eta)

Applying Pearl's three rules of do-calculus symbolically to manipulate
interventional distributions.

```scheme
(module do-rules
  (import std.io std.causal)
  (begin

    ;; Backdoor criterion
    ;; A set Z satisfies the back-door criterion relative to (X, Y) if:
    ;;   1. No node in Z is a descendant of X
    ;;   2. Z blocks every back-door path from X to Y
    (define g
      (causal:dag
        '((confounder x) (confounder y) (x y))))

    ;; Find all valid back-door adjustment sets
    (define adj-sets
      (causal:backdoor-sets g 'x 'y))

    (display "Back-door adjustment sets: ")
    (println adj-sets)
    ;; => ((confounder))

    ;; Identify the interventional distribution P(y | do(x))
    ;; using the back-door adjustment formula:
    ;;   P(y | do(x)) = sum_z P(y | x, z) * P(z)
    (define formula
      (causal:identify g 'y 'x (car adj-sets)))

    (println formula)
    ;; => (backdoor-adjust y x (confounder))
  ))
```

---

## Causal — Neural Pipeline

`cookbook/causal/causal_demo.eta` · [source](https://github.com/lewismj/eta/blob/main/cookbook/causal/causal_demo.eta)

A four-stage pipeline that chains symbolic processing, causal reasoning,
constraint logic, and neural computation into a single program.

**Data-generating process (known, for validation):**
```
return = 1.5·beta + 0.8·sector_code + 0.3·beta·sector_code + noise
True ATE(beta: 1.2→0.9) ≈ 0.447
```

### Stage 1 — Symbolic Factor Model

Define a linear factor model as an S-expression, symbolically differentiate
it with respect to each factor, and simplify with fixed-point algebraic
rewriting.

```scheme
;; Factor model: return = a*beta + b*macro + noise
(define factor-model
  '(+ (* alpha beta) (* gamma macro)))

;; Symbolic gradient w.r.t. beta
(define d-beta  (symbolic-diff factor-model 'beta))
(define d-macro (symbolic-diff factor-model 'macro))

(println d-beta)    ; => alpha
(println d-macro)   ; => gamma
```

### Stage 2 — Causal Reasoning

Encode the DAG of market variables and use the back-door adjustment
formula to derive `P(return | do(beta))`.

```scheme
(define g
  (causal:dag
    '((sentiment macro) (sentiment return)
      (macro return) (beta return)
      (sector return))))

;; Back-door adjustment for P(return | do(beta))
;; Adjustment set: {macro, sector}  (blocks all back-door paths)
(define adj-sets (causal:backdoor-sets g 'beta 'return))
(println (car adj-sets))    ; => (macro sector)
```

### Stage 3 — CLP Validation

Use `findall` with backtracking to discover every valid back-door adjustment
set, then validate each candidate with CLP(Z) domain constraints.

```scheme
(define valid-sets
  (filter
    (lambda (s)
      ;; Verify subset size constraint via CLP(Z)
      (let ((n (logic-var)))
        (clp:domain n 1 3)
        (clp:= n (length s))
        (clp:labeling (list n) 'strategy 'ff)))
    adj-sets))

(println (length valid-sets))   ; number of valid adjustment sets
```

### Stage 4 — Neural ATE Estimation

Train a small neural network (`nn/sequential` + `nn/linear`) to estimate
`E[return | beta, sector]`, then plug predictions into the causal formula
to compute the Average Treatment Effect.

```scheme
(import std.torch)

;; Two-layer MLP: input(2) -> hidden(16) -> output(1)
(define model
  (nn/sequential
    (nn/linear 2 16)
    (nn/relu)
    (nn/linear 16 1)))

;; Mini-batch SGD training loop
(defun train-step (model xs ys lr)
  (let* ((pred (nn/forward model xs))
         (loss (mse-loss pred ys)))
    (zero-grad! model)
    (backward! loss)
    (sgd-step! model lr)
    (item loss)))

;; Training loop (50 epochs)
(let loop ((epoch 0))
  (when (< epoch 50)
    (let ((loss (train-step model X-train y-train 0.01)))
      (when (= (modulo epoch 10) 0)
        (display "epoch ") (display epoch)
        (display "  loss = ") (println loss)))
    (loop (+ epoch 1))))

;; ATE via back-door adjustment:
;;   ATE = E[Y|do(beta=1.2)] - E[Y|do(beta=0.9)]
;;       = sum_s [ E_nn[return|1.2,s] - E_nn[return|0.9,s] ] * P(s)
(display "Estimated ATE: ")
(println (compute-ate model sector-probs 1.2 0.9))
;; => ~0.447  (matches ground truth)
```

