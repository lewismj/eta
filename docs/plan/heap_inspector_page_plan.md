# Eta Heap Inspector UI Improvement Plan

## Goal

Improve the usability and readability of the Eta Heap Inspector webview without changing the Eta Dark theme palette. The focus is layout, interaction, information hierarchy, and heap-debugging workflow.

## Current issues

- The top toolbar mixes title, filtering, baseline controls, diff controls, and refresh into one dense row.
- The memory and cons pool gauges are useful, but their labels and numeric details could be easier to scan.
- Gauge percentages are visually separated from the gauge body and can be missed.
- The object-kind table is compact, but it lacks clear affordances for sorting and drilling into a kind.
- `BYTES v` suggests sorting, but the visual treatment is subtle.
- GC Roots can become very long and dominate the page.
- Root groups and individual roots are shown in the same visual language, which makes hierarchy less clear.
- Object id pills are useful, but it is not obvious whether they are copyable, clickable, or navigational.
- The page has no obvious “focus object”, “selected object”, or “details” area when drilling into heap data.
- The baseline/diff workflow is present but not self-explanatory when disabled.

## Recommended UI changes

### 1. Split the toolbar into title, search, and actions

Current toolbar content is horizontally dense:

```text
ETA HEAP INSPECTOR   Filter kinds...   Capture Baseline   Diff   Clear Baseline   Refresh
```

Suggested structure:

```text
Eta Heap Inspector
[Filter kinds or roots…                         ]  [Refresh]

Baseline: [Capture] [Diff] [Clear]
```

Or for a single-row compact layout:

```text
Eta Heap Inspector      Filter kinds or roots…          [Refresh] [Baseline ▾]
```

Benefits:

- Makes the page title readable.
- Gives search/filter enough width.
- Separates normal refresh from baseline/diff actions.
- Reduces the “everything is a button row” feel.

### 2. Make baseline state explicit

When `Diff` and `Clear Baseline` are disabled, explain why.

Suggested states:

```text
Baseline: none captured      [Capture]
Baseline: captured 12:41:08  [Diff] [Clear]
Diff: comparing current heap to baseline from 12:41:08
```

Benefits:

- Makes the baseline/diff workflow discoverable.
- Prevents disabled controls from looking broken.
- Gives users confidence that the captured baseline is active.

### 3. Convert memory gauges into summary cards

The gauges are strong, but the details can be better structured.

Suggested memory card:

```text
Memory                         0.2%
237.6 KB used / 150.00 MB soft limit
Free headroom: 149.77 MB
[ gauge bar ]
```

Suggested cons pool card:

```text
Cons pool                      0.5%
41 / 8,192 cells used
Free: 8,151 cells · 656 B
[ gauge bar ]
```

Benefits:

- Keeps the percentage with the label.
- Improves readability of used/free/limit values.
- Makes the gauge the visual summary rather than the only structure.

### 4. Add threshold/status text to gauges

Show a simple status label near each gauge.

Examples:

```text
Memory                         0.2%   Healthy
Cons pool                      0.5%   Healthy
```

Potential states:

- Healthy
- Elevated
- Near limit
- Critical

Benefits:

- Users do not need to infer status from percentage alone.
- Supports future warning/critical thresholds.
- Helps screenshots communicate state quickly.

### 5. Make object-kind rows clickable drill-down targets

The object kind table should be a navigation point, not just a summary.

Suggested behaviour:

- Clicking `Closure` filters or opens Closure objects.
- Clicking `Cons` filters or opens Cons objects.
- Clicking the count opens the object list for that kind.
- Clicking bytes sorts or filters by retained/allocated size if supported.

Suggested row affordance:

```text
Closure      95      3.7 KB     View objects ›
```

Benefits:

- Turns the summary into a useful exploration entry point.
- Reduces the need to manually type kind filters.
- Makes heap navigation more discoverable.

### 6. Clarify table sorting

Current `BYTES v` is compact but subtle.

