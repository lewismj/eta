# VS Code Debug UI Update Plan (Environment Inspector + Styling Pass)

[Back to plan index](./README.md)

---

## 1. Objectives

1. Add a **dedicated Environment Inspector panel** (own webview tab surface, like Heap Inspector), with explicit lexical environment-chain traversal.
2. Keep Heap Inspector and Environment Inspector as separate tools with clear responsibilities.
3. Align the look and feel of debug trees/panels with the styling direction in
   [`suggested_vs_update.md`](./suggested_vs_update.md), unless a specific mapping conflicts with runtime data.
4. Clarify and harden Object Kinds behavior in Heap Inspector so it scales as more runtime kinds appear.

---

## 2. Clarification on Heap "Object Kinds" Table

Yes: the Object Kinds table is expected to grow/shrink dynamically as the paused heap contains more/fewer runtime kinds.

Current practical constraints:

1. Kind rows are bounded by `maxKindRows` in `eta/heapSnapshot` request options.
2. If many kinds exist, lower-volume kinds can be clipped by that limit.

Planned adjustment:

1. Keep dynamic behavior.
2. Surface truncation explicitly in UI ("showing X of Y kinds") when clipping occurs.
3. Raise default row cap if needed after profiling stop-time cost.

---

## 3. UX Target

### 3.1 New panel surface

Add **Eta: Show Environment Inspector** command that opens a webview tab:

1. Title: `Eta Environment Inspector`
2. Placement: `ViewColumn.Beside` (same behavior pattern as Heap Inspector)
3. Refresh lifecycle:
   - on-demand via toolbar refresh button,
   - optional auto-refresh on `stopped`,
   - follow active stack frame/thread when enabled.

### 3.2 Explicit lexical chain traversal

The Environment Inspector primary view is a chain/tree of lexical scopes for the selected frame:

1. `Frame locals`
2. `Closure parent #1`
3. `Closure parent #2` (etc, if present)
4. `Module scope`
5. `Builtins` (optional, hidden by default)

Each level:

1. shows visible bindings at that lexical level,
2. is expandable/collapsible,
3. supports variable expansion via `variablesReference`,
4. supports jump/inspect actions (see Section 6).

### 3.3 Filters and defaults

Defaults (noise-reduced):

1. Locals: ON
2. Closures: ON
3. Globals/module: OFF
4. Builtins: OFF
5. Internal names: OFF
6. Nil-valued entries: OFF

Filter controls:

1. Toolbar toggles in the Environment Inspector webview.
2. Settings-backed persistence via `eta.debug.environment.*`.
3. Command palette quick config remains as secondary entry point.

---

## 4. Data Contract (DAP)

Use a dedicated request (no compatibility mode needed):

1. Request: `eta/environment`
2. Inputs:
   - `threadId`
   - `frameIndex`
   - include flags (locals/closures/globals/builtins/internal/nil)
   - limits per section

3. Response shape must represent lexical chain explicitly:
   - ordered `environments[]` array (nearest -> outermost),
   - each environment has `kind`, `label`, `depth`, `bindings`, totals, truncation.

Suggested shape:

```json
{
  "threadId": 1,
  "frameIndex": 0,
  "frameName": "foo",
  "moduleName": "bar",
  "environments": [
    { "kind": "locals", "label": "Frame locals", "depth": 0, "total": 3, "truncated": false, "bindings": [] },
    { "kind": "closure", "label": "Closure parent #1", "depth": 1, "total": 2, "truncated": false, "bindings": [] },
    { "kind": "module", "label": "Module (bar)", "depth": 2, "total": 12, "truncated": false, "bindings": [] },
    { "kind": "builtins", "label": "Builtins", "depth": 3, "total": 0, "truncated": false, "bindings": [] }
  ]
}
```

---

## 5. Styling and Visual Consistency

Adopt the codicon/theming direction from [`suggested_vs_update.md`](./suggested_vs_update.md) for both:

1. Environment Inspector chain rows and binding rows.
2. Existing debug sidebar tree rows that remain.

Refinements for this phase:

