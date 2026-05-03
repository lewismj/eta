# std.io

I/O conveniences and dynamic port redirection.

```scheme
(import std.io)
```

## Output

| Symbol | Description |
| --- | --- |
| `(println . xs)` | Display values separated by spaces, then newline. |
| `(eprintln . xs)` | Same as `println` but writes to standard error. |
| `(display->string x)` | Render `x` to a string using `display`. |

## Input

| Symbol | Description |
| --- | --- |
| `(read-line [port])` | Read a line of text. Returns the EOF object at end of stream. |

## Port redirection

| Symbol | Description |
| --- | --- |
| `(with-port port thunk)` | Run `thunk` with `port` installed as the current output port. |
| `(with-input-port port thunk)` | Run `thunk` with `port` as current input port. |
| `(with-output-to-port port thunk)` | Alias of `with-port`. |
| `(with-input-from-port port thunk)` | Alias of `with-input-port`. |
| `(with-error-to-port port thunk)` | Run `thunk` with `port` as current error port. |

## Deprecated

| Symbol | Replacement |
| --- | --- |
| `print` | `println` or `display`. |
| `display-to-string` | `display->string`. |

