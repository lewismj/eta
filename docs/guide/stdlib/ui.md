# std.ui

[← Stdlib Reference](./README.md) · [Language Guide](../../language_guide.md) · [std.chart](./chart.md)

Declarative interactive applications. A program describes its interface as a
tree of nodes built from its state; `ui:run` serves the front end from the Eta
process itself, sends the tree to every connected page, runs event handlers
and re-renders when the state changes.

```scheme
(import std.ui)

(define state (ui:state 'count 0))

(defun view (s)
  (ui:window "Counter"
    (ui:column
      (ui:stat "Count" (ui:get s 'count) 'format "int")
      (ui:button "Add one"
        'on-click (lambda (ev) (ui:update! s 'count (lambda (n) (+ n 1))))))))

(ui:run view state)
```

Run it with `etai counter.eta`. The page opens in the browser; `--ui window`
opens a standalone window instead, and `--ui none` only prints the URL (for
VS Code's *Eta: Open App* command, tests and remote use).

## Writing elements

Every builder takes properties as `'name value` pairs and children as
further arguments, in any order. Lists of children are spliced in, and `#f`
and `'()` are ignored, so `(if cond node #f)` and `(map ...)` work inline.

- A property whose value is a procedure becomes an **event handler**.
- A property whose name starts with `%` stays in Eta and is never sent.
- `'key` gives a child a stable identity, so the front end can animate it.

## Layout and content

| Symbol | Description |
| --- | --- |
| `(ui:window title . children)` | Root of an app. `'id` names the app (port, saved session, reconnect); defaults to the title. `'window-width`, `'window-height` size the standalone window. |
| `(ui:row . children)` / `(ui:column . children)` | Flex layout. `'width` (px), `'height` (px), `'flex` (grow factor; `0` sizes to content). |
| `(ui:panel . children)` / `(ui:card . children)` | Surface with an optional `'title`. |
| `(ui:heading text)` / `(ui:text text)` | Text; `(ui:text s 'muted #t)` for secondary notes. |
| `(ui:stat label value)` | Headline figure. `'format`: `"num"`, `"pct"`, `"bp"`, `"int"`, `"usd"`. |
| `(ui:kv rows)` | Key/value list from `(label . value)` pairs. |
| `(ui:button label)` | `'on-click`, `'variant "primary"`. |
| `(ui:divider)` / `(ui:spacer)` | Separators. |

## Inputs

Input handlers receive the new value directly.

| Symbol | Description |
| --- | --- |
| `(ui:slider label)` | `'min`, `'max`, `'step`, `'value`, `'format`, `'on-change`. Sends live (throttled) updates while dragging; `'live #f` sends only on release. |
| `(ui:select label options)` | `options`: strings or `(value . label)` pairs. `'value`, `'on-change` (value string). |
| `(ui:toggle label)` | `'value` (`#t`/`#f`), `'on-change` (boolean). |

## Tables

`(ui:table rows columns . props)` shows a sortable, resizable, virtualised
table. `columns` is a list of specs: a field symbol, or
`(field "Label" [format [align]])`. Rows are hash maps, association lists, or
anything readable with accessor procedures (see `ui:field`). Props: `'key`
(field identifying rows), `'selected` (a key), `'on-select` (called with the
original row), `'title`, `'height`.

## State and events

| Symbol | Description |
| --- | --- |
| `(ui:state key value ...)` | Mutable application state. |
| `(ui:get s key [default])` / `(ui:set! s key v)` / `(ui:update! s key f)` | Read and change state; changes re-render. |
| `(ui:event-ref ev key [default])` | Read a raw event payload field. |
| `(ui:field row spec)` | Read a field from a hash map, alist, or via a procedure. |

## Running

| Symbol | Description |
| --- | --- |
| `(ui:run view state)` | Serve the app and block until its last page or window closes. Returns the final state. |
| `(ui:url)` | URL of the running app. |
| `(ui:render node)` | The wire form of a tree plus its handler table (for tests). |

**Rerun behaviour.** Each app keeps the same port and token between runs. A
page left open reconnects to the next run and animates to the new data; if a
previous run still holds the port, the new run asks it to quit.

---

**Source:** [`stdlib/std/ui.eta`](../../../stdlib/std/ui.eta) ·
native server: [`packages/stdlib/native/ui`](../../../packages/stdlib/native/ui) ·
front end: [`eta/tools/ui/frontend`](../../../eta/tools/ui/frontend)
