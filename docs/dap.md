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
- `cancel`
- `terminateThreads`
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
- `supportsCancelRequest: true`
- `supportsSteppingGranularity: true`
- `supportsTerminateThreadsRequest: true`

## Cancellation

- `cancel` is accepted for `requestId`-based request cancellation.
- `eta/heapSnapshot` checks cancellation and returns a structured error when cancelled:
  `{ "error": { "id": 2020, "format": "Request cancelled" } }`.

## Stepping Granularity

- `next` and `stepIn` accept `granularity: "instruction"` and use
  instruction-level stepping in the VM debug core.
- `stepOut` accepts the field for DAP compatibility and retains its
  depth-based semantics.

## Thread Termination

- `terminateThreads` is accepted and wired to in-process actor-thread
  termination.
- Current behavior is best-effort: it closes the actor mailbox socket and
  detaches the backing `std::thread` handle to avoid blocking the adapter.

## Breakpoint Locations

`breakpointLocations` returns line numbers that are executable according to
bytecode source maps currently loaded in the driver.

- Input: `source.path`, `line`, optional `endLine`.
- Output: `body.breakpoints[]` with `{ "line": <line-number> }`.

## Testing

- `eta/test/src/dap_tests.cpp` includes both synchronous framing tests and an
  async harness for paused-session request/event flows.
- The async harness currently covers a stop-on-entry round trip:
  `initialize` -> `launch` -> `configurationDone` -> `stopped` ->
  `continue` -> `terminated`.
