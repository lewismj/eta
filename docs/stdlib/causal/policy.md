# std.causal.policy

Uplift, policy-value and synthetic-truth scoring helpers.

```scheme
(import std.causal.policy)
```

## Ranking metrics

| Symbol | Description |
| --- | --- |
| `(policy:qini-curve scores treat outcome)` | Qini curve points. |
| `(policy:qini-coefficient scores treat outcome)` | Area-based Qini coefficient. |
| `(policy:auuc scores treat outcome)` | Area under uplift curve. |
| `(policy:cumulative-gain-curve scores treat outcome)` | Cumulative-gain curve. |

## Policy values

| Symbol | Description |
| --- | --- |
| `(policy:value-ipw policy data y x cols)` | IPW estimate of policy value. |
| `(policy:value-aipw policy data y x cols)` | AIPW estimate of policy value. |
| `(policy:rank-by-cate scores)` | Rank actions by predicted CATE. |

## Synthetic-truth metrics

| Symbol | Description |
| --- | --- |
| `(policy:pehe true-cate predicted-cate)` | Square root of mean squared CATE error. |
| `(policy:ate-rmse true-ate estimates)` | RMSE of ATE estimates. |
| `(policy:ate-bias true-ate estimates)` | Mean bias of ATE estimates. |

## Policy construction

| Symbol | Description |
| --- | --- |
| `(policy:greedy-treat-positive scores)` | Treat units with positive predicted uplift. |
| `(policy:greedy-budget scores k)` | Treat the top-`k` units by score. |

