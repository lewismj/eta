# Blackjack Notebook

This directory contains an optional Jupyter notebook wrapper:

- [`blackjack_demo.ipynb`](blackjack_demo.ipynb)

The notebook now uses the explicit workbook module
`blackjack_workbook` with a small step-by-step flow:

- `run-step-1`: trace generation (`make-mc-shoe`, `simulate`, `simulate-summary`)
- `run-step-2`: causal sweep (`make-causal-state`, `sweep-do`, `find-flip-threshold`)
- `run-step-3`: learning and policy artifacts
  (`recover-hi-lo-supervised`, `render-strategy-chart`, `render-maxim-clauses-for-count`)

Source locations called out in the notebook:

- simulation: `../blackjack/src/mc.eta`
- causal: `../blackjack/src/causal.eta`
- learning: `../blackjack/src/learn.eta`
- strategy/maxims: `../blackjack/src/strategy.eta`, `../blackjack/src/maxims.eta`

This keeps the story readable in a workbook instead of routing each cell
through the full CLI wrapper.

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
