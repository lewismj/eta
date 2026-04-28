# Eta Notebooks

Three runnable notebooks demonstrating the language. All three open against
the **`Eta`** Jupyter kernel (`eta_jupyter`).

| Notebook | What it shows |
|---|---|
| [`LanguageBasics.ipynb`](LanguageBasics.ipynb) | S-expressions, modules, macros, REPL workflow. |
| [`AAD.ipynb`](AAD.ipynb) | Reverse-mode automatic differentiation; Black–Scholes Greeks in one backward pass. |
| [`Portfolio.ipynb`](Portfolio.ipynb) | Causal portfolio engine: do-calculus identification, NN return model, CLP(R) QP, AAD risk, do(m) scenarios. |

## Running locally

Make sure `eta_jupyter` is installed and the kernelspec is registered:

```sh
eta_jupyter --install --user      # or --sys-prefix in a virtualenv / conda env
jupyter lab
```

## Running on Binder

Click any of the badges in the top-level [`README.md`](../../docs/README.md). The
Binder image is built from [`../../binder/Dockerfile`](../../binder/Dockerfile);
first launch after a commit takes ~20 minutes, subsequent launches are
instant.

## Long-form companion docs

- AAD: [`docs/aad.md`](../../docs/aad.md)
- Portfolio: [`docs/portfolio.md`](../../docs/portfolio.md)
- Causal engine: [`docs/causal.md`](../../docs/causal.md)
- LibTorch bindings: [`docs/torch.md`](../../docs/torch.md)