Suggested header:

```text
Kind             Count        Bytes ↓
```

On hover or focus:

```text
Sort by bytes descending
```

Benefits:

- Makes the active sort clear.
- Aligns with common table conventions.
- Avoids ambiguity between a glyph and data.

### 7. Add quick summary totals above object kinds

Add a compact total row or summary line before the object kind table.

Suggested:

```text
Object kinds: 6 · Objects: 2,011 · Bytes: 27.8 KB
```

Benefits:

- Gives an immediate heap-shape summary.
- Avoids requiring mental addition across rows.
- Helps compare before/after baseline diffs.

### 8. Separate GC Roots into a dedicated explorer section

GC Roots can become long and should behave like a tree explorer.

Suggested header:

```text
GC Roots                         549 roots
[Filter roots…]
```

Suggested groups:

```text
▾ Stack                           1
▾ Globals                         521
  ▾ european                      8
  ▾ std.aad                       19
```

Benefits:

- Makes root exploration feel like a tree.
- Gives root filtering a clearer home.
- Prevents roots from visually blending with summary cards.

### 9. Add `Expand All` and `Collapse All` for roots

Provide root-specific tree controls.

Suggested:

```text
GC Roots        [Expand All] [Collapse All] [Hide empty]
```

Benefits:

- Gives users control over large root trees.
- Matches the Environment Inspector plan.
- Reduces scrolling when many namespaces are present.

### 10. Preserve root expansion state across refreshes

Preserve these values where possible:

- Expanded/collapsed root groups.
- Object-kind sort column and direction.
- Kind/root filter text.
- Scroll position.
- Selected object, if any.

Benefits:

- Prevents the page from jumping while debugging.
- Makes refresh safe during heap investigation.
- Keeps users oriented in long root lists.

### 11. Make object id pills explicitly actionable

Object id pills such as `#10108` are valuable but ambiguous.

Suggested behaviour:

- Click: select/open object details.
- Hover: show `Inspect object #10108`.
- Context menu or row action: copy object id.

Suggested visual label for expanded affordance:

```text
#10108 ↗
```

Benefits:

- Makes navigation discoverable.
- Encourages heap-object inspection from roots.
- Avoids treating ids as inert labels.

### 12. Add an object details pane or inline details card

When a root or object id is selected, show details without losing context.

Possible layouts:

#### Inline card

```text
Object #10108
Kind: Closure
Size: 40 B
Referenced by: Globals › european › branch-primal
References: #10109, #10110
```

#### Split layout

```text
Left: summaries and roots
Right: selected object details
```

Benefits:

- Gives object ids somewhere meaningful to navigate.
- Reduces context switching to separate views.
- Makes heap exploration feel complete.

### 13. Add root path breadcrumbs for selected objects

When selecting an object under roots, show its path.

Suggested:

```text
Globals › european › branch-primal › #10108
```

Benefits:

- Keeps users oriented in nested root trees.
- Makes selected objects easier to discuss and debug.
- Helps when multiple roots point to nearby objects.

### 14. Improve root row layout

Root rows currently show name on the left and object id pill on the right. This is compact, but for long names and nested groups a table-like structure will scale better.

Suggested structure:

```text
Name                 Object       Kind       Size
branch-primal        #10108       Closure    40 B
norm-cdf             #10110       Closure    40 B
params               #5           Vector     72 B
```

Benefits:

- Makes object metadata visible without opening every object.
- Aligns ids and sizes consistently.
- Helps compare roots quickly.

### 15. Add row hover actions for roots and objects

Show secondary actions only on hover.

Suggested actions:

- Inspect object
- Copy id
- Copy root path
- Filter to kind
- Show references/referrers, if supported

Benefits:

- Keeps dense rows clean.
- Makes common debugger actions discoverable.
- Avoids permanent button clutter.

### 16. Add kind/root filter modes

The existing `Filter kinds...` field is useful but appears scoped to object kinds only. Consider either broadening it or making the scope explicit.

