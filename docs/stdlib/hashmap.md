# std.hashmap

Helpers built over the runtime hash-map primitives.

```scheme
(import std.hashmap)
```

| Symbol | Description |
| --- | --- |
| `(hash-map-empty? m)` | True when `m` has no entries. |
| `(hash-map-contains? m key)` | True when `key` is present. |
| `(hash-map-update m key f)` | Return a new map where `key` is replaced by `(f current)`. |
| `(hash-map-update-with-default m key f default)` | Like `hash-map-update` but uses `default` when the key is absent. |
| `(hash-map-merge a b)` | Merge two maps; values from `b` win on conflict. |
| `(hash-map-merge-with f a b)` | Merge using `(f a-val b-val)` to combine collisions. |
| `(hash-map-map f m)` | Map values; `f` receives `(key value)`. |
| `(hash-map-filter pred m)` | Keep entries for which `(pred key value)` is true. |
| `(hash-map->alist m)` | Convert to an association list. |
| `(alist->hash-map xs)` | Build a map from an association list. |

