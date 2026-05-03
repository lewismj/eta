# std.json

JSON read/write wrappers over the `%json-*` runtime primitives.

```scheme
(import std.json)
```

| Symbol | Description |
| --- | --- |
| `(json:read port)` | Parse a JSON value from `port`. |
| `(json:read-string str)` | Parse a JSON value from a string. |
| `(json:write value port)` | Serialise `value` to `port`. |
| `(json:write-string value)` | Serialise `value` to a string. |

JSON objects map to hash maps, arrays to lists, `null` to `'null`, and
booleans to `#t` / `#f`.