Options:

```text
Filter kinds…
Filter roots…
Filter heap…
```

Or:

```text
[All ▾] Filter heap…
```

Modes:

- All
- Kinds
- Roots
- Object ids
- Modules/namespaces

Benefits:

- Makes filtering predictable.
- Reduces need for multiple search boxes if space is tight.
- Helps users find roots as well as object kinds.

### 17. Add diff-specific columns when diff mode is active

When comparing to a baseline, show changes directly in tables and cards.

Suggested object-kind diff table:

```text
Kind          Count     Δ Count     Bytes      Δ Bytes
Primitive     1,864     +12         23.0 KB    +144 B
Closure          95      -3          3.7 KB    -120 B
```

Suggested gauge details:

```text
Memory 237.6 KB used   Δ +12.4 KB since baseline
```

Benefits:

- Makes baseline capture immediately useful.
- Avoids requiring users to infer changes manually.
- Supports leak/debug workflows.

### 18. Add empty and loading states

Show intentional states for no heap snapshot or failed refresh.

Examples:

```text
No heap snapshot available

Run or pause an Eta program, then refresh the heap inspector.

[Refresh]
```

```text
Unable to load heap snapshot

The debugger did not return heap data.
[Retry] [Show details]
```

Benefits:

- Avoids blank or partially-rendered panels.
- Gives users clear next actions.
- Helps distinguish no data from broken UI.

## Suggested revised layout

```text
Eta Heap Inspector

[Filter heap…                                      ] [Refresh] [Baseline ▾]
Baseline: none captured

Memory                                             0.2%  Healthy
237.6 KB used / 150.00 MB soft limit · Free headroom 149.77 MB
[ gauge bar ]

Cons pool                                          0.5%  Healthy
41 / 8,192 cells used · Free 8,151 cells · 656 B
[ gauge bar ]

Object kinds: 6 · Objects: 2,011 · Bytes: 27.8 KB

Kind             Count        Bytes ↓      Action
Primitive        1,864        23.0 KB      View objects ›
Closure             95         3.7 KB      View objects ›
Cons                41          656 B      View objects ›
Tape                 3           96 B      View objects ›
Port                 5           80 B      View objects ›
Vector               3           72 B      View objects ›

GC Roots                                      549 roots
[Filter roots…] [Expand All] [Collapse All]

▾ Stack                                         1
  Object #10157                                 [Inspect]

▾ Globals                                     521
  ▾ european                                    8
    branch-primal       #10108                  [Inspect]
    norm-cdf            #10110                  [Inspect]
    norm-pdf            #10109                  [Inspect]
    params              #5                      [Inspect]

  ▾ std.aad                                     19
    ad-max              #9625                   [Inspect]
    ad-min              #9626                   [Inspect]
```

## Suggested implementation order

1. Split the toolbar into title/search/actions and make baseline state explicit.
2. Convert memory and cons pool gauges into clearer summary cards.
3. Add object-kind total summary and clearer sort indicators.
4. Make object-kind rows clickable drill-down targets.
5. Add root-specific `Expand All` and `Collapse All` controls.
6. Preserve root expansion state, filters, sort, scroll, and selected object across refreshes.
7. Make object id pills explicitly clickable with hover tooltips/actions.
8. Add root filtering and clarify whether the main filter covers kinds, roots, or all heap data.
9. Add an inline selected-object details card.
10. Add diff-specific columns and gauge deltas when diff mode is active.
11. Improve no-snapshot, loading, and error states.

## Non-goals

- Do not change the Eta Dark colour theme.
- Do not introduce a new visual palette for the heap inspector.
- Do not change heap snapshot semantics or debugger backend data formats unless required by a specific follow-up implementation task.
- Do not remove access to GC roots or object ids; collapse, filter, or summarize them only when users can still reveal details.
- Do not make baseline/diff controls less accessible; make their state and purpose clearer.
