# Logging Plan - `std.log` via spdlog 1.17.0

[Back to next-steps](../next-steps.md) ·
[Modules and Stdlib](../guide/reference/modules.md) ·
[JSON](../guide/reference/json.md) ·
[OS Primitives](../guide/reference/os.md)

---

## 1. Goals

Ship structured, level-aware, multi-sink logging as `std.log`, the second
slice of hosted-platform Phase H3 (after `std.json`), with behavior that fits
Eta's current runtime architecture:

1. **Port-native integration first.**
   Logging must respect Eta ports (`current-error-port`, callback ports in DAP/Jupyter,
   dynamic redirection via `with-error-to-port`).
2. **Registration discipline matches current runtime.**
   `builtin_names.h` order and runtime patch order must stay in lock-step.
3. **Scheme-idiomatic API surface.**
   Per-level functions support both default logger and explicit logger forms.
4. **No unsafe global teardown from `Driver` destructors.**
   Do not call process-global `spdlog::shutdown()` from every `Driver`.

### Functional coverage (v1)

| Capability | API surface (Eta) | spdlog/runtime feature |
|---|---|---|
| Levels: `trace`, `debug`, `info`, `warn`, `error`, `critical`, `off` | `log:trace` ... `log:critical`, `log:set-level!`, `log:level` | `spdlog::level::level_enum`, `set_level` |
| Per-logger filtering | `log:set-level!`, `log:level` | `logger::set_level`, `logger::level` |
| Console sink (color/plain) | `log:make-stdout-sink`, `log:make-stderr-sink` | `stdout/stderr(_color)_sink_mt` |
| File sink (append/truncate) | `log:make-file-sink` | `basic_file_sink_mt` |
| Rotating file sink | `log:make-rotating-sink` | `rotating_file_sink_mt` |
| Daily file sink | `log:make-daily-sink` | `daily_file_sink_mt` |
| **Port sink (sync only)** | `log:make-port-sink`, `log:make-error-port-sink` | custom sink writing via Eta `Port` API |
| Pattern formatting | `log:set-pattern!` | `logger::set_pattern` |
| Human vs JSON formatter | `log:set-formatter!` | logger-local mode + `%log-emit` formatting |
| Structured payload (alist/hash-map) | `(log:info ... payload)` | encoded via Eta JSON utilities |
| Async logger | `log:make-async-logger` | `spdlog::async_logger`, thread pool |
| Multi-sink logger composition | `log:make-logger`, `log:make-async-logger` | `logger(name, begin, end)` |
| Flush control | `log:flush!`, `log:flush-on!` | `logger::flush`, `logger::flush_on` |
| Default logger per VM | `log:default`, `log:set-default!` | VM-scoped default logger table |

### Non-goals (v1)

- User-defined sink callbacks from Eta.
- syslog/journald/Windows event log sinks.
- MDC/nested diagnostics context.
- Port sinks on async loggers (explicitly deferred to v2).

---

## 2. Decisions (resolved)

1. **Per-level call shape (Scheme idiomatic):**
   - `(log:info msg)`
   - `(log:info msg payload)`
   - `(log:info logger msg)`
   - `(log:info logger msg payload)`
   Explicit logger argument is first when present.
2. **Port sinks in v1:** sync loggers only. Async logger creation fails if any sink is a port sink.

---

## 3. Dependency and Build Plumbing

## 3.1 `cmake/FetchSpdlog.cmake`

Mirror current fetch style (`FetchNng.cmake`, `FetchXeus.cmake`):

- Pin `ETA_SPDLOG_TAG` to `v1.17.0`.
- Prefer preinstalled `find_package(spdlog CONFIG QUIET)`.
- Use bundled fmt (`SPDLOG_FMT_EXTERNAL=OFF`).
- Disable tests/examples/bench/install.
- Build shared on Windows (for DLL copy/install parity), allow default on Unix.

## 3.2 Top-level `eta/CMakeLists.txt`

Add:

- `include(../cmake/FetchSpdlog.cmake)`
- `add_subdirectory(log)` after existing runtime subsystems
- `eta_copy_log_dlls(<target>)` function near torch/nng helpers
- `install(FILES $<TARGET_FILE:spdlog> DESTINATION bin)` on Windows

## 3.3 Link targets that compile `Driver` / runtime entrypoints

Add `eta_log` anywhere we currently link `eta_core eta_torch eta_nng eta_stats`, including:

- `etai`, `etac`, `eta_repl`, `eta_lsp`, `eta_dap`, `eta_jupyter`, `eta_test`, `eta_test_runner`, `eta_core_test`

And call `eta_copy_log_dlls(<target>)` for the same executable set on Windows.

## 3.4 Packaging scripts

Update:

