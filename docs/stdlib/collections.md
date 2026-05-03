# std.collections

Higher-order operations on lists and vectors. Closure-compatible variants of
the standard sequence procedures.

```scheme
(import std.collections)
```

## List operations

| Symbol | Description |
| --- | --- |
| `(map* f xs)` | Map `f` over `xs`. |
| `(map2 f xs ys)` | Map a binary function over two lists. |
| `(zip-with f xs ys)` | Alias for `map2`. |
| `(map-indexed f xs)` | Map `(f index x)` over `xs`. |
| `(filter pred xs)` | Keep elements for which `pred` returns true. |
| `(foldl f init xs)` | Left fold. |
| `(foldr f init xs)` | Right fold. |
| `(reduce f xs)` | Left fold using the first element as seed. Errors on empty input. |
| `(any? pred xs)` | True when at least one element satisfies `pred`. |
| `(every? pred xs)` | True when all elements satisfy `pred`. |
| `(count pred xs)` | Number of elements satisfying `pred`. |
| `(sum-by f xs)` | Sum of `(f x)` over `xs`. |
| `(zip xs ys)` | List of pairs. |
| `(pairwise xs)` | List of consecutive overlapping pairs. |
| `(take n xs)` | First `n` elements. |
| `(drop n xs)` | Drop first `n` elements. |
| `(flatten xs)` | Flatten one level of nesting. |
| `(range start end [step])` | Numeric range as a list. |
| `(sort less? xs)` | Stable sort. |

## Vector operations

| Symbol | Description |
| --- | --- |
| `(vector-map f v)` | Map producing a new vector. |
| `(vector-for-each f v)` | Apply `f` for side effects. |
| `(vector-foldl f init v)` | Left fold over a vector. |
| `(vector-foldr f init v)` | Right fold over a vector. |
| `(vector->list v)` | Convert vector to list. |
| `(list->vector xs)` | Convert list to vector. |

