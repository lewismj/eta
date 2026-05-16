# Eta Environment Inspector UI Improvement Plan

## Goal

Improve the usability and readability of the Eta Environment Inspector webview without changing the Eta Dark theme palette. The focus is layout, interaction, information hierarchy, and debugging workflow.

## Current issues

- Filter controls are visually flat and not grouped by purpose.
- The current frame context is useful but easy to miss.
- Empty closure-parent sections consume attention and vertical space.
- Binding rows are readable for small values, but long vectors and heap values strain the layout.
- The `Heap` affordance is ambiguous: it is not immediately clear whether it is a type label or an action.
- There is a `Collapse All` action but no matching `Expand All` action.
- The view can become noisy when many closure parents are present.

## Recommended UI changes

### 1. Add a compact frame summary block

Replace the single inline frame label with a small summary section.

Current:

```text
Thread 1 Frame 0: <lambda@european:226:13>
```

Suggested:

```text
Current frame
Thread 1 · Frame 0 · <lambda>
european:226:13
```

Benefits:

- Makes the active debug context easier to scan.
- Separates frame identity from filter controls.
- Makes source location more readable.

### 2. Group filters into `Scope` and `Options`

Current controls are presented as one flat list.

Suggested grouping:

```text
Scope:   [x] Locals  [x] Closures  [ ] Module  [ ] Builtins
Options: [x] Follow frame  [ ] Internal  [ ] Nil  [ ] Changed only
```

Benefits:

- Clarifies which controls affect visible environment sources.
- Separates display options from scope selection.
- Leaves room for future controls without making the toolbar chaotic.

### 3. Add `Expand All`

Keep `Collapse All`, but add the inverse action.

Suggested toolbar:

```text
[Refresh] [Expand All] [Collapse All]
```

Benefits:

- Makes recovery from a fully collapsed state immediate.
- Helps users inspect all visible scopes after applying filters.

### 4. Hide or collapse empty scopes by default

Empty closure-parent sections currently occupy similar visual weight to populated sections.

Recommended behaviour:

- Collapse empty scopes by default.
- Add an optional `Hide empty` toggle.
- Render empty collapsed rows in a compact form.

Suggested compact row:

```text
▸ Closure parent #3 — empty
```

Benefits:

- Reduces noise in long closure chains.
- Keeps attention on scopes with useful bindings.
- Still allows empty scopes to be revealed when needed.

### 5. Make section headers more descriptive

Current section badges show only a number.

Suggested examples:

```text
▾ Frame locals                         2 bindings
▾ Closure parent #1                    1 binding
▸ Closure parent #3 — empty
```

Benefits:

- Makes the badge meaning explicit.
- Improves scanability.
- Makes empty scopes clear without requiring expansion.

### 6. Use consistent disclosure glyphs

Use tree-style disclosure glyphs instead of plain text characters.

Suggested:

```text
▾ expanded
▸ collapsed
```

Benefits:

- Better matches VS Code tree conventions.
- Reduces visual ambiguity with binding rows.

### 7. Improve binding rows with table-like alignment

Move toward a consistent two-column or three-column layout.

Suggested structure:

```text
Kind   Name             Value
#      vega-val         22.4859
▸      vg               #(-0.488997 0.793173 -35.6928 …)   [Heap ↗]
```

Benefits:

- Makes names and values easier to compare.
- Aligns numeric values more predictably.
- Gives heap-backed values a clear action area.

### 8. Make heap actions explicit

Replace the ambiguous `Heap` pill with an action label.

Suggested labels:

```text
[Heap ↗]
[Inspect heap]
```

Benefits:

- Makes it clear the control is clickable.
- Communicates that the action opens or focuses the heap inspector.

### 9. Add value truncation and expansion

Long vectors should be truncated in the row and expandable on demand.

Collapsed row:

```text
vg    #(-0.488997 0.793173 -35.6928 23.2861 …)    [Heap ↗]
```

Expanded value:

```text
vg
#(
  -0.488997
   0.793173
 -35.6928
  23.2861
  27.3302
)
```

Benefits:

- Prevents long values from dominating the row.
- Makes vectors and heap objects easier to inspect.
- Avoids unnecessary horizontal scrolling.

### 10. Add row hover actions

Show secondary actions only when the user hovers a binding row.

Suggested actions:

- Copy name
- Copy value
- Expand value
- Inspect heap object

Benefits:

- Keeps normal rows clean.
- Improves discoverability of useful debugger actions.
- Avoids permanent button clutter.

### 11. Add a binding filter input

Add a small search field above the scope list.

Suggested placeholder:

```text
Filter bindings…
```

Behaviour:

- Match binding names.
- Optionally match rendered values.
- Preserve scope grouping while filtering.

Benefits:

- Speeds up debugging in large frames.
- Reduces manual scanning through closure parents.

### 12. Preserve view state across refreshes

Preserve these values where possible:

- Expanded/collapsed sections.
- Expanded binding rows.
- Filter settings.
- Binding filter text.
- Scroll position.

Benefits:

- Prevents the view from resetting while stepping or refreshing.
- Makes `Follow frame` less disruptive.

### 13. Improve changed-value presentation

Because a `Changed Only` filter exists, changed bindings should also be visible in normal mode.

Possible indicators:

```text
● vega-val        22.4859
```

Or:

```text
vega-val          22.4859      changed
```

Benefits:

- Makes changes discoverable without hiding unchanged bindings.
- Gives `Changed Only` a clearer mental model.

### 14. Improve no-session and no-frame empty states

When there is no active Eta debug session or no selected frame, show a compact empty state rather than a sparse status line.

Suggested:

```text
No active Eta debug session

Start or pause an Eta debug session to inspect lexical environments.

[Run File] [Debug File]
```

Benefits:

- Makes the panel feel intentional rather than broken.
- Gives users a clear next action.
- Reduces confusion when the debugger is not paused.

## Suggested revised layout

```text
Eta Environment Inspector

[Refresh] [Expand All] [Collapse All]              Filter bindings…

Scope:   [x] Locals  [x] Closures  [ ] Module  [ ] Builtins
Options: [x] Follow frame  [ ] Internal  [ ] Nil  [ ] Changed only  [x] Hide empty

Current frame
Thread 1 · Frame 0 · <lambda>
european:226:13

▾ Frame locals                                      2 bindings
  Kind   Name          Value
  #      vega-val      22.4859
  ▸      vg            #(-0.488997 0.793173 -35.6928 …)     [Heap ↗]

▾ Closure parent #1                                1 binding
  #      vanna-from-delta                           -0.488997

▾ Closure parent #2                                1 binding
  #      vanna-from-delta                           -0.488997

▸ Closure parent #3 — empty
▸ Closure parent #4 — empty
```

## Suggested implementation order

1. Group filters into `Scope` and `Options`.
2. Add `Expand All` beside `Collapse All`.
3. Collapse empty scopes by default and add `Hide empty`.
4. Make section headers show `binding`, `bindings`, or `empty`.
5. Improve binding row layout and make `Heap` an explicit action.
6. Add value truncation with expandable long values.
7. Add binding search/filter input.
8. Preserve expansion, filter, and scroll state across refreshes.
9. Improve no-session and no-frame empty states.

## Non-goals

- Do not change the Eta Dark colour theme.
- Do not introduce a new visual palette for the inspector.
- Do not change debugger semantics or the data returned by the backend.
- Do not remove access to empty scopes; hide or collapse them only when requested/defaulted.
