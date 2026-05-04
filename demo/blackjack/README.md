# Blackjack Demo

This directory contains the blackjack demo packages defined in
[docs/plan/blackjack_demo.md](../../docs/plan/blackjack_demo.md).

## Packages

- [`blackjack/`](blackjack/README.md): library package with deterministic shoe/rules, causal checks, learning, strategy charts, and maxim induction.
- [`blackjack-demo/`](blackjack-demo/README.md): app package with the `induce|causal|learn|chart|maxims|all` CLI.
- [`notebooks/`](notebooks/README.md): optional Jupyter notebook wrapper.

## Quick run (copy/paste)

```console
cd demo/blackjack/blackjack
ETA_HEAP_SOFT_LIMIT=500M HEAP=524288000 eta test

cd ../blackjack-demo
ETA_HEAP_SOFT_LIMIT=500M HEAP=524288000 eta test
eta build
eta run -- all --config tests/config/supervised_demo.json
```

The `all` command prints deterministic section headers:

- `== induce ==`
- `== causal ==`
- `== learn ==`
- `== chart ==`
- `== maxims ==`

Each section also prints `elapsed-ms=...`, and the footer includes
`total-elapsed-ms=...`.

## Links

- Repo README: [README.md](../../README.md)
- TLDR quickstart: [TLDR.md](../../TLDR.md)
- Next roadmap items: [docs/next-steps.md](../../docs/next-steps.md)
