# std.chart

[← Stdlib Reference](./README.md) · [Language Guide](../../language_guide.md) · [std.ui](./ui.md)

GPU-rendered charts for [std.ui](./ui.md) apps (WebGPU, falling back to
WebGL2). Marks animate between renders: keyed points and bars move and
resize, lines and areas morph point by point, heatmap cells recolour.

```scheme
(import std.ui std.chart)

(chart:plot
  'title "Exposure profile"
  'x (chart:axis "Years" 'format "num")
  'y (chart:axis "Exposure" 'format "usd")
  (chart:band times p05 p95 'series "EPE")
  (chart:line times epe 'label "EPE")
  (chart:line times ene 'label "ENE"))
```

## Plots and marks

| Symbol | Description |
| --- | --- |
| `(chart:plot . marks)` | 2D plot. `'title`, `'subtitle`, `'x`/`'y` axes, `'height`. Wheel zooms x, drag pans, double-click resets. Hover shows a crosshair (lines) or a tooltip (points, bars). |
| `(chart:axis label)` | `'format`, `'min`, `'max`, `'zero`. |
| `(chart:line xs ys)` | Line; `'label` (legend), `'width`, `'emphasis`. Non-numbers break the line. |
| `(chart:area xs ys)` | Fill to `'baseline` or `'lower`; `'fill "gradient"` (default) or `"band"`, `'opacity`, `'series` (share a line's colour). |
| `(chart:band xs lo hi)` | Flat band between two series, e.g. a quantile range. |
| `(chart:points rows)` | Scatter: `'x`, `'y`, `'key` field specs (required), `'label`, `'size`, `'selected`, `'on-select` (row). |
| `(chart:bars categories amounts)` | Horizontal bars keyed by category; `'selected`, `'on-select` (category). |
| `(chart:heatmap rows cols cells)` | Matrix of coloured cells; `cells` is a list of rows or a flat row-major vector. `'scale "diverging"` (default, symmetric around 0) or `"sequential"`, `'min`, `'max`, `'format`, `'show-values`, `'selected-row`, `'on-select` (row label, column label). |
| `(chart:surface xs ys zs)` | Lit 3D surface; `zs` has `(length ys)` rows of `(length xs)` values. `'x`, `'y`, `'z` axes. Drag to orbit, wheel to zoom, hover for values. |

## Colour

Colours follow a validated, colour-vision-deficiency-safe palette: series take
eight fixed slots in order (never cycled), sequential scales use one hue, and
diverging scales run blue to red through a neutral grey. Text always uses
text colours, never series colours.

## Data

Numeric columns of 16 or more values are sent as binary Float64 buffers, so
long series and large grids cost no JSON encoding.

---

**Source:** [`stdlib/std/chart.eta`](../../../stdlib/std/chart.eta)
