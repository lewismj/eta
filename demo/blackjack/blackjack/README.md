# blackjack

Library package for the blackjack demo.

## Modules

- `src/shoe.eta`
  - deterministic `make-shoe` and pure `deal`
  - hand scoring helpers: `hand-totals`, `best-total`, `soft-hand`
  - count helper: `running-count` with configurable weight vector
- `src/rules.eta`
  - predicates: `bust`, `blackjack`, `dealer-hits`, `dealer-stands`
  - outcome checks: `player-wins`, `push`
- `src/mc.eta`
  - compact rollout core (`[13]` rank-count vector and fixed-size hand tuple)
  - frozen trace schema: `blackjack-trace-v1`
  - deterministic in-proc actor sharding (default `8` workers) with replay checks
- `src/causal.eta`
  - causal DAG declaration via `std.causal.render/define-dag`
  - observational EV (`ev-obs`) and interventional EV (`ev-do`) for hit/stand
  - `policy-do` and count-sweep helpers for the `(16,10)` action flip check
- `src/learn.eta`
  - Torch count-compressor + EV head (`std.torch`)
  - deterministic supervised and joint policy-iteration training paths
  - adaptive learning-rate decay on loss plateaus
  - per-run training logs via `std.log` to `blackjack-learn-<mode>-<seed>-<epoch_ms>.log`
  - bounded no-improvement stop to avoid unbounded non-converging loops
  - B5 gate metrics: cosine-to-Hi-Lo and monotone EV check
- `src/strategy.eta`
  - direct cartesian enumeration over `(player_total, dealer_up, count_bucket)`
  - `argmax` policy projection via causal `policy-do`
  - indexed chart lookups for fast rendering
- `src/maxims.eta`
  - greedy set-cover induction over chart cells for compact hard-total rules
  - deterministic rule rendering for `count_bucket` snapshots
  - rule evaluation helper used by B6.5 acceptance tests
- `src/blackjack.eta`
  - public re-export surface for the package

## Tests

```console
ETA_HEAP_MEMORY=262144000 ETA_HEAP_SOFT_LIMIT=250M HEAP=262144000 eta test
```

## Links

- Demo overview: [../README.md](../README.md)
- App package: [../blackjack-demo/README.md](../blackjack-demo/README.md)
- Plan: [../../../docs/plan/blackjack_demo.md](../../../docs/plan/blackjack_demo.md)
- TLDR: [../../../TLDR.md](../../../TLDR.md)
- Next steps: [../../../docs/next-steps.md](../../../docs/next-steps.md)
