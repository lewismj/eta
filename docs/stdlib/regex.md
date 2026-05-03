# std.regex

Regular-expression helpers and match-payload accessors over the
`%regex-*` runtime primitives.

```scheme
(import std.regex)
```

## Compile

| Symbol | Description |
| --- | --- |
| `(regex:compile pattern . flags)` | Compile a pattern. Flags include `'icase`, `'multiline`, `'dotall`. |
| `(regex? x)` | True when `x` is a compiled regex. |
| `(regex:pattern r)` | Source pattern string. |
| `(regex:flags r)` | Compiled flags. |
| `(regex:quote str)` | Escape `str` so it matches literally. |

## Match

| Symbol | Description |
| --- | --- |
| `(regex:match? r str)` | True when `r` matches anywhere in `str`. |
| `(regex:search r str)` | Return first match, or `#f`. |
| `(regex:find-all r str)` | List of all non-overlapping matches. |
| `(regex:split r str)` | Split `str` at matches of `r`. |

## Replace

| Symbol | Description |
| --- | --- |
| `(regex:replace r str replacement)` | Replace all matches with a string. |
| `(regex:replace-fn r str f)` | Replace each match with `(f match)`. |

## Match accessors

| Symbol | Description |
| --- | --- |
| `(regex-match? x)` | True when `x` is a match record. |
| `(regex-match-start m)` | Start offset of the whole match. |
| `(regex-match-end m)` | End offset of the whole match. |
| `(regex-match-text m)` | Matched substring. |
| `(regex-match-group m i)` | Numbered group text. |
| `(regex-match-named m name)` | Named group text. |
| `(regex-match-span m)` | `(start . end)` of the whole match. |
| `(regex-match-group-span m i)` | `(start . end)` of group `i`. |
| `(regex:match-groups-hash m)` | All named groups as a hash map. |

