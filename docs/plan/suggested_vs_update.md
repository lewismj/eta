# VS Code Extension — GC Roots Tree View Icon Refresh

[Back to plans index](./README.md)

---

## 1. Motivation

The Eta VS Code extension exposes the runtime heap as a tree view
("GC Roots" → `Stack` / `Globals` / module sub-scopes → individual
bindings). The current implementation in
[`editors/vscode/src/gcRootsTreeView.ts`](../../editors/vscode/src/gcRootsTreeView.ts)
draws every variable row with one of just two codicons:

```ts
item.iconPath = new ThemeIcon(hasChildren ? 'symbol-object' : 'symbol-variable');
```

The result is a visually flat tree where a procedure (`+`), a pair
(`(1 . 2)`), a string, a tensor, and a hash table all look identical.
The headline branches (`Stack`, `Globals`, `Globals (european)`) also
share `globe`/`references`-style icons that don't read as a
hierarchy.

This plan specifies a built-in-codicon-only refresh that:

1. Distinguishes scopes (`Stack`, `Globals`, modules, `Builtins`,
   `Locals`, `Closures`) at a glance.
2. Picks an icon per **Eta value type** (procedure, pair, vector,
   record, string, number, …) instead of per "has children".
3. Uses `ThemeColor` so icons inherit the user's active theme symbol
   palette — the same colours users already see in the Outline view.
4. Marks special heap states (cycles, `nil`, dead refs, pinned
   roots) with conventional codicons.

