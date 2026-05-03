# std.jupyter

Notebook-focused MIME display markers. Each constructor wraps a value in
a record that the kernel renders with the matching MIME type.

```scheme
(import std.jupyter)
```

## Generic

| Symbol | Description |
| --- | --- |
| `(jupyter:display value)` | Emit `value` using its default MIME bundle. |

## Text and graphics

| Symbol | Description |
| --- | --- |
| `(jupyter:html str)` | Render as HTML. |
| `(jupyter:markdown str)` | Render as Markdown. |
| `(jupyter:latex str)` | Render as LaTeX. |
| `(jupyter:svg str)` | Render an inline SVG document. |
| `(jupyter:png bytes)` | Render a PNG image from raw bytes. |

## Data

| Symbol | Description |
| --- | --- |
| `(jupyter:vega spec)` | Render a Vega-Lite specification. |
| `(jupyter:table data)` | Render a tabular value. |
| `(jupyter:plot spec)` | Render a chart spec. |

## Domain-specific

| Symbol | Description |
| --- | --- |
| `(jupyter:tensor t)` | Render a torch tensor. |
| `(jupyter:tensor-explorer t)` | Interactive tensor explorer. |
| `(jupyter:dag g)` | Render a causal DAG. |
| `(jupyter:heap)` | Snapshot of the runtime heap. |
| `(jupyter:disasm fn)` | Disassemble a procedure. |
| `(jupyter:actors)` | Snapshot of running actors. |

