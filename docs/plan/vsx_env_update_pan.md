# VS Code Environment Panel Update Plan

[Back to VS Code reference](../guide/reference/vscode.md)

---

## 1. Goals

1. Add a first-class **Environment** panel in VS Code debug views, separate from the Heap Inspector.
2. When the Environment panel is open, it should **follow debugger navigation** (stack frame/thread changes).
3. Keep noise low by default:
   - globals hidden by default,
   - builtins hidden by default,
   - internal and nil entries hidden by default.
4. Provide user-controllable filters (checkbox-style choices) for:
   - locals/variables,
   - closures/upvalues,
   - globals,
   - builtins,
   - internal entries,
   - nil-valued entries.

## 2. Current State (as of May 2, 2026)

1. Heap inspection is already separate (`heapView.ts`).
2. The current `Memory` tree (`gcRootsTreeView.ts`) is effectively frame memory
   (locals, upvalues, module globals), not true heap roots.
3. Environment refresh currently targets `eta/localMemory` with `frameIndex: 0`,
   so it does not follow the selected stack frame.
4. Module globals are always requested (`includeModuleGlobals: true`).

## 3. UX Decisions

1. Keep Heap Inspector unchanged as its own WebView surface.
2. Introduce/rename the debug tree view label to **Environment**.
3. Environment behavior:
   - refresh on debugger `stopped`,
   - refresh on `debug.onDidChangeActiveStackItem` when view is visible,
   - follow selected thread/frame by default.
4. Default visible sections:
   - Locals: ON
   - Closures/Upvalues: ON
   - Globals: OFF
   - Builtins: OFF
5. Use VS Code native `ThemeIcon` codicons for professional tree presentation.
   Do not require external icon extensions.

## 4. Configuration Model

Add `eta.debug.environment.*` settings:

1. `followActiveFrame` (boolean, default `true`)
2. `showLocals` (boolean, default `true`)
3. `showClosures` (boolean, default `true`)
4. `showGlobals` (boolean, default `false`)
5. `showBuiltins` (boolean, default `false`)
6. `showInternal` (boolean, default `false`)
7. `showNil` (boolean, default `false`)

Add command:

- `Eta: Configure Environment Filters`
  - multi-select QuickPick (checkbox-style UX),
  - writes selected filters to `eta.debug.environment.*` settings.

## 5. API / DAP Plan

### Phase A (quick win, no protocol change)

Use existing `eta/localMemory` with better routing:

1. Track active frame/thread from `debug.activeStackItem`.
2. Call `eta/localMemory` with selected `threadId` + `frameIndex`.
3. Set `includeModuleGlobals` from `showGlobals`.
4. Apply client-side filtering for internal/nil visibility.

This delivers immediate lexical-navigation value with minimal churn.

### Phase B (target shape, protocol addition)

Add custom request `eta/environment`:

`eta/environment` request args (proposed):

```json
{
  "threadId": 1,
  "frameIndex": 0,
  "include": {
    "locals": true,
    "closures": true,
    "globals": false,
    "builtins": false,
    "internal": false,
    "nil": false
  },
  "limits": {
    "maxLocals": 200,
    "maxClosures": 200,
    "maxGlobals": 200,
    "maxBuiltins": 200
  }
}
```

Response shape (proposed):

```json
{
  "threadId": 1,
  "frameIndex": 0,
  "frameName": "foo",
  "moduleName": "bar",
  "scopes": [
    { "kind": "locals", "total": 3, "truncated": false, "variables": [] },
    { "kind": "closures", "total": 2, "truncated": false, "variables": [] },
    { "kind": "globals", "total": 0, "truncated": false, "variables": [] },
    { "kind": "builtins", "total": 0, "truncated": false, "variables": [] }
  ]
}
```

Keep `eta/localMemory` for compatibility and fallback.

## 6. Implementation Steps (Suggested PR Order)

1. **PR 1: View rename + follow-active-frame**
   - Rename debug view label from `Memory` to `Environment`.
   - Keep separate from Heap Inspector.
   - Wire `debug.onDidChangeActiveStackItem` refresh path.
   - Pass selected frame/thread to `eta/localMemory` calls.

2. **PR 2: Filter settings + config command**
   - Add `eta.debug.environment.*` settings.
   - Implement `Eta: Configure Environment Filters`.
   - Hide globals/builtins/internal/nil by default.

3. **PR 3: DAP `eta/environment`**
   - Add handler in `dap_server.cpp`.
   - Include builtins scope support.
   - Keep limits/truncation semantics aligned with existing memory requests.

4. **PR 4: Extension consumption of `eta/environment`**
   - Prefer `eta/environment` when supported.
   - Fallback to `eta/localMemory` for older adapters.
   - Add basic click-through hooks (e.g., inspect object when object id is available).

5. **PR 5: Docs + tests**
   - Update `editors/vscode/README.md`, `docs/guide/reference/vscode.md`.
   - Add/adjust extension tests for manifest contributions and filter behavior.
   - Add DAP tests for frame/thread-aware environment responses.

## 7. Quick Wins (1-2 day slice)

1. Rename panel to **Environment**.
2. Follow selected stack frame automatically when the view is open.
3. Hide globals by default (`showGlobals = false`).
4. Add initial settings-only toggles; command palette configurator can land next.

## 8. Risks and Mitigations

1. **Stop-time overhead**: only refresh environment when view is visible.
2. **Large scope payloads**: keep hard limits + truncated flags.
3. **Thread/frame mismatch**: always source from active stack item; fallback to main thread frame 0 when unavailable.
4. **User confusion between heap vs environment**: explicit naming in view titles and docs.

## 9. Definition of Done

1. Debug sidebar shows **Environment** and **Heap Inspector** as distinct concepts.
2. Environment view tracks stack frame changes while debugging.
3. Globals are hidden by default.
4. Users can toggle locals/closures/globals/builtins/internal/nil visibility.
5. Docs and tests cover the new behavior.

