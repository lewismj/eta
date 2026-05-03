# std.process

Subprocess management built on the `%process-*` runtime primitives.

```scheme
(import std.process)
```

## Run synchronously

| Symbol | Description |
| --- | --- |
| `(process:run argv . opts)` | Run `argv`, wait for completion, return exit code. |
| `(process:run-string argv . opts)` | Run `argv`, capture stdout, return string. |

Common options: `'stdin`, `'stdout`, `'stderr`, `'env`, `'cwd`.

## Spawn asynchronously

| Symbol | Description |
| --- | --- |
| `(process:spawn argv . opts)` | Start a process and return a handle. |
| `(process:wait handle)` | Wait for completion. |
| `(process:kill handle)` | Force-terminate. |
| `(process:terminate handle)` | Request graceful termination. |

## Inspecting a handle

| Symbol | Description |
| --- | --- |
| `(process:handle? x)` | True when `x` is a process handle. |
| `(process:pid handle)` | OS process id. |
| `(process:alive? handle)` | True when the child is still running. |
| `(process:exit-code handle)` | Exit code, or `#f` if not terminated. |
| `(process:stdin-port handle)` | Writable port to child stdin. |
| `(process:stdout-port handle)` | Readable port from child stdout. |
| `(process:stderr-port handle)` | Readable port from child stderr. |

