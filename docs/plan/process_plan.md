# std.process - Subprocess Execution Plan

[Back to next-steps](../next-steps.md) - Phase H1 (final slice)

---

## 1. Scope and Goals

`std.process` closes the remaining Hosted-Platform Phase H1 gap: running and
controlling external OS processes from Eta.

With `std.os`, `std.fs`, `std.json`, and `std.log` already shipped, subprocess
support unlocks common automation and notebook workflows: shelling out to
`git`, `curl`, `python`, `ffmpeg`, etc.

### Goals

1. A blocking `(process:run ...)` that returns `(status stdout stderr)` for the
   common shell-out case.
2. A non-blocking `(process:spawn ...)` returning a process handle, plus
   lifecycle operations: `process:wait`, `process:kill`, `process:terminate`,
   `process:pid`, `process:alive?`, `process:exit-code`.
3. Child stdio as first-class Eta ports (`stdin`/`stdout`/`stderr`) so existing
   port APIs (`display`, `read-char`, `read-u8`, `write-u8`, `close-port`) work
   unchanged.
4. Cross-platform support (Linux/macOS/Windows) with UTF-8 arguments and paths.
5. GC-safe integration:
   process handles are heap objects; cleanup is done by RAII destructors and
   explicit wait/kill paths, without introducing blocking GC behavior.
6. No new third-party dependency unless needed.
   Prefer direct POSIX/Win32 process APIs, consistent with existing
   `os_primitives.h` style.

### Non-goals (v1)

- PTY allocation.
- Shell-expression mode (`system("...")`-style expansion and quoting).
- Full async I/O multiplexing API.
- Process-group/job-control surface beyond terminate/kill.
- Arbitrary inherited FD/HANDLE passing.

---

## 2. API Surface

### 2.1 Stdlib module - `stdlib/std/process.eta`

```scheme
(module std.process
  (export
    ;; Blocking
    process:run

    ;; Non-blocking
    process:spawn
    process:wait
    process:kill
    process:terminate
    process:pid
    process:alive?
    process:exit-code
    process:handle?

    ;; Stdio access
    process:stdin-port
    process:stdout-port
    process:stderr-port

    ;; Convenience
    process:run-string)
  (begin
    (define process:run            %process-run)
    (define process:spawn          %process-spawn)
    (define process:wait           %process-wait)
    (define process:kill           %process-kill)
    (define process:terminate      %process-terminate)
    (define process:pid            %process-pid)
    (define process:alive?         %process-alive?)
    (define process:exit-code      %process-exit-code)
    (define process:handle?        %process-handle?)
    (define process:stdin-port     %process-stdin-port)
    (define process:stdout-port    %process-stdout-port)
    (define process:stderr-port    %process-stderr-port)

    (defun process:run-string (program args . rest)
      (let* ((result (apply %process-run program args rest)))
        (car (cdr result))))))
```

### 2.2 Calling conventions

#### `process:run program args [opts]` -> `(status stdout stderr)`

- `program`: string, resolved through `PATH` if relative.
- `args`: list of strings, no shell interpretation.
- `opts`: optional alist (hash-map accepted) with keys:
  - `cwd`: string (default inherited).
  - `env`: alist of `(name . value)` pairs (default inherited).
  - `replace-env?`: boolean (default `#f`) to replace instead of extend env.
  - `stdin`: string | bytevector | `'inherit` | `'null` (default `'null`).
  - `stdout` / `stderr`: `'capture` (default), `'inherit`, `'null`.
    For `stderr` only: `'merge` (redirect to stdout).
  - `timeout-ms`: integer timeout.
  - `binary?`: when `#t`, captured streams are bytevectors instead of strings.
- return value:
  `(exit-code stdout stderr)`, with `stdout`/`stderr` as string or bytevector.
  Streams configured as `'inherit`/`'null` return `#f`.

#### `process:spawn program args [opts]` -> process handle

- Same options as `process:run`, but stream defaults are pipe-oriented:
  - `stdin`: `'pipe` by default.
  - `stdout`/`stderr`: `'pipe` by default.
- Returns an opaque process handle.

#### Lifecycle

- `process:wait handle [timeout-ms]` -> exit-code or `#f` on timeout.
- `process:kill handle` -> hard kill (`SIGKILL` / `TerminateProcess`).
- `process:terminate handle` -> graceful terminate (`SIGTERM` / terminate).
- `process:pid handle` -> integer.
- `process:alive? handle` -> `#t` / `#f`.
- `process:exit-code handle` -> integer or `#f` if still running.

#### Errors

Builtins return `RuntimeError` via `std::expected`, like other runtime modules.

- argument validation: `RuntimeErrorCode::TypeError`
- operational failures: `RuntimeErrorCode::InternalError` with stable message
  prefixes:
  - `process-spawn-failed: ...`
  - `process-not-found: ...`
  - `process-timeout: ...`
  - `process-wait-failed: ...`

