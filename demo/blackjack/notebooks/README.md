# Blackjack Notebook

This directory contains an optional Jupyter notebook wrapper:

- [`blackjack_demo.ipynb`](blackjack_demo.ipynb)

The notebook uses `blackjack_demo/run-command` so each cell matches the CLI
subcommands (`causal`, `learn`, `chart`, `maxims`) with
`tests/config/supervised_demo.json`.

## Example flow

```console
cd demo/blackjack/blackjack-demo
eta build
jupyter lab
```

Open `demo/blackjack/notebooks/blackjack_demo.ipynb` and select the Eta kernel.

## Links

- Demo overview: [../README.md](../README.md)
- Plan: [../../../docs/plan/blackjack_demo.md](../../../docs/plan/blackjack_demo.md)