No new icon assets, no SVGs, no font dependencies — only the built-in
[VS Code codicon set](https://microsoft.github.io/vscode-codicons/dist/codicon.html).

---

## 2. Scope

### In scope

- Icon + colour mapping for `EnvironmentScopeNode` and
  `EnvironmentVariableNode` rows in `gcRootsTreeView.ts`.
- A small `variableIcon(v)` helper driven by the DAP variable's
  `type` field, with a value-string fallback heuristic.
- Tooltip polish via `MarkdownString` (rich `name = value` + `#id`
  preview).
- Cycle / shared-object handling (mark the back-edge, do not expand).
- `package.json` `view/item/context` menu hooks for pin / filter /
  collapse-all using built-in codicons.

### Out of scope

- Backend protocol changes. If `type` is missing on a variable we
  fall back to value-string sniffing; surfacing richer type info
  from the DAP server is a separate ticket.
- New tree views (e.g. heap graph, retainer paths).
- Custom SVG icons or theming beyond `ThemeColor`.
- Refactoring the data flow (`refreshFromSession`, `expandVariable`,
  caching) — pure presentation change.

---

## 3. Current State

Relevant excerpt from
[`gcRootsTreeView.ts`](../../editors/vscode/src/gcRootsTreeView.ts):

```ts
function scopeIcon(scopeKind: string): ThemeIcon {
    switch (scopeKind) {
        case 'locals':    return new ThemeIcon('symbol-variable');
        case 'closures':
        case 'upvalues':  return new ThemeIcon('references');
        case 'globals':   return new ThemeIcon('globe');
        case 'builtins':  return new ThemeIcon('symbol-function');
        default:          return new ThemeIcon('symbol-namespace');
    }
}

// ...inside getTreeItem for an EnvironmentVariableNode...
item.iconPath = new ThemeIcon(hasChildren ? 'symbol-object' : 'symbol-variable');
item.description = v.value;
item.tooltip = `${v.name} = ${v.value}`;
```

Effect in the screenshotted tree:

- `Stack` and `Globals` only differ by their (right-aligned) count
  badge.
- Every binding under `european` (`+`, `-`, `eq?`, `cons`, `car`,
  `cdr`, `pair?`, …) renders as the same generic variable glyph.
- `Object #18869` under `Stack` is indistinguishable from a procedure.

---

## 4. Proposed Mapping

### 4.1 Scope rows (top-level branches)

| Node | Current | New codicon | Theme colour | Notes |
|---|---|---|---|---|
| `GC Roots` (synthetic root, if surfaced) | — | `database` | `charts.foreground` | Roots of the heap graph. |
| `Stack` | — | `layers` | `symbolIcon.namespaceForeground` | Frames stack; alt `debug-stackframe`. |
| `Globals` (toplevel) | `globe` | `globe` *(keep)* | `symbolIcon.namespaceForeground` | Recognisable. |
| `Globals (modulename)` | `globe` | `symbol-module` (alias `package`) | `symbolIcon.moduleForeground` | Distinguishes module from toplevel. |
| `Closures` / `Upvalues` | `references` | `link` | `symbolIcon.referenceForeground` | "Captured binding". |
| `Locals` | `symbol-variable` | `bracket` | `symbolIcon.variableForeground` | Frame-local feel. |
| `Builtins` | `symbol-function` | `library` | `symbolIcon.functionForeground` | Visually distinct from user code. |
| Empty / status / message row | `info` | `info` *(keep)* | `descriptionForeground` | — |

### 4.2 Variable rows — by Eta runtime type

Branch on `v.type` (preferred) or sniff `v.value` as a fallback.

| Eta type | Codicon | Theme colour | Notes |
|---|---|---|---|
| Procedure / lambda | `symbol-method` | `symbolIcon.functionForeground` | Default for user procedures. |
| Builtin / primitive op | `zap` | `symbolIcon.functionForeground` | `+`, `cons`, `car`, … |
| Macro / syntax transformer | `symbol-event` | `symbolIcon.keywordForeground` | Or `wand`. |
| Pair / list / cons cell | `list-tree` | `symbolIcon.arrayForeground` | Linked structure. |
| Vector / array | `symbol-array` | `symbolIcon.arrayForeground` | — |
| Hash table | `symbol-structure` | `symbolIcon.structForeground` | Keyed container. |
| Record | `symbol-structure` | `symbolIcon.structForeground` | Same family; may add `notebook` later. |
| String | `symbol-string` | `symbolIcon.stringForeground` | — |
| Symbol (Scheme symbol) | `symbol-key` | `symbolIcon.keyForeground` | Interned name. |
| Number / int / float | `symbol-number` | `symbolIcon.numberForeground` | — |
| Boolean | `symbol-boolean` | `symbolIcon.booleanForeground` | — |
| Char | `symbol-text` | `symbolIcon.stringForeground` | — |
| Port (file/process/socket) | `plug` | `symbolIcon.interfaceForeground` | `radio-tower` for sockets if separable. |
| Tensor / torch object | `graph` | `symbolIcon.numberForeground` | Numeric block. |
| Continuation | `debug-step-back` | `symbolIcon.eventForeground` | Resumable control. |
| Promise / lazy | `clock` | `symbolIcon.eventForeground` | Deferred. |
| Environment / frame | `bracket-dot` | `symbolIcon.namespaceForeground` | Bindings container. |
| Foreign / opaque | `symbol-misc` | `symbolIcon.colorForeground` | Catch-all. |
| `nil` / `#nil` / `()` | `circle-slash` | `disabledForeground` | Greyed. |
| Cyclic back-edge (already-seen `#id`) | `arrow-circle-up` | `charts.yellow` | Mark cycle, do not expand. |
| Pinned / watched root | `pin` | `charts.blue` | Toggled via context menu. |

### 4.3 Right-side metadata

- `item.description` already shows the count badge for scopes.
  Extend variable rows to:

  ```ts
  item.description = `${v.value}   #${objectId}`;
  ```

  with `objectId` taken from `(v as any).objectId ?? v.variablesReference`.

- `item.tooltip` becomes a `MarkdownString`:

  ```text
  **<v.name>**
  `value: <v.value>`
  `#id : <objectId>`
  `type : <v.type>`
  ```

- For procedures, when arity is available, append
  `description += "  (n args)"`.

---

## 5. Reference Implementation Sketch

Drop-in replacement targeting
[`gcRootsTreeView.ts`](../../editors/vscode/src/gcRootsTreeView.ts):

```ts
import { ThemeColor, ThemeIcon } from 'vscode';

function scopeIcon(scopeKind: string): ThemeIcon {
    switch (scopeKind) {
        case 'locals':    return new ThemeIcon('bracket',
            new ThemeColor('symbolIcon.variableForeground'));
        case 'closures':
        case 'upvalues':  return new ThemeIcon('link',
            new ThemeColor('symbolIcon.referenceForeground'));
        case 'globals':   return new ThemeIcon('symbol-module',
            new ThemeColor('symbolIcon.moduleForeground'));
        case 'builtins':  return new ThemeIcon('library',
            new ThemeColor('symbolIcon.functionForeground'));
        case 'stack':     return new ThemeIcon('layers',
            new ThemeColor('symbolIcon.namespaceForeground'));
        case 'roots':     return new ThemeIcon('database');
        default:          return new ThemeIcon('symbol-namespace');
    }
}

const TYPE_ICON: Record<string, [string, string?]> = {
    procedure:    ['symbol-method',    'symbolIcon.functionForeground'],
    lambda:       ['symbol-method',    'symbolIcon.functionForeground'],
    builtin:      ['zap',              'symbolIcon.functionForeground'],
    macro:        ['symbol-event',     'symbolIcon.keywordForeground'],
    syntax:       ['wand',             'symbolIcon.keywordForeground'],
    pair:         ['list-tree',        'symbolIcon.arrayForeground'],
    list:         ['list-tree',        'symbolIcon.arrayForeground'],
    vector:       ['symbol-array',     'symbolIcon.arrayForeground'],
    hashtable:    ['symbol-structure', 'symbolIcon.structForeground'],
    record:       ['symbol-structure', 'symbolIcon.structForeground'],
    string:       ['symbol-string',    'symbolIcon.stringForeground'],
    symbol:       ['symbol-key',       'symbolIcon.keyForeground'],
    number:       ['symbol-number',    'symbolIcon.numberForeground'],
    integer:      ['symbol-number',    'symbolIcon.numberForeground'],
    float:        ['symbol-number',    'symbolIcon.numberForeground'],
    boolean:      ['symbol-boolean',   'symbolIcon.booleanForeground'],
    char:         ['symbol-text',      'symbolIcon.stringForeground'],
    port:         ['plug',             'symbolIcon.interfaceForeground'],
    tensor:       ['graph',            'symbolIcon.numberForeground'],
    continuation: ['debug-step-back',  'symbolIcon.eventForeground'],
    promise:      ['clock',            'symbolIcon.eventForeground'],
    environment:  ['bracket-dot',      'symbolIcon.namespaceForeground'],
    nil:          ['circle-slash',     'disabledForeground'],
};

function sniffType(value: string): string {
    if (value.startsWith('<procedure'))    return 'procedure';
    if (value.startsWith('#<vector'))      return 'vector';
    if (value.startsWith('#<tensor'))      return 'tensor';
    if (value.startsWith('#<hash'))        return 'hashtable';
    if (value.startsWith('#<record'))      return 'record';
    if (value.startsWith('#<port'))        return 'port';
    if (value.startsWith('('))             return 'pair';
    if (value.startsWith('"'))             return 'string';
    if (value.startsWith("'"))             return 'symbol';
    if (value === '#t' || value === '#f')  return 'boolean';
    if (value === '()' || value === '#nil') return 'nil';
    if (/^-?\d/.test(value))               return 'number';
    return 'object';
}

function variableIcon(v: DebugVariable): ThemeIcon {
    const declared = ((v as any).type as string | undefined)?.toLowerCase();
    const key = declared && TYPE_ICON[declared] ? declared : sniffType(v.value ?? '');
    const [icon, color] = TYPE_ICON[key] ?? ['symbol-misc'];
    return color ? new ThemeIcon(icon, new ThemeColor(color)) : new ThemeIcon(icon);
}
```

`getTreeItem` for a variable becomes:

```ts
if (element instanceof EnvironmentVariableNode) {
    const v = element.variable;
    const hasChildren = (v.variablesReference ?? 0) > 0;
    const item = new TreeItem(
        v.name,
        hasChildren ? TreeItemCollapsibleState.Collapsed : TreeItemCollapsibleState.None,
    );
    item.iconPath = variableIcon(v);
    const id = (v as any).objectId ?? v.variablesReference;
    item.description = id ? `${v.value}   #${id}` : v.value;
    const md = new MarkdownString();
    md.isTrusted = false;
    md.appendMarkdown(`**${v.name}**\n\n`);
    md.appendCodeblock(v.value ?? '', 'eta');
    if ((v as any).type) md.appendMarkdown(`\n\n_type:_ \`${(v as any).type}\``);
    if (id) md.appendMarkdown(`\n\n_id:_ \`#${id}\``);
    item.tooltip = md;
    item.contextValue = 'eta.gcRoots.variable';
    return item;
}
```

---

## 6. Cycle / Shared-Object Handling

Today, expanding a variable always issues a `variables` DAP request
and re-renders the children. For Eta heap graphs that contain cycles
this can blow up. The icon refresh is a good moment to add:

1. A `Set<number>` of `objectId`s already encountered along the
   current ancestor chain (track per-expansion via a parent pointer
   or by walking back through the tree).
2. When a variable's `objectId` is in that set, render it with
   `arrow-circle-up` + `charts.yellow`, set `collapsibleState =
   None`, and append `(cycle)` to the description.
3. On hover, the `MarkdownString` notes "Already shown — click to
   reveal in tree" and (stretch) supports a `command:` link that
   reveals the original node.

---

## 7. `package.json` Wiring

Add inline title-bar / item-context actions using built-in codicons:

```jsonc
"contributes": {
  "commands": [
    { "command": "eta.gcRoots.refresh",       "title": "Refresh Heap View",
      "icon": "$(refresh)" },
    { "command": "eta.gcRoots.collapseAll",   "title": "Collapse All",
      "icon": "$(collapse-all)" },
    { "command": "eta.gcRoots.toggleBuiltins","title": "Toggle Builtins",
      "icon": "$(library)" },
    { "command": "eta.gcRoots.toggleNil",     "title": "Show nil bindings",
      "icon": "$(circle-slash)" },
    { "command": "eta.gcRoots.pin",           "title": "Pin root",
      "icon": "$(pin)" },
    { "command": "eta.gcRoots.unpin",         "title": "Unpin root",
      "icon": "$(pinned)" }
  ],
  "menus": {
    "view/title": [
      { "command": "eta.gcRoots.refresh",     "when": "view == eta.gcRoots", "group": "navigation@1" },
      { "command": "eta.gcRoots.collapseAll", "when": "view == eta.gcRoots", "group": "navigation@2" },
      { "command": "eta.gcRoots.toggleBuiltins","when": "view == eta.gcRoots", "group": "navigation@3" }
    ],
    "view/item/context": [
      { "command": "eta.gcRoots.pin",
        "when":    "view == eta.gcRoots && viewItem == eta.gcRoots.variable",
        "group":   "inline" }
    ]
  }
}
```

All glyphs above are first-class codicons; no font additions needed.

---

## 8. Implementation Plan

Three small slices, each independently mergeable.

| # | Slice | Files touched | Effort |
|---|---|---|---|
| 1 | Scope-icon refresh + `variableIcon(v)` helper + value sniffing fallback | `editors/vscode/src/gcRootsTreeView.ts` | 0.5 d |
| 2 | `MarkdownString` tooltips, `#id` description, arity for procedures | `gcRootsTreeView.ts`, `dapTypes.ts` (optional `objectId` field) | 0.5 d |
| 3 | Cycle detection + `arrow-circle-up` rendering + `view/item/context` menu actions (refresh, collapse, pin, toggle builtins/nil) | `gcRootsTreeView.ts`, `extension.ts`, `package.json` | 1.0 d |
| **Total** | | | **≈ 2 engineering days** |

Each slice lands the extension in a runnable, demoable state.

---

## 9. Testing

- **Snapshot test** of `variableIcon` over a hand-crafted table of
  20 representative `DebugVariable` shapes (one per row in §4.2).
  Asserts the chosen `id` and `color` strings.
- **Sniff fallback** test: feed a `DebugVariable` with no `type`
  but representative `value` strings; assert the same icon as the
  typed equivalent.
- **Cycle test**: synthetic two-node cycle (`a → b → a`) drives the
  expansion path; assert the second `a` gets `arrow-circle-up` and
  `collapsibleState === None`.
- **Manual smoke** in `.vscode-test.mjs` against the existing DAP
  fixture session — verify the screenshot tree no longer renders all
  variables with the same glyph.

---

## 10. Risks

| Risk | Mitigation |
|---|---|
| DAP server doesn't populate `type`; sniffing misclassifies value strings | Keep a permissive default (`symbol-misc`), file a follow-up to surface `type` from the runtime. |
| Codicon names drift across VS Code versions | Pin against the engine version already in `package.json`; all icons used here are stable since v1.50+. |
| Theme colours invisible in some user themes | `ThemeColor` falls back to default symbol colours; users can override via `workbench.colorCustomizations`. |
| Cycle tracking accidentally hides legitimately repeated children (e.g. shared sub-tree that user wants expanded twice) | Track only along the ancestor chain, not globally; allow "Reveal original" command to navigate. |
| `MarkdownString` tooltip rendering cost on very wide trees | Cap value preview to ~200 chars before truncation; build the MarkdownString lazily in `getTreeItem`. |

---

## 11. Open Questions

1. **`type` field surface** — should we extend the DAP variable
   payload now (via `eta/environment` custom request) to include
   canonical `type` strings? Doing so removes the need for value
   sniffing. *Proposal:* yes, but in a follow-up ticket; this PR
   ships the sniff fallback so the visual win is immediate.
2. **Procedure arity** — is arity already in `v.value`
   (e.g. `<procedure + (2 args)>`) or does it need a new field?
   *Proposal:* parse `v.value` for `(\d+ args)`; add a structured
   field later.
3. **Pinning storage** — per-workspace `Memento` or per-session?
   *Proposal:* per-workspace, keyed by module + symbol name.
4. **Cycle detection scope** — per-expansion (cheap, local) or full
   heap walk with shared-object marker on first occurrence? *Proposal:*
   per-expansion ancestor chain in v1; full walk only if a user
   reports confusion.
5. **`Stack` icon choice** — `layers` or `debug-stackframe`?
   *Proposal:* `layers` for the scope branch, `debug-stackframe` for
   individual frame children if/when the tree exposes them.

---

## 12. Delivery Order

1. **PR 1** — Slice 1: scope + variable icon refresh with sniff
   fallback, no protocol change. Visual win from day one.
2. **PR 2** — Slice 2: MarkdownString tooltips, `#id` in description,
   arity badge for procedures.
3. **PR 3** — Slice 3: cycle detection + context menu / view-title
   commands (refresh, collapse-all, pin, toggle builtins/nil).
4. **(Follow-up, separate plan)** — Extend DAP `eta/environment`
   payload with a canonical `type` field; remove the sniff fallback.

---

## Appendix — Codicon Cheat Sheet (used here)

`database`, `layers`, `link`, `library`, `bracket`, `bracket-dot`,
`symbol-module`, `symbol-namespace`, `symbol-method`,
`symbol-event`, `symbol-array`, `symbol-structure`, `symbol-string`,
`symbol-key`, `symbol-number`, `symbol-boolean`, `symbol-text`,
`symbol-misc`, `symbol-variable`, `symbol-function`, `symbol-object`,
`zap`, `wand`, `list-tree`, `graph`, `clock`, `plug`,
`debug-step-back`, `circle-slash`, `arrow-circle-up`, `pin`,
`pinned`, `refresh`, `collapse-all`, `eye`, `eye-closed`, `info`.

All available out-of-the-box — no font additions, no asset shipping.