This matches current runtime error handling and catchability behavior.

---

## 3. Implementation Plan

### 3.1 New runtime files

```text
eta/core/src/eta/runtime/
  process_primitives.h
  process_primitives.cpp
  types/
    process_handle.h
```

If OS-specific logic grows, split helpers into
`process_platform_posix.h` / `process_platform_win32.h`.

### 3.2 `ProcessHandleObject` heap type

`ProcessHandle` should stay lean and lifecycle-focused:

```cpp
namespace eta::runtime::types {

struct ProcessHandle {
#ifdef _WIN32
    HANDLE process_handle{nullptr};
    DWORD  pid{0};
#else
    pid_t  pid{-1};
#endif

    std::atomic<bool> running{true};
    std::atomic<bool> waited{false};
    std::atomic<int>  exit_code{-1};

    // Keep stable port identity and keep ports live while handle is live.
    LispVal stdin_port{nanbox::Nil};
    LispVal stdout_port{nanbox::Nil};
    LispVal stderr_port{nanbox::Nil};

    ~ProcessHandle(); // close handles/pipes; no blocking wait in GC path
};

struct ProcessHandleObject {
    std::shared_ptr<ProcessHandle> handle;
};

} // namespace eta::runtime::types
```

Important correction: captured stdout/stderr reader threads and buffers used by
`process:run` should be local to that call path, not fields on the heap object.

### 3.3 Heap and GC wiring

Integrate with existing runtime patterns:

1. Add `ObjectKind::ProcessHandle` in
   `eta/core/src/eta/runtime/memory/heap.h` (not in `types/types.h`).
2. Add `types/process_handle.h` include in `types/types.h`.
3. Add `make_process_handle(...)` in `runtime/factory.h`.
4. Update heap visitor plumbing:
   - `memory/heap_visit.h`: add `visit_process_handle`.
   - `memory/mark_sweep_gc.h`: mark `stdin_port` / `stdout_port` / `stderr_port`
     when present.
5. Update value formatting and debug display paths as needed
   (`value_formatter.*`, DAP heap views).

Cleanup model correction:

- do not rely on user-level heap finalizer callbacks for process cleanup.
- use RAII in `ProcessHandle` destructor for best-effort resource release.
- keep destructor non-blocking from GC perspective.

### 3.4 Spawn implementation

#### POSIX path

1. Build `argv[]` and optional `envp[]`.
2. Build requested pipes with `pipe2(O_CLOEXEC)` (or `pipe` + `fcntl`).
3. Prefer `posix_spawnp`; fall back to `fork`/`execvp` where required.
4. Parent closes child ends and wraps parent pipe ends into new
   native-handle-backed Eta ports.

Important correction:
there is no existing generic FD/HANDLE-to-port factory in current `port.h`.
`std.process` needs a new port implementation for native pipe handles.

#### Windows path

1. Build a properly quoted UTF-16 command line from `program` + `args`.
2. Create inheritable pipes with `CreatePipe`.
3. Configure `STARTUPINFO` std handles and call `CreateProcessW`.
4. Parent closes child ends and wraps parent ends into native-handle ports.

### 3.5 `process:run` implementation

`process:run` should be implemented as:

- internal spawn helper configured for capture/inherit/null
- concurrent drain readers for captured stdout/stderr
- optional stdin writer thread for string/bytevector input
- wait/timeout/kill orchestration
- return `(status stdout stderr)` in text or binary mode

Timeout behavior should return an `InternalError` with
`process-timeout:` prefix (consistent with current runtime errors).

### 3.6 Builtin registration

Add the process block in `builtin_names.h` after os and before time:

```cpp
/**
 * process_primitives.h (must match registration order exactly)
 */
r("%process-run",          2, true);
r("%process-spawn",        2, true);
r("%process-wait",         1, true);
r("%process-kill",         1, false);
r("%process-terminate",    1, false);
r("%process-pid",          1, false);
r("%process-alive?",       1, false);
r("%process-exit-code",    1, false);
r("%process-handle?",      1, false);
r("%process-stdin-port",   1, false);
r("%process-stdout-port",  1, false);
r("%process-stderr-port",  1, false);
```

Update `all_primitives.h`:

```cpp
#include "eta/runtime/process_primitives.h"

runtime::register_os_primitives(env, heap, intern, vm, command_line_arguments);
runtime::register_process_primitives(env, heap, intern, vm);
runtime::register_time_primitives(env, heap, intern, &vm);
```

Also update the canonical-order comments in both `all_primitives.h` and
`builtin_names.h` to include `process_primitives.h`.

### 3.7 Stdlib

- Add `stdlib/std/process.eta`.
- Keep `std.process` opt-in (do not auto-import via `prelude.eta`).

