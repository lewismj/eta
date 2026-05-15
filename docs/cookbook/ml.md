---
title: "Cookbook — ML / Torch"
---

# ML / Torch

Machine learning examples using Eta's `std.torch` (libtorch) and `ml.lightgbm`
packages.

> **Requires build with `-DETA_BUILD_TORCH=ON`** (libtorch) or the
> `ml` package installed for LightGBM.

**Run with:**
```bash
etai cookbook/ml/torch.eta
etai cookbook/ml/lightgbm.eta
```

---

## Contents

- [Tensor Computing & Neural Networks](#tensor-computing--neural-networks)
- [LightGBM](#lightgbm)

---

## Tensor Computing & Neural Networks

`cookbook/ml/torch.eta` · [source](https://github.com/lewismj/eta/blob/main/cookbook/ml/torch.eta)

Demonstrates Eta's libtorch integration: tensor creation, arithmetic,
autograd, neural-network layers, optimisers, and a complete training loop
that fits `y = 2x + 1` via gradient descent.

### Tensor Creation & Arithmetic

```scheme
(module torch-example
  (import std.core std.io std.torch)
  (begin

    ;; Create tensors
    (define a (ones '(3)))
    (define b (ones '(3)))
    (tensor-print (t+ a b))          ; [2, 2, 2]
    (tensor-print (t* a (from-list '(3.0 3.0 3.0))))  ; [3, 3, 3]

    ;; From a list
    (define v (from-list '(1.0 2.0 3.0 4.0 5.0)))
    (tensor-print v)
    (println (item (tsum v)))        ; => 15.0

    ;; Shape operations
    (define m (zeros '(3 3)))
    (tensor-print (reshape m '(9)))  ; flat view
    (tensor-print (t-transpose m))   ; 3x3 transpose
  ))
```

### Autograd

```scheme
    ;; grad-tracking forward pass
    (define x (tensor-with-grad (from-list '(1.0 2.0 3.0))))
    (define y (tsum (* x x)))        ; y = sum(x^2)
    (backward! y)
    (tensor-print (grad x))          ; dy/dx = 2x => [2, 4, 6]
```

### Neural Network Layers

```scheme
    ;; nn/linear: y = Wx + b
    (define linear (nn/linear 3 2))
    (define x      (from-list '(1.0 0.5 -1.0)))
    (tensor-print (nn/forward linear x))

    ;; nn/sequential: chain layers
    (define net
      (nn/sequential
        (nn/linear 3 8)
        (nn/relu)
        (nn/linear 8 1)))
    (tensor-print (nn/forward net x))
```

### Complete Training Loop — Fit y = 2x + 1

```scheme
    ;; Training data: 100 samples of y = 2x + 1 + noise
    (define X-data
      (from-list (map (lambda (i) (/ i 100.0)) (range 0 100))))
    (define y-data
      (t+ (* (from-list '(2.0)) X-data)
          (from-list '(1.0))))

    ;; Simple linear model
    (define model (nn/linear 1 1))
    (define optim (adam (nn/parameters model) 0.01))

    (let loop ((epoch 0))
      (when (< epoch 200)
        (let* ((pred (nn/forward model (reshape X-data '(100 1))))
               (loss (mse-loss pred (reshape y-data '(100 1)))))
          (zero-grad! optim)
          (backward! loss)
          (optim-step! optim)
          (when (= (modulo epoch 50) 0)
            (display "epoch ") (display epoch)
            (display "  loss = ")
            (println (item loss))))
        (loop (+ epoch 1))))

    ;; Inspect learned parameters
    (display "weight = ") (tensor-print (nn/weight (nn/layer model 0)))
    (display "bias   = ") (tensor-print (nn/bias   (nn/layer model 0)))
    ;; weight ~ 2.0,  bias ~ 1.0
```

### Loss Functions & Optimisers

```scheme
    ;; Loss functions
    (define pred   (from-list '(0.1 0.9 0.3)))
    (define target (from-list '(0.0 1.0 0.0)))

    (println (item (mse-loss pred target)))
    (println (item (binary-cross-entropy pred target)))

    ;; Optimisers
    (define sgd-opt  (sgd  (nn/parameters model) 0.01))
    (define adam-opt (adam (nn/parameters model) 0.001))
```

### Device Information

```scheme
    ;; Query device capabilities
    (println (cuda-available?))      ; => #t / #f
    (println (device-count))         ; number of CUDA devices
    (println (current-device))       ; active device index
```

---

## LightGBM

`cookbook/ml/lightgbm.eta` · [source](https://github.com/lewismj/eta/blob/main/cookbook/ml/lightgbm.eta)

Gradient boosting with LightGBM via the `ml.lightgbm` package.
Build a dataset, train a booster, predict, evaluate, and reload from disk.

```scheme
(module cookbook.ml.lightgbm
  (import std.io ml.lightgbm)
  (begin

    ;; Build dataset from feature rows and labels
    (define dataset
      (dataset-from-list
        '((1.0 2.0)
          (1.5 1.2)
          (2.1 0.9))
        '(0.0 1.0 1.0)))

    ;; Create and train a booster
    (define booster (booster-create dataset))
    (train! booster dataset)
    (train! booster dataset)

    ;; Inspect the model
    (println (string-append "num-trees = "
              (number->string (num-trees booster))))

    ;; Predict on new data
    (println (predict booster '(1.2 1.8)))

    ;; Evaluate on training set
    (println (eval booster dataset))

    ;; Feature importance
    (println (feature-importance booster))

    ;; Persist and reload
    (save   booster "model.lgb")
    (define reloaded (load "model.lgb"))
    (println (predict reloaded '(1.2 1.8)))
  ))
```

