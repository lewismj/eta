# std.causal.render

Rendering helpers for causal graphs. Edge kinds: `->`, `<->`, `--`.

```scheme
(import std.causal.render)
```

| Symbol | Description |
| --- | --- |
| `(dag:->dot g [opts])` | Render to Graphviz DOT. Options: `(title . str)`, `(rankdir . "LR")`. |
| `(dag:->mermaid g)` | Render to Mermaid flowchart syntax. |
| `(dag:->latex g)` | Render to a compact LaTeX expression. |
| `(define-dag g)` | Validate and normalise a graph literal containing `->` and `<->` edges. |

