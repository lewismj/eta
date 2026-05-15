---
title: "Cookbook — Numerics"
---

# Numerics

Quantitative finance and scientific computing: options pricing, AAD,
portfolio optimisation, SABR, fact tables, and statistics.

**Run any example with:**
```bash
etai cookbook/numerics/<file>.eta
```

Computationally intensive programs can be compiled first:
```bash
etac -O cookbook/numerics/<file>.eta
etai <file>.etac
```

---

## Contents

- [European Option Greeks (AAD)](#european-option-greeks-aad)
- [AAD — Tape-Based Reverse-Mode AD](#aad--tape-based-reverse-mode-ad)
- [SABR Model](#sabr-model)
- [Portfolio LP](#portfolio-lp)
- [Causal Portfolio Engine](#causal-portfolio-engine)
- [Fact Tables](#fact-tables)
- [Statistics](#statistics)

---

## European Option Greeks (AAD)

`cookbook/numerics/european.eta` · [source](https://github.com/lewismj/eta/blob/main/cookbook/numerics/european.eta)

Black-Scholes option Greeks (Delta, Vega, Theta, Rho) and second-order Greeks
(Gamma, Vanna, Volga) using Eta's built-in tape-based reverse-mode AD.
The VM records arithmetic onto a flat tape when a `TapeRef` operand is present;
a single backward pass sweeps the tape to accumulate all first-order adjoints.

```scheme
(module european
  (import std.core std.io std.aad)
  (begin

    ;; Primal extraction helper for branch decisions
    (defun branch-primal (x)
      (if (tape-ref? x) (tape-ref-value x) x))

    ;; Normal PDF: phi(x) = (1/sqrt(2*pi)) * exp(-x^2/2)
    (define inv-sqrt-2pi 0.3989422804014327)

    (defun norm-pdf (x)
      (* inv-sqrt-2pi (exp (* -0.5 (* x x)))))

    ;; Normal CDF (rational approximation)
    (defun norm-cdf (x)
      (let* ((px (branch-primal x))
             (neg? (< px 0.0))
             (ax  (if neg? (- 0.0 x) x))
             (t   (/ 1.0 (+ 1.0 (* 0.2316419 ax))))
             (poly (+ (* t (+ 0.319381530
                    (* t (+ -0.356563782
                    (* t (+ 1.781477937
                    (* t (+ -1.821255978
                    (* t 1.330274429))))))))))
             (cdf (* (- 1.0 (* (norm-pdf ax) poly)))))
        (if neg? (- 1.0 cdf) cdf)))

    ;; Black-Scholes call price
    (defun bs-call (s k r q t vol)
      (let* ((sqrt-t (sqrt t))
             (d1     (/ (+ (log (/ s k))
                           (* t (+ (- r q) (* 0.5 vol vol))))
                        (* vol sqrt-t)))
             (d2     (- d1 (* vol sqrt-t)))
             (nd1    (norm-cdf d1))
             (nd2    (norm-cdf d2)))
        (- (* s (exp (* (- q) t)) nd1)
           (* k (exp (* (- r) t)) nd2))))

    ;; Compute first-order Greeks via reverse-mode AD
    ;; Inputs: S=100 K=100 r=0.05 q=0.02 T=1.0 vol=0.20
    (let* ((tape (aad:new-tape))
           (s    (aad:var tape 100.0))
           (k    (aad:var tape 100.0))
           (r    (aad:var tape 0.05))
           (q    (aad:var tape 0.02))
           (T    (aad:var tape 1.0))
           (vol  (aad:var tape 0.20))
           (pv   (bs-call s k r q T vol)))
      (aad:backward tape pv)
      (println (string-append "Price  = " (number->string (tape-ref-value pv))))
      (println (string-append "Delta  = " (number->string (aad:grad s))))
      (println (string-append "Vega   = " (number->string (aad:grad vol))))
      (println (string-append "Theta  = " (number->string (aad:grad T))))
      (println (string-append "Rho    = " (number->string (aad:grad r)))))
  ))
```

---

## AAD — Tape-Based Reverse-Mode AD

`cookbook/numerics/aad.eta` · [source](https://github.com/lewismj/eta/blob/main/cookbook/numerics/aad.eta)

Low-level demonstration of Eta's automatic adjoint differentiation (AAD) tape.
Shows tape construction, forward evaluation, backward sweep, and gradient
extraction for elementary and composed functions.

```scheme
(module aad-demo
  (import std.core std.io std.aad)
  (begin

    ;; f(x, y) = x^2 + 3*y
    ;; df/dx = 2x,  df/dy = 3
    (let* ((tape (aad:new-tape))
           (x    (aad:var tape 4.0))
           (y    (aad:var tape 2.0))
           (f    (+ (* x x) (* 3.0 y))))
      (aad:backward tape f)
      (println (aad:grad x))           ; => 8.0  (2*4)
      (println (aad:grad y)))          ; => 3.0

    ;; g(x) = sin(x) * exp(x)
    ;; dg/dx = cos(x)*exp(x) + sin(x)*exp(x)
    (let* ((tape (aad:new-tape))
           (x    (aad:var tape 1.0))
           (g    (* (sin x) (exp x))))
      (aad:backward tape g)
      (display "dg/dx at x=1: ")
      (println (aad:grad x)))
    ;; => cos(1)*exp(1) + sin(1)*exp(1) ~= 3.756
  ))
```

---

## SABR Model

`cookbook/numerics/sabr.eta` · [source](https://github.com/lewismj/eta/blob/main/cookbook/numerics/sabr.eta)

SABR stochastic volatility model: Hagan approximation for implied volatility,
calibration via gradient descent using AAD, and volatility smile generation.

```scheme
(module sabr
  (import std.core std.io std.math std.aad)
  (begin

    ;; Hagan et al. SABR implied-vol approximation
    ;; Parameters: F (forward), K (strike), T (expiry),
    ;;             alpha (vol-of-forward), beta, rho, nu (vol-of-vol)
    (defun sabr-vol (F K T alpha beta rho nu)
      (let* ((FK    (sqrt (* F K)))
             (logFK (log (/ F K)))
             (z     (* (/ nu alpha) (expt FK (- 1.0 beta)) logFK))
             (chi   (log (/ (+ (sqrt (- 1.0 (* 2.0 rho z) (* z z))) z (- rho))
                            (- 1.0 rho))))
             (A     (/ alpha (expt FK (- 1.0 beta))))
             (B1    (+ 1.0 (/ (* (- 1.0 beta) (- 1.0 beta) logFK logFK) 24.0)
                       (/ (* (- 1.0 beta) (- 1.0 beta) logFK logFK logFK logFK) 1920.0)))
             (B2    (+ 1.0
                       (* T (+ (/ (* (- 1.0 beta) (- 1.0 beta) alpha alpha)
                                  (* 24.0 (expt FK (* 2.0 (- 1.0 beta)))))
                               (/ (* rho beta nu alpha)
                                  (* 4.0 (expt FK (- 1.0 beta))))
                               (/ (* nu nu (- 2.0 (* 3.0 rho rho)))
                                  24.0))))))
        (* A (/ z chi) (/ 1.0 B1) B2)))

    ;; Print a volatility smile
    (define F 100.0) (define T 1.0)
    (define alpha 0.20) (define beta 0.5)
    (define rho -0.3)  (define nu 0.4)

    (for-each
      (lambda (K)
        (display "K=") (display K) (display "  vol=")
        (println (sabr-vol F K T alpha beta rho nu)))
      '(80.0 90.0 95.0 100.0 105.0 110.0 120.0))
  ))
```

---

## Portfolio LP

`cookbook/numerics/portfolio-lp.eta` · [source](https://github.com/lewismj/eta/blob/main/cookbook/numerics/portfolio-lp.eta)

Mean-variance portfolio optimisation solved as a linear programme using
`std.clpr` (CLP over reals). Maximise expected return subject to a
volatility budget and long-only constraints.

```scheme
(module portfolio-lp
  (import std.core std.io std.collections std.clpr)
  (begin

    ;; Assets: expected returns and pairwise covariance matrix
    (define expected-returns '(0.12 0.08 0.15 0.06))
    (define cov-matrix
      '((0.04 0.01 0.02 0.00)
        (0.01 0.02 0.01 0.00)
        (0.02 0.01 0.06 0.01)
        (0.00 0.00 0.01 0.01)))

    ;; Variables: weights w1..w4
    (define ws (map (lambda (_) (clpr:var)) expected-returns))

    ;; Constraint: weights sum to 1
    (clpr:= (apply + ws) 1.0)

    ;; Long-only: each weight >= 0
    (for-each (lambda (w) (clpr:>= w 0.0)) ws)

    ;; Volatility budget: portfolio variance <= 0.025
    (clpr:<= (apply +
               (map (lambda (wi row)
                      (apply + (map (lambda (wj cij) (* wi wj cij))
                                    ws row)))
                    ws cov-matrix))
             0.025)

    ;; Maximise expected return
    (define obj (apply + (map * ws expected-returns)))
    (clpr:maximize obj)

    ;; Print solution
    (for-each
      (lambda (w i)
        (display (string-append "w" (number->string i) " = "))
        (println (clpr:value w)))
      ws '(1 2 3 4))
  ))
```

---

## Causal Portfolio Engine

`cookbook/numerics/portfolio.eta` · [source](https://github.com/lewismj/eta/blob/main/cookbook/numerics/portfolio.eta)

An end-to-end institutional portfolio construction system that combines
symbolic specification, causal inference (do-calculus), CLP(Z/R) constraints,
neural return estimation (libtorch), and AAD — all in a single Eta program.

```scheme
;; Abbreviated overview — see the full source for details.
(module portfolio
  (import std.io std.core std.math std.collections
          std.fact_table std.causal std.causal.identify
          std.logic std.clp std.clpr
          (except std.torch grad) std.stats)
  (begin

    ;; 1. Define the causal DAG
    ;;    sentiment -> macro -> return
    ;;    sentiment -> return
    ;;    beta, sector, rate -> return
    (define dag
      (causal:dag
        '((sentiment macro) (sentiment return)
          (macro return) (beta return)
          (sector return) (rate return))))

    ;; 2. Identify back-door adjustment formula for P(return|do(beta))
    (define adjustment-sets
      (causal:backdoor-sets dag 'beta 'return))

    ;; 3. Generate synthetic data, train a neural net to estimate
    ;;    E[return | beta, macro, sector], then compute ATE via
    ;;    the adjustment formula.

    ;; 4. Solve portfolio optimisation with CLP(R) constraints
    ;;    (volatility budget, sector limits, long-only).
    ...
  ))
```

> See [`docs/featured/portfolio`](/docs/featured/portfolio/) for the full
> walkthrough with outputs.

---

## Fact Tables

`cookbook/numerics/fact-table.eta` · [source](https://github.com/lewismj/eta/blob/main/cookbook/numerics/fact-table.eta)

In-memory columnar fact tables for aggregation and analytics, similar to a
stripped-down OLAP engine. Supports group-by, filter, sum, mean, and join.

```scheme
(module fact-table-demo
  (import std.io std.fact_table)
  (begin

    ;; Build a fact table from a list of records
    (define ft
      (fact-table:from-records
        '(sector beta return)
        '((tech  1.2  0.15)
          (tech  0.9  0.08)
          (fin   1.4  0.12)
          (fin   1.1  0.09)
          (util  0.6  0.04)
          (util  0.7  0.05))))

    ;; Mean return by sector
    (define by-sector
      (fact-table:group-by ft 'sector
        (list (cons 'mean-return (fact-table:mean 'return)))))

    (fact-table:print by-sector)
    ;; sector | mean-return
    ;; tech   | 0.115
    ;; fin    | 0.105
    ;; util   | 0.045
  ))
```

---

## Statistics

`cookbook/numerics/stats.eta` · [source](https://github.com/lewismj/eta/blob/main/cookbook/numerics/stats.eta)

Descriptive statistics, sampling distributions, and hypothesis testing
via `std.stats`.

```scheme
(module stats-demo
  (import std.io std.stats)
  (begin

    (define xs '(2.1 3.4 2.8 4.1 3.7 2.2 3.9 4.5 2.6 3.1))

    (println (stats:mean   xs))     ; => 3.24
    (println (stats:stddev xs))     ; sample std dev
    (println (stats:median xs))
    (println (stats:iqr    xs))     ; interquartile range

    ;; Normal distribution helpers
    (println (stats:norm-pdf 0.0))  ; => 0.3989...
    (println (stats:norm-cdf 1.96)) ; => 0.9750...

    ;; One-sample t-test (H0: mu = 3.0)
    (define result (stats:t-test xs 3.0))
    (println (cons 't-stat  (cdr (assoc 't-stat  result))))
    (println (cons 'p-value (cdr (assoc 'p-value result))))
  ))
```