- `scripts/build-release.ps1`
- `scripts/install.ps1`

to include `spdlog.dll` in runtime checks and missing-DLL diagnostics.

---

## 4. Subsystem Layout: `eta/log/`

```
eta/log/
  CMakeLists.txt
  src/eta/log/
    log_primitives.h        ; register_log_primitives(...)
    log_types.h             ; LogSink / LogLogger heap object structs
    log_port_sink.h         ; sync port sink implementation
    log_payload.h           ; payload encoders (human + json)
    log_state.h             ; VM-scoped default logger + async pool state
```

`eta/log/CMakeLists.txt` follows the same `INTERFACE` pattern as `eta/nng`,
`eta/stats`, `eta/torch`, and links `eta_core` + `spdlog::spdlog`.

---

## 5. Runtime Integration Details

## 5.1 New runtime object kinds

Add `ObjectKind` entries:

- `LogSink`
- `LogLogger`

with corresponding type headers and factory helpers.

Also update:

- `eta/core/src/eta/runtime/memory/heap.h` (`ObjectKind` enum + to_string)
- `eta/core/src/eta/runtime/types/types.h` includes
- `eta/core/src/eta/runtime/memory/heap_visit.h` (leaf visitor entries)
- `eta/core/src/eta/runtime/value_formatter.cpp` (`#<log-sink>`, `#<logger:name>`)

No GC edges are needed (both hold native/shared_ptr state only).

## 5.2 Primitive registration (fit with current patch model)

Do **not** register logging primitives directly in `Driver`.

Instead:

1. Add `%log-*` names/arities to `builtin_names.h` in canonical order.
2. Add `register_log_primitives(...)` call to `register_all_primitives(...)` in
   `eta/interpreter/all_primitives.h` at the matching slot.
3. Update comments/docs in `all_primitives.h`, `builtin_names.h`, and `Driver`
   constructor comments to include log phase.

This keeps `BuiltinEnvironment::begin_patching()/verify_all_patched()` coherent.

## 5.3 `%log-*` builtin surface

```
%log-make-stdout-sink         (color? stream-symbol)          -> LogSink
%log-make-file-sink           (path truncate?)                -> LogSink
%log-make-rotating-sink       (path max-size max-files)       -> LogSink
%log-make-daily-sink          (path hour minute max-files)    -> LogSink
%log-make-port-sink           (port)                          -> LogSink   ; sync-only
%log-make-current-error-sink  ()                              -> LogSink   ; sync-only

%log-make-logger              (name sinks-list)               -> LogLogger
%log-make-async-logger        (name sinks-list q overflow)    -> LogLogger ; rejects port sinks
%log-get-logger               (name)                          -> LogLogger | #f
%log-default-logger           ()                              -> LogLogger  ; VM-scoped
%log-set-default!             (logger)                        -> '()

%log-set-level!               (logger level-symbol)           -> '()
%log-level                    (logger)                        -> level-symbol
%log-set-global-level!        (level-symbol)                  -> '()
%log-set-pattern!             (logger pattern-string)         -> '()
%log-set-formatter!           (logger formatter-symbol)       -> '()       ; 'human|'json
%log-flush!                   (logger)                        -> '()
%log-flush-on!                (logger level-symbol)           -> '()
%log-emit                     (logger level msg payload)      -> '()
%log-shutdown!                ()                              -> '()       ; explicit, process-level
```

## 5.4 Formatter state (fix thread-local/map weakness)

Formatter mode is stored on `LogLogger` runtime object (`enum FormatterMode { Human, Json }`),
not in thread-local side maps keyed by raw pointers.

- `%log-set-formatter!` updates logger object mode + underlying pattern.
- `%log-emit` reads logger object mode and serializes accordingly.

## 5.5 Payload encoding

Reuse in-tree JSON conversion utilities (same semantic shape as `std.json`) for JSON lines.

- Human formatter: `msg key=val key=val`
- JSON formatter: single JSON object per line including level/logger/msg + payload fields

---

## 6. Ports and Driver Lifecycle

## 6.1 Port sinks

Port sink writes through Eta `Port::write_string`/`flush`, so it naturally participates in:

- `current-error-port`
- callback ports (`Driver::set_stream_sinks`)
- `with-error-to-port` redirection in `std.io`

`log:make-error-port-sink` resolves against the VM's current error port at emit-time
(sync path), not a one-time snapshot.

## 6.2 VM-scoped default logger

Default logger is tracked per VM (in `eta/log/log_state.h`), not as one global mutable default
shared by all drivers.

- `log:default` / `log:set-default!` operate on current VM entry.
- Initial default logger for each VM uses current error port sink + formatter `human` + level `info`.
- `ETA_LOG_LEVEL` is applied at VM logger init.

## 6.3 Shutdown policy

