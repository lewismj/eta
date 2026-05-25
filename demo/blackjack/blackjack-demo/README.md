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
eta run -- report --config tests/config/supervised_demo.json
eta run -- all --config tests/config/supervised_demo.json
```

## Workbook-first API

For notebook/tutorial use, import `blackjack_workbook` and run explicit steps
instead of routing through the full CLI:

```eta
(import blackjack_workbook)
(workbook-overview)
(run-step-1 42 64 1)
(run-step-2 42 16 10)
(run-step-3 42 8 32 0.01 4)
(run-workbook 42)
```

Step source locations:

- Step 1 simulation: `../blackjack/src/mc.eta`
- Step 2 causal sweep: `../blackjack/src/causal.eta`
- Step 3 learning: `../blackjack/src/learn.eta`
- Step 3 policy chart/maxims: `../blackjack/src/strategy.eta`, `../blackjack/src/maxims.eta`

## App module split

- `src/blackjack_demo.eta`: thin CLI entrypoint and command dispatch.
- `src/blackjack_demo_config.eta`: CLI spec + JSON/run config resolution.
- `src/blackjack_demo_logging.eta`: app-log session handling and phase log lines.
- `src/blackjack_demo_render.eta`: induce/causal/learn/chart/maxims/report rendering.
- `src/blackjack_workbook.eta`: explicit step-by-step notebook/workbook API.

Supported options:

- `--config <path>`: JSON config for seed + ML settings
- `--seed <n>`: seed fallback/override when `--config` is not used
- `--workers <n>`: in-proc actor workers for induce rollouts (default: `8`)
- `--app-log <auto|off|path>`: app timing log destination (default: `auto`)
- `--learn <joint|supervised>`: mode fallback/override when `--config` is not used

Config keys read from JSON:

- `seed`
- `app_log_path`
- `induce_rounds`
- `induce_workers`
- `learn_mode`
- `learn_epochs`
- `learn_batch_size`
- `learn_learning_rate`
- `learn_policy_refresh`

Defaults are tuned for performance profiling:
`induce_rounds=200000`, `induce_workers=8`.
The test config (`tests/config/supervised_demo.json`) keeps
`induce_rounds=64` for fast smoke runs.

Each section prints `elapsed-ms=...`, and `all`/`report` print `total-elapsed-ms=...`.
Timed commands persist progress and timings to a timestamped app log:
`blackjack-demo-<command>-<seed>-<epoch_ms>.log`.
Induce worker timing profiles are written into the app log with timestamps.
`learn` output embeds the Torch training log content and file path.
Use `--app-log off` (or `app_log_path: "off"`) to disable file logging.

`all` and `report` run the full pipeline in this order:
`induce -> causal -> learn -> chart -> maxims`.

## Test/build

```console
ETA_HEAP_MEMORY=262144000 ETA_HEAP_SOFT_LIMIT=250M HEAP=262144000 eta test
eta build
```

## Links

- Demo overview: [../README.md](../README.md)
- Library package: [../blackjack/README.md](../blackjack/README.md)
- Plan: [../../../docs/plan/blackjack_demo.md](../../../docs/plan/blackjack_demo.md)
- TLDR: [../../../TLDR.md](../../../TLDR.md)
- Next steps: [../../../docs/next-steps.md](../../../docs/next-steps.md)
