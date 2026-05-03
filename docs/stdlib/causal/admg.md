# std.causal.admg

Acyclic directed mixed graphs (ADMGs) and latent projection. Edges are
`(u -> v)` for directed and `(u <-> v)` for bidirected (latent
confounding).

```scheme
(import std.causal.admg)
```

| Symbol | Description |
| --- | --- |
| `(admg:directed g)` | Directed edges only. |
| `(admg:bidirected g)` | Bidirected edges only. |
| `(admg:nodes g)` | All node symbols. |
| `(admg:district g v)` | District (c-component) containing `v`. |
| `(admg:districts g)` | All districts. |
| `(admg:project g latent)` | Latent projection: marginalise out `latent`. |
| `(admg:moralize g)` | Moralised undirected graph. |
| `(admg:ancestors g v)` | Ancestors using directed edges only. |
| `(admg:c-component g v)` | C-component containing `v`. |
| `(admg:c-components g)` | All c-components. |

