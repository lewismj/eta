# Eta DAP Reference

This document tracks the current Debug Adapter Protocol surface for `eta_dap`.

## Requests

### Standard requests

- `initialize`
- `launch`
- `setBreakpoints`
- `setFunctionBreakpoints`
- `breakpointLocations`
- `setExceptionBreakpoints`
- `configurationDone`
- `threads`
- `stackTrace`
- `scopes`
- `variables`
- `continue`
- `next`
- `stepIn`
- `stepOut`
- `pause`
- `evaluate`
- `setVariable`
- `disassemble`
- `restart`
- `completions`
- `terminate`
- `disconnect`

### Eta custom requests

- `eta/heapSnapshot`
- `eta/inspectObject`
- `eta/disassemble`
- `eta/childProcesses`

`eta/disassemble` response includes:

- `text`: textual disassembly
- `functionName`: current function name when available
- `currentPC`: paused instruction index (or `-1` when unavailable)

## Capabilities

Current `initialize` capability flags:

- `supportsConfigurationDoneRequest: true`
- `supportsSetBreakpoints: true`
- `supportsBreakpointLocationsRequest: true`
- `supportsTerminateRequest: true`
- `supportsEvaluateForHovers: true`
- `supportsCompletionsRequest: true`
- `supportsFunctionBreakpoints: true`
- `supportsConditionalBreakpoints: true`
- `supportsHitConditionalBreakpoints: true`
- `supportsLogPoints: true`
- `supportsSetVariable: true`
- `supportsRestartRequest: true`
- `supportsDisassembleRequest: true`

## Breakpoint Locations

`breakpointLocations` returns line numbers that are executable according to
bytecode source maps currently loaded in the driver.

- Input: `source.path`, `line`, optional `endLine`.
- Output: `body.breakpoints[]` with `{ "line": <line-number> }`.