---

## 4. Testing

### 4.1 C++ unit tests

Add `eta/test/src/process_primitives_tests.cpp` (model `os_primitives_tests.cpp`)
and include it in `eta/test/CMakeLists.txt`.

Suggested coverage:

1. `process:run` no-op returns 0.
2. stdout capture works.
3. stderr capture works.
4. non-zero exit code propagation.
5. spawn + stdin/stdout port round-trip.
6. kill/terminate transitions `alive?` correctly.
7. timeout path surfaces `process-timeout` error prefix.
8. invalid program path surfaces `process-not-found`.
9. `cwd` option respected.
10. `env` option respected.
11. handle object formatting/predicate.
12. GC path does not leak process handles or pipe handles.

Keep tests dependency-light:

- avoid requiring Python for stderr checks.
- use platform-native shell commands only where needed.

### 4.2 Stdlib tests - `stdlib/tests/process.test.eta`

Use the same discovery model as existing stdlib tests (`*.test.eta`).
No separate manifest entry is required.

Example skeleton:

```scheme
(module process.tests
  (import std.test std.process std.os)
  (begin
    (defun test-contains? (text needle)
      (let ((n (string-length text))
            (m (string-length needle)))
        (if (= m 0)
            #t
            (if (< n m)
                #f
                (letrec ((loop (lambda (i)
                                 (if (> i (- n m))
                                     #f
                                     (if (string=? (substring text i (+ i m)) needle)
                                         #t
                                         (loop (+ i 1)))))))
                  (loop 0))))))

    (defun echo-cmd ()
      (if (equal? (os:getenv "OS") "Windows_NT")
          (list "cmd" '("/c" "echo" "hello"))
          (list "/bin/echo" '("hello"))))

    ;; ... tests ...
  ))
```

### 4.3 Examples

- `examples/process-shellout.eta` - run `git --version`.
- `examples/process-pipeline.eta` - pipe one spawned process to another.

---

## 5. Documentation Updates

1. Add `docs/guide/reference/process.md`.
2. Add module entry in `docs/guide/reference/modules.md`.
3. Cross-link from `os.md` and `fs.md`.
4. Update `docs/next-steps.md` and `docs/release-notes.md` after ship.
5. Add VS Code snippets for `process:run` and `process:spawn`.

---

## 6. Risks and Mitigations

| Risk | Mitigation |
|---|---|
| Deadlock on full stdout/stderr pipes | Always drain captured streams concurrently while waiting. |
| Windows quoting mistakes | Centralize quoting builder, unit test tricky arg sets, avoid shell mode. |
| Resource leaks on error paths | RAII wrappers for pipes/process handles and spawn attributes. |
| GC pause inflation | Keep process-handle destructor non-blocking; avoid blocking wait in GC path. |
| Zombie process risk if callers never wait | Document explicit `process:wait` requirement; best-effort terminate/close in destructor. |
| Missing FD/HANDLE port bridge | Add explicit native-handle port implementation for subprocess pipes. |
| Invalid UTF-8 output | `binary? #t` option for raw bytes; text mode uses replacement decoding. |

---

## 7. Effort Estimate

| Item | Effort |
|---|---|
| Runtime (`process_primitives.*`, `types/process_handle.h`) | ~600 LoC |
| Heap/factory/visitor integration | ~100 LoC |
| Native-handle port implementation | ~200 LoC |
| C++ tests | ~400 LoC |
| Stdlib wrapper + stdlib tests | ~250 LoC |
| Docs/snippets/release notes | ~300 lines |
| **Total** | **~2 engineering weeks** |

---

## 8. Delivery Order

1. **PR 1** - `ProcessHandle` type, `ObjectKind`, factory, GC visitor wiring.
2. **PR 2** - `%process-spawn`, `%process-wait`, `%process-kill`,
   `%process-terminate`, `%process-pid`, `%process-alive?`, `%process-exit-code`,
   `%process-handle?` (POSIX first).
3. **PR 3** - Windows implementation for PR 2 + CI coverage.
4. **PR 4** - native-handle pipe ports and
   `%process-stdin-port` / `%process-stdout-port` / `%process-stderr-port`.
5. **PR 5** - `%process-run` capture/timeout/binary mode.
6. **PR 6** - `std.process`, stdlib tests, examples, docs, snippets, release note.

---

## 9. Open Questions

1. **Default capture type**: string vs bytevector.
   Proposal: string default, `binary? #t` for bytevector.
2. **Signal API depth**: keep terminate/kill only, or expose `process:signal!`.
   Proposal: terminate/kill only in v1.
3. **Process-group semantics**: whether kill should target child only or group.
   Proposal: child-only in v1.
4. **Prelude import**: keep explicit import?
   Proposal: yes, keep `std.process` opt-in.
