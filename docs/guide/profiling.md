# Profiling

[<- Back to Guide](README.md)

Eta provides two runtime profiler modes:

- `sample` (default): sampled call stacks, lower overhead.
- `trace`: exact call/return accounting, higher overhead.

## CLI quickstart

Run sampled profiling (default) and write speedscope JSON:

```bash
eta prof run --out profile.speedscope.json path/to/program.eta
```

Run trace profiling and write an archive for later reporting/merge:

```bash
eta prof run --mode trace --format eta-prof --out run.eta-prof path/to/program.eta
```

Render reports from an archive:

```bash
eta prof report --format pretty run.eta-prof
eta prof report --format json run.eta-prof
eta prof report --format chrome run.eta-prof
```

Merge multiple runs:

```bash
eta prof merge --out merged.eta-prof run1.eta-prof run2.eta-prof
```

Open with speedscope:

```bash
eta prof view merged.eta-prof
```

`eta prof run` is a wrapper over `eta run --prof ...`. You can use either form.
`etai` also supports profiling directly with `--prof`, `--prof-hz`, `--prof-format`, and `--prof-out`.

### CLI surface

`eta prof` subcommands:

```text
eta prof run [--mode sample|trace] [--hz N] [--format FMT] [--out FILE] <file.eta|file.etac>
eta prof report [--format pretty|json|speedscope|chrome|pprof] FILE.eta-prof
eta prof merge --out OUT.eta-prof IN1.eta-prof IN2.eta-prof ...
eta prof view FILE.speedscope.json|FILE.eta-prof
```

Key `eta prof run` options:

- `--mode sample|trace`: profile mode (`sample` default).
- `--hz N`: sample frequency (sample mode only, default `1000`).
- `--format FMT`: one of `pretty|json|speedscope|eta-prof|chrome|pprof`.
- `--out FILE`: write report to a file.

Equivalent `eta run` flags:

```text
eta run --prof[=sample|trace] [--prof-hz N] [--prof-format FMT] [--prof-out FILE] [run args...]
```

## Notebook / Jupyter

`eta_jupyter` supports a `%%prof` cell magic that profiles one cell and renders
an inline flamegraph.

```text
%%prof [sample|trace] [--mode sample|trace] [--hz N]
<eta code...>
```

Examples:

```text
%%prof
(+ 1 2)
```

```text
%%prof trace
(fib 12)
```

```text
%%prof --mode=sample --hz=4000
(workload)
```

The notebook output includes the cell result plus a profiler report
(`text/plain`) and inline flamegraph HTML (`text/html`).

## REPL

`eta_repl` supports a one-shot `:prof` meta-command that profiles the next
submission.

```text
:prof [sample|trace] [--mode sample|trace] [--hz N] [--format FMT]
```

Examples:

```text
eta> :prof trace --format pretty
eta> (my-workload)
```

```text
eta> :prof sample --hz 2000 --format json
eta> (my-workload)
```

Use `:prof off` (or `:prof disable`) to clear a pending request.

## Formats

Supported report/export formats:

- `pretty`: text summary (`flat`, `tree`, plus `counters` when present).
- `json`: machine-readable summary with `flat`, `tree`, `counters`.
- `speedscope`: speedscope JSON.
- `eta-prof`: Eta archive JSON for offline report/merge/view.
- `chrome`: Chrome trace JSON.
- `pprof`: reserved for optional pprof export path.

## Report fields

`pretty`/`json` include `flat` and `tree` tables.

`flat` columns:

- `frame`: frame/function label.
- `self_ns`: self-time in nanoseconds.
- `inclusive_ns`: inclusive time in nanoseconds.
- `calls`: call count (trace mode) or weighted sample count (sample mode).
- `bytes_allocated`: coarse per-frame allocated bytes from allocator hooks.

`tree` columns:

- `parent`, `child`: caller/callee frame labels.
- `inclusive_ns`: edge-inclusive time.
- `calls`: edge call/sample count.

Notes:

- Default output format is `speedscope` in `sample` mode and `pretty` in `trace` mode.
- Current pprof writer is a stub and returns a clear runtime error message.
- `pretty` and `json` `flat` rows include `bytes_allocated` (coarse allocation
  bytes attributed per frame).

## std.prof

Import:

```scheme
(import std.prof)
```

Exports:

- `(prof/start [mode] [hz]) -> session`
- `(prof/stop session) -> handle | #f`
- `(prof/report handle [format]) -> string`
- `(prof/counter name n) -> #t`
- `(prof/with thunk [mode] [hz]) -> (values result handle)`
- `(prof/region name thunk) -> result`
- `(prof/enabled?) -> bool`

`mode` accepts `trace` or `sample` (symbol or string).  
`format` accepts `pretty`, `json`, `speedscope`, `chrome`, or `eta-prof`.
