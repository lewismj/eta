# blackjack-demo

Executable package for the blackjack demo.

## CLI

```console
eta run -- help
eta run -- induce --config tests/config/supervised_demo.json
eta run -- causal --config tests/config/supervised_demo.json
eta run -- learn --config tests/config/supervised_demo.json
eta run -- chart --config tests/config/supervised_demo.json
eta run -- maxims --config tests/config/supervised_demo.json
eta run -- all --config tests/config/supervised_demo.json
```

Supported options:

- `--config <path>`: JSON config for seed + ML settings
- `--seed <n>`: seed fallback/override when `--config` is not used
- `--learn <joint|supervised>`: mode fallback/override when `--config` is not used

Config keys read from JSON:

- `seed`
- `induce_rounds`
- `learn_mode`
- `learn_epochs`
- `learn_batch_size`
- `learn_learning_rate`
- `learn_policy_refresh`

Each section prints `elapsed-ms=...`, and `all` prints `total-elapsed-ms=...`.

`all` runs the full pipeline in this order:
`induce -> causal -> learn -> chart -> maxims`.

## Test/build

```console
ETA_HEAP_SOFT_LIMIT=500M HEAP=524288000 eta test
eta build
```

## Links

- Demo overview: [../README.md](../README.md)
- Library package: [../blackjack/README.md](../blackjack/README.md)
- Plan: [../../../docs/plan/blackjack_demo.md](../../../docs/plan/blackjack_demo.md)
- TLDR: [../../../TLDR.md](../../../TLDR.md)
- Next steps: [../../../docs/next-steps.md](../../../docs/next-steps.md)
