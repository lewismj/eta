# std.causal.learn

Structure-learning helpers for causal graphs.

```scheme
(import std.causal.learn)
```

## Conditional-independence tests

Each test returns `(independent? . p-value)`.

| Symbol | Description |
| --- | --- |
| `(learn:ci-test:fisher-z data x y z alpha)` | Fisher-Z partial-correlation test. |
| `(learn:ci-test:chi2 data x y z alpha)` | Discrete chi-square test. |

## Search

| Symbol | Description |
| --- | --- |
| `(learn:pc data alpha ci-test)` | PC algorithm: skeleton, collider orientation, Meek propagation. Returns a CPDAG with `(u -> v)` and `(u -- v)` edges. |
| `(learn:fci data alpha ci-test)` | Latent-aware hook; currently delegates to `learn:pc`. |
| `(learn:ges data [alpha [ci-test]])` | Score-search hook; currently delegates to `learn:pc`. |
| `(learn:notears data lambda1 max-iter)` | Continuous-relaxation learner using correlation thresholding plus transitive reduction. |