1. Use only built-in codicons + `ThemeColor`.
2. Keep icon mapping deterministic by type; fallback to value sniffing only when type metadata is absent.
3. Keep dense debugger layout (compact rows, predictable spacing, no card-in-card composition).

From your screenshot context, specifically improve:

1. Scope hierarchy readability (scope nodes must not look like plain variable rows).
2. Root/object row differentiation.
3. Count badges + truncation indicators.

---

## 6. Interactions

Environment Inspector interactions:

1. Click variable with object identity -> `Inspect in Heap Inspector`.
2. Click function/procedure binding -> optional `Show Disassembly` for current/all.
3. Expand compound value -> DAP `variables` request.
4. Chain navigation controls:
   - collapse all,
   - expand nearest N levels,
   - follow-active-frame toggle.

State behavior:

1. If paused session unavailable, show clear idle state text.
2. If request fails, show non-blocking error in panel body and keep last good render until replaced.

---

## 7. Implementation Plan (PR slices)

### PR 1 - Environment Inspector webview shell

1. Add `environmentView.ts` panel class (patterned after `heapView.ts`).
2. Add `media/environment/{environment.html, environment.css, environment.js}`.
3. Add command `eta.showEnvironmentInspector`.
4. Add debug setting `eta.debug.autoShowEnvironment` (default `false`).

### PR 2 - Lexical chain data contract

1. Expand `eta/environment` response to explicit lexical chain (`environments[]`).
2. Include per-level totals/truncation and stable kind labels.
3. Add/normalize type hints per binding where available.

### PR 3 - Inspector rendering + frame follow

1. Render chain/tree in webview with environment-level grouping.
2. Wire follow-active-frame behavior using `debug.activeStackItem`.
3. Add toolbar filters; persist to `eta.debug.environment.*`.

### PR 4 - Cross-tool actions

1. Add "Inspect in Heap Inspector" from environment binding rows.
2. Add disassembly jump action for callable bindings where resolvable.
3. Add graceful fallback labels when object/function identity is unavailable.

### PR 5 - Styling pass alignment

1. Apply icon + `ThemeColor` mapping from `suggested_vs_update.md` to:
   - Environment Inspector rows,
   - sidebar environment tree rows.
2. Add tooltip polish (`MarkdownString` in tree; rich hover text in webview).
3. Add cycle/shared-reference markers where object ids repeat in a traversal branch.

### PR 6 - Heap Object Kinds polish

1. Add explicit kind-table truncation indicator (if capped).
2. Validate dynamic row growth with synthetic multi-kind workloads.
3. Tune `maxKindRows` default only if needed after stop-time measurement.

---

## 8. Testing Plan

1. Extension unit tests:
   - manifest command/view contributions,
   - settings read/write for environment filters,
   - follow-active-frame selection extraction.
2. DAP tests:
   - `eta/environment` returns ordered lexical chain for selected frame/thread,
   - include flags correctly hide/show globals/builtins/internal/nil.
3. UI smoke tests:
   - Environment Inspector opens and refreshes in paused session,
   - chain expands/collapses correctly,
   - inspect-in-heap action triggers heap panel focus.
4. Heap table checks:
   - Object Kinds row count changes with runtime composition,
   - truncation indicator appears when row cap is reached.

---

## 9. Risks and Mitigations

1. Stop-time latency from extra requests:
   - fetch only when inspector visible or auto-refresh enabled.
2. Incomplete type metadata:
   - icon fallback heuristics and neutral defaults.
3. Visual divergence between sidebar and webview:
   - single icon/style mapping table shared in implementation notes.
4. Data volume in large lexical chains:
   - per-scope limits + truncation UI.

---

## 10. Definition of Done

1. Environment browsing exists as a **dedicated webview panel** (not only sidebar rename).
2. Lexical chain is explicit and traversable level-by-level.
3. Filters/defaults behave as specified (globals/builtins/internal/nil hidden by default).
4. Styling aligns with `suggested_vs_update.md` icon/theming direction.
5. Heap Object Kinds behavior is documented in UI and handles truncation transparently.

