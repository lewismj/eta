# Eta Notebooks

Three runnable notebooks demonstrating the language. All three open against
the **`Eta`** Jupyter kernel (`eta_jupyter`).

| Notebook | What it shows |
|---|---|
| [`LanguageBasics.ipynb`](https://github.com/lewismj/eta/blob/main/cookbook/notebooks/LanguageBasics.ipynb) | S-expressions, modules, macros, REPL workflow. |
| [`AAD.ipynb`](https://github.com/lewismj/eta/blob/main/cookbook/notebooks/AAD.ipynb) | Reverse-mode automatic differentiation; Black–Scholes Greeks in one backward pass. |
| [`Portfolio.ipynb`](https://github.com/lewismj/eta/blob/main/cookbook/notebooks/Portfolio.ipynb) | Causal portfolio engine: do-calculus identification, NN return model, CLP(R) QP, AAD risk, do(m) scenarios. |

## Running locally

Make sure `eta_jupyter` is installed and the kernelspec is registered:

```sh
eta_jupyter --install --user      # or --sys-prefix in a virtualenv / conda env
jupyter lab
```

## Running on Binder

Click any of the badges in the top-level [README](https://github.com/lewismj/eta#readme). The
Binder image is built from [binder/Dockerfile](https://github.com/lewismj/eta/blob/main/binder/Dockerfile);
first launch after a commit takes ~20 minutes, subsequent launches are
instant.

## Long-form companion docs

- AAD: [`docs/guide/reference/aad.md`](../../guide/reference/aad/)
- Portfolio: [`docs/featured/portfolio.md`](../../featured/portfolio/)
- Causal engine: [`docs/guide/reference/causal.md`](../../guide/reference/causal/)
- LibTorch bindings: [`docs/guide/reference/torch.md`](../../guide/reference/torch/)