- **Do not call `spdlog::shutdown()` in `Driver` destructor.**
- `log:shutdown!` remains explicit process-level escape hatch (mostly tests/tools).
- Driver teardown only detaches VM-local logger state from log subsystem.

This avoids cross-thread/child-driver interference.

---

## 7. `std.log` Wrapper (`stdlib/std/log.eta`)

Use wrapper patterns already in `std.json`/`std.csv`:

1. Import `std.core` for helpers (`assoc-ref` and option helpers).
2. Parse trailing options as key/value pairs with key normalization (`:foo` and `foo` both accepted).
3. Export thin wrappers over `%log-*`.

### 7.1 Level function API (idiomatic + consistent)

Each level function supports:

- default logger path: `(log:info msg [payload])`
- explicit logger path: `(log:info logger msg [payload])`

Implementation uses one internal helper to decode argument shape and call `%log-emit`.

### 7.2 Sink constructors

Expose:

- `log:make-stdout-sink`
- `log:make-stderr-sink`
- `log:make-file-sink`
- `log:make-rotating-sink`
- `log:make-daily-sink`
- `log:make-port-sink`
- `log:make-error-port-sink`

Path arguments stay plain strings (same style as `std.fs`/OS primitives); runtime normalizes via `std::filesystem`.

### 7.3 Prelude

Add `std.log` import + re-exports in `stdlib/prelude.eta`.

---

## 8. Tests

## 8.1 C++ runtime tests (`eta/test/src/log_tests.cpp`)

- Sink creation for all sink kinds
- Port sink writes to `CallbackPort` / redirected error port
- Logger creation + level/pattern/formatter controls
- `%log-emit` payload encoding (alist/hash-map/nested)
- Async logger rejects port sinks (v1 contract)
- VM-scoped default logger isolation (parent/child driver)
- Explicit `log:shutdown!` behavior and idempotence

## 8.2 Stdlib integration tests (`stdlib/tests/log.test.eta`)

- `log:info` with default logger and explicit logger forms
- `with-error-to-port` captures `log:...` output via error-port sink
- `log:set-level!` filtering
- `log:set-formatter!` human <-> json switch
- file + rotating sink output checks
- async logger path for non-port sinks

---

## 9. Documentation Updates

- Add `docs/guide/reference/log.md` (API + examples + sync-only port sink note)
- Update:
  - `docs/guide/reference/modules.md`
  - `docs/guide/language_guide.md`
  - `docs/next-steps.md`
  - `docs/release-notes.md`
  - `editors/vscode/snippets/eta.json`

Document explicit caveat:

- Port sinks are sync-only in v1.

---

## 10. Build/Packaging Checklist

- [ ] `cmake/FetchSpdlog.cmake` added and pinned to `v1.17.0`
- [ ] `eta/log/CMakeLists.txt` defines `eta_log` interface target
- [ ] `eta/CMakeLists.txt` fetches spdlog and adds `add_subdirectory(log)`
- [ ] `eta_copy_log_dlls(<target>)` defined and applied to runtime executables/tests
- [ ] Runtime targets link `eta_log` alongside `eta_core/eta_torch/eta_nng/eta_stats`
- [ ] Windows install copies `spdlog.dll`
- [ ] release/install scripts include `spdlog.dll` audits
- [ ] `builtin_names.h` includes `%log-*` entries in canonical order
- [ ] `all_primitives.h` registers log primitives in matching order
- [ ] `std.log` added to `stdlib/prelude.eta`

---

## 11. Risks and Mitigations

| Risk | Mitigation |
|---|---|
| Builtin slot mismatch crashes patch mode | Treat `builtin_names.h` + `all_primitives.h` updates as one atomic change; run builtin sync tests |
| Port sink used with async logger | Reject at `%log-make-async-logger` with clear runtime error |
| Cross-driver logger teardown races | No destructor-triggered global shutdown; VM-scoped logger state detach only |
| Formatter-mode races | Store mode in `LogLogger` object, not thread-local pointer maps |
| Platform path issues | Use `std::filesystem` normalization in runtime sink constructors |
| Runtime DLL missing on Windows | Add `spdlog.dll` to build/install audits and copy helpers |

---

## 12. Effort Estimate

| Slice | Effort |
|---|---|
| Fetch/CMake/plumbing + Windows DLL handling | 0.5 day |
| `eta/log` subsystem + object kinds + formatting/payload | 2.0 days |
| Primitive registration + builtin ordering integration | 1.0 day |
| `std.log` wrapper + prelude integration | 0.5 day |
| Port sink + VM-scoped default logger lifecycle work | 1.0 day |
| C++ + stdlib tests | 1.5 days |
| Docs/snippets/release notes | 0.5 day |
| **Total** | **~7.0 days** |

