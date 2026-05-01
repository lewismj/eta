# std.process — Subprocess Execution Plan

[Back to next-steps](../next-steps.md) · Phase H1 (final slice)

---

## 1. Scope & Goals

`std.process` closes the last gap in Hosted-Platform Phase **H1**: the
ability to spawn and control external OS processes from Eta. With
`std.os`, `std.fs`, `std.json`, and `std.log` already shipped, adding
subprocess support brings Eta to feature-parity with Python / Clojure
for everyday automation: deploy scripts, test runners, calling `git`,
`python`, `ffmpeg`, etc.

### Goals

1. A blocking `(process:run …)` that returns `(status stdout stderr)`
   — covers the 80% case (shell-out helpers).
2. A non-blocking `(process:spawn …)` returning a process handle for
   long-running children, with `process:wait`, `process:kill`,
   `process:pid`, `process:alive?`.
3. Stdio piping that surfaces the child's `stdin`/`stdout`/`stderr` as
   first-class Eta `Port` values, so existing `read-line`,
   `read-string`, `read-u8`, `display`, `write-u8`, `close-port` work
   unchanged.
4. Cross-platform: POSIX (Linux/macOS) and Windows. UTF-8 paths and
   args throughout (matching `std.fs` conventions).
5. GC-safe: process handles are heap objects with finalizers that
   reap zombies and close pipe FDs.
6. No new third-party dependency if avoidable. Use `std::filesystem`
   patterns already proven in `os_primitives.h`; fall back to direct
   POSIX (`fork`/`execvp`/`pipe`/`posix_spawn`) and Win32
   (`CreateProcessW`, `CreatePipe`) calls.

### Non-goals (v1)

- PTY allocation (no `pty.openpty` equivalent). Defer; most automation
  doesn't need it.
- Shell expansion. Args are always a list; no `system("cmd …")` form.
  This matches Clojure `sh` and avoids quoting bugs.
- Asynchronous I/O multiplexing on child pipes. v1 uses background
  reader threads to drain stdout/stderr.
- Process groups / job control / signals beyond `kill`/`terminate`.
- Inheriting arbitrary FDs into the child.

---

## 2. API Surface

### 2.1 Stdlib module — `stdlib/std/process.eta`

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
    process:run-string)   ;; like process:run but returns stdout as a string
  (begin
    (define process:run             %process-run)
    (define process:spawn           %process-spawn)
    (define process:wait            %process-wait)
    (define process:kill            %process-kill)
    (define process:terminate       %process-terminate)
    (define process:pid             %process-pid)
    (define process:alive?          %process-alive?)
    (define process:exit-code       %process-exit-code)
    (define process:handle?         %process-handle?)
    (define process:stdin-port      %process-stdin-port)
    (define process:stdout-port     %process-stdout-port)
    (define process:stderr-port     %process-stderr-port)

    (defun process:run-string (cmd . rest)
      (let* ((result (apply %process-run cmd rest))
             (out    (cadr result)))
        out))))
```

### 2.2 Calling conventions

#### `process:run program args [opts]` → `(status stdout stderr)`

- `program` — string, looked up via `PATH` if not absolute.
- `args` — list of strings, **no shell interpretation**.
- `opts` — optional alist (hash-map also accepted) with keys:
  - `cwd` — string (default: inherited)
  - `env` — alist of `(name . value)` pairs (default: inherited);
    pass `'replace? #t` to replace rather than extend.
  - `stdin` — string / bytevector to write, or `'inherit` /
    `'null` (default: `'null`).
  - `stdout` / `stderr` — `'capture` (default), `'inherit`, `'null`,
    or `'merge` (stderr → stdout, valid only for `stderr`).
  - `timeout-ms` — integer; if exceeded, child is killed and a
    `('process-timeout pid)` condition is raised.
  - `binary?` — `#t` to return captured streams as bytevectors instead
    of strings.
- Returns a list: `(exit-code stdout stderr)` where the captured
  streams are strings (UTF-8) by default or bytevectors when
  `'binary? #t`. Streams set to `'inherit`/`'null` return `#f`.

#### `process:spawn program args [opts]` → process handle

- Same `opts` as `run` except:
  - `stdin` may be `'pipe` (default for spawn) — gives a writable
    port via `process:stdin-port`.
  - `stdout` / `stderr` may be `'pipe` (default) — readable ports.
- Returns a `ProcessHandle` opaque object.

#### Lifecycle

- `process:wait handle [timeout-ms]` — blocks until exit, returns
  exit code (or `#f` on timeout).
- `process:kill handle` — `SIGKILL` / `TerminateProcess`.
- `process:terminate handle` — `SIGTERM` (POSIX) / graceful close
  message + `TerminateProcess` fallback on Windows.
- `process:pid handle` → integer.
- `process:alive? handle` → `#t`/`#f`.
- `process:exit-code handle` → integer or `#f` if still running.

#### Errors

All builtins return `RuntimeError` via `std::expected`, matching the
pattern used in `os_primitives.h`. Error tags:

- `process-spawn-failed` — `execvp` / `CreateProcess` returned an OS
  error.
- `process-not-found` — distinct case for `ENOENT`.
- `process-timeout`.
- `process-already-waited`.
- Generic `TypeError` for argument validation.

---

## 3. Implementation Plan

### 3.1 New runtime files

```
eta/core/src/eta/runtime/
  process_primitives.h        ;; registration entry point + inline impls
  process_primitives.cpp      ;; (split if implementation grows; thread state lives here)
  types/
    process_handle.h          ;; ProcessHandleObject heap kind
```

### 3.2 `ProcessHandleObject` — heap object kind

Mirror the existing `PortObject` (see
`eta/core/src/eta/runtime/types/port.h`):

```cpp
namespace eta::runtime::types {

struct ProcessHandle {
    // OS handle
#ifdef _WIN32
    HANDLE process_handle = nullptr;   ///< CreateProcess hProcess
    DWORD  pid            = 0;
#else
    pid_t  pid            = -1;
#endif

    // Pipe ports (nullable; only present if 'pipe in opts).
    std::shared_ptr<Port> stdin_port;
    std::shared_ptr<Port> stdout_port;
    std::shared_ptr<Port> stderr_port;

    // State (atomic for the finalizer / monitor thread).
    std::atomic<bool> reaped{false};
    std::atomic<int>  exit_code{-1};

    // Background drain threads (only used by run() with capture).
    std::thread stdout_reader;
    std::thread stderr_reader;
    std::string captured_stdout;
    std::string captured_stderr;
};

struct ProcessHandleObject {
    std::shared_ptr<ProcessHandle> handle;
};

}  ///< namespace eta::runtime::types
```

The `shared_ptr` lets the finalizer close OS handles without stalling
the GC cycle, and lets the captured-output buffers outlive the handle
slot if needed.

### 3.3 New heap-object kind

Add `ObjectKind::ProcessHandle` to `runtime/types/types.h`. Wire:

- `make_process_handle(heap, …)` factory in `runtime/factory.h`.
- Tracing: handle has no traceable `LispVal` children (the embedded
  `Port` objects are held by `shared_ptr<Port>`, not as `LispVal`),
  so no GC mark interaction is needed beyond root enumeration. This
  matches how `PortObject` works today.
- Finalizer: on collection, if `!reaped`, send `SIGKILL` /
  `TerminateProcess`, then `waitpid(WNOHANG)` /
  `WaitForSingleObject(0)`, close pipes. Re-uses the finalizer
  infrastructure documented in [finalizers.md](../finalizers.md).

### 3.4 Spawn implementation

#### POSIX path

1. Build `argv[]` (null-terminated, UTF-8).
2. Build `envp[]` if `env` was supplied (or pass `nullptr` to inherit).
3. `pipe2()` (or `pipe` + `fcntl`) for each requested stdio stream
   (CLOEXEC on parent end).
4. `posix_spawnp()` preferred over `fork`/`exec` to avoid inheriting
   GC heap state. Fall back to `fork`+`execvp` if `posix_spawn` lacks
   needed features (`addchdir_np` is glibc/macOS≥10.15 only); detect
   via `__has_include` / feature macros.
5. Parent closes the child ends, wraps the parent ends in `Port`
   objects via the existing FD-port factory used by
   `open-input-file` / `open-output-file`.

#### Windows path

1. Build the wide-char command line via the documented MSDN
   `CommandLineToArgvW` round-trip algorithm. **All quoting is owned
   by `std.process`** — never delegated to the user.
2. `CreatePipe` for each piped stdio stream; `SetHandleInformation`
   to make the parent end non-inheritable.
3. `CreateProcessW` with `STARTUPINFOEX` setting `hStdInput` /
   `hStdOutput` / `hStdError` to the inheritable child ends.
4. Wrap parent handles into Eta `Port` objects via a new
   `make_port_from_handle(HANDLE, mode)` helper alongside the
   existing FD helper.

### 3.5 `process:run` implementation

`process:run` is `process:spawn` + drain + `process:wait`.

- For captured streams, spin two `std::jthread`s per child that read
  into a `std::string`. Required because POSIX `read` and Windows
  `ReadFile` will both deadlock if the child writes >64 KB to a pipe
  and we wait first.
- For `'inherit`, the child uses the parent's `stdin/stdout/stderr`
  HANDLEs/FDs directly — no port wrapping needed.
- For `'null`, redirect to `/dev/null` (POSIX) or `NUL` (Windows).
- If `stdin` is a string / bytevector, a third writer thread pumps it
  into the child and closes the write end.
- On timeout, kill the child, join the threads, raise
  `process-timeout`.

### 3.6 Builtin registration

Add a new section to `eta/core/src/eta/runtime/builtin_names.h`
(canonical order — *append after the `os_primitives.h` block, before
the `time_primitives.h` block*):

```cpp
/**
 * process_primitives.h  (must match registration order exactly)
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

Update `eta/interpreter/src/eta/interpreter/all_primitives.h`:

```cpp
#include "eta/runtime/process_primitives.h"
// ...existing code...
runtime::register_os_primitives(env, heap, intern, vm, command_line_arguments);
runtime::register_process_primitives(env, heap, intern, vm);
runtime::register_time_primitives(env, heap, intern, &vm);
// ...existing code...
```

Insert after `register_os_primitives` and before
`register_time_primitives` so the canonical order in the header
comment matches `builtin_names.h`.

Note: builtins are exposed with a `%` prefix (matching `%log-*`,
`%time-*`); the public, non-prefixed names live in `std.process`.
This keeps the user-visible surface stable even if we refactor the
runtime internals.

### 3.7 Stdlib

- New file `stdlib/std/process.eta` (see §2.1).
- Auto-import in `stdlib/prelude.eta`? **No.** Keep opt-in
  (`(import std.process)`). Subprocess capability is unusual in pure
  computation; matching Python's `import subprocess` discoverability
  is fine.

---

## 4. Testing

### 4.1 C++ unit tests

`eta/test/src/process_primitives_tests.cpp` (modeled on
`os_primitives_tests.cpp`):

1. `process:run` of a no-op (`true` on POSIX, `cmd /c exit 0` on
   Windows) returns exit 0.
2. Capture stdout: run an echo, assert string equality.
3. Capture stderr: run a Python one-liner that prints to stderr.
4. Non-zero exit code surfaces correctly.
5. `process:spawn` + write to stdin port + read from stdout port
   (round-trip via `cat` / `findstr`).
6. `process:kill` on a sleeping child — verify `process:alive?`
   transitions and `process:exit-code` returns a non-zero / signal
   code.
7. Timeout path raises `process-timeout`.
8. Invalid program raises `process-not-found`.
9. cwd opt: spawn `pwd` / `cd` and assert output.
10. env opt: spawn process that prints a custom env var.
11. GC test: drop a handle without waiting, force GC, verify
    finalizer reaped the child and closed FDs (no FD leak —
    measure via `/proc/self/fd` count on Linux).

For portability, gate Windows-only / POSIX-only tests with
`#ifdef _WIN32` and pick a canonical "echo hello" replacement on
each platform.

### 4.2 Stdlib tests — `stdlib/tests/process.test.eta`

Mirror the existing `os_fs.test.eta` pattern. Use only commands that
exist on every supported OS:

```scheme
(module process.tests
  (import std.test std.process std.os)
  (begin

    (define (echo-cmd)
      (if (eq? (os:getenv "OS") "Windows_NT")
          (list "cmd" '("/c" "echo" "hello"))
          (list "/bin/echo" '("hello"))))

    (define suite
      (make-group "std.process"
        (list
          (make-test "process:run echoes stdout"
            (lambda ()
              (let* ((c (echo-cmd))
                     (r (process:run (car c) (cadr c))))
                (assert-equal 0 (car r))
                (assert-true (string-contains? (cadr r) "hello"))))))))))
```

Add the file to the stdlib test runner's manifest so it runs under
`eta_stdlib_tests`.

### 4.3 Examples

- `examples/process-shellout.eta` — call `git --version`, parse the
  output. Demonstrates `process:run`.
- `examples/process-pipeline.eta` — spawn two processes and pipe
  stdout of the first into stdin of the second, in pure Eta.

---

## 5. Documentation

- New page `docs/guide/reference/process.md` — full API reference,
  examples, error-handling table, platform notes (quoting on
  Windows, signal numbers on POSIX). Cross-link from
  [`os.md`](../guide/reference/os.md) and
  [`fs.md`](../guide/reference/fs.md).
- Update `docs/next-steps.md`:
  - Move "Subprocess (`std.process`) is the remaining H1 slice." into
    the "Recently Completed" section once shipped.
  - Mark capability-matrix row "Subprocess / `exec`" as **Closed**.
- Update `docs/release-notes.md` with a dated entry.
- Add snippets for `process:run` and `process:spawn` to
  `editors/vscode/.../snippets/eta.json`.

---

## 6. Risks & Mitigations

| Risk | Mitigation |
|---|---|
| Deadlock when child writes >pipe-buffer to stdout while we wait. | Drain in background threads (always, when capturing). |
| Windows command-line quoting bugs (the classic `argv` round-trip hazard). | Use the documented MSDN algorithm for `CommandLineToArgvW`, with a unit test that round-trips through it. Never expose a `'shell #t` opt. |
| FD leaks on early throw before `posix_spawn_file_actions_destroy`. | RAII wrappers around `posix_spawn_file_actions_t`, `posix_spawnattr_t`, and parent pipe FDs. |
| GC stalls if child writes a lot before reaping. | Background readers don't touch the heap; they hand the finished `std::string` over only after `wait` completes. |
| Finalizer running on a still-running child blocks GC. | Finalizer issues SIGKILL/TerminateProcess first, then a *non-blocking* `waitpid(WNOHANG)` and re-queues if not yet exited; runs on the finalizer thread, not the mutator. |
| Signal-handling on POSIX (`SIGCHLD`). | Don't install handlers; rely on `waitpid`. The drain threads naturally observe EOF when the child exits. |
| `posix_spawn_file_actions_addchdir_np` not available on macOS < 10.15 / older glibc. | Fall back to `fork` + `chdir` + `execvp` path. Detect via `__has_include` / feature macros. |
| Captured output may not be valid UTF-8. | Default to string with replacement chars; opt-in `'binary? #t` for raw bytevectors. |

---

## 7. Effort Estimate

| Item | Effort |
|---|---|
| C++ runtime (`process_primitives.{h,cpp}` + `types/process_handle.h`) | ~600 LoC |
| Builtin-name registration + factory + GC wiring | ~50 LoC |
| Stdlib wrapper (`std/process.eta`) | ~80 LoC |
| C++ unit tests | ~400 LoC |
| Stdlib tests | ~150 LoC |
| Docs (`process.md`, release notes, snippets) | ~300 lines |
| **Total** | **~2 weeks engineering** (matches H1 estimate in `next-steps.md`) |

---

## 8. Delivery Order

1. **PR 1** — `ProcessHandle` type, factory, GC wiring, finalizer.
   No builtins yet; verified by C++ unit test that constructs a
   handle and forces a GC cycle.
2. **PR 2** — `%process-spawn`, `%process-pid`, `%process-alive?`,
   `%process-wait`, `%process-kill`, plus `%process-handle?`. POSIX
   only. Tests against `/bin/true`, `/bin/sleep`.
3. **PR 3** — Windows path for the same builtins. Add Windows CI
   coverage.
4. **PR 4** — Pipe / stdio handling: `%process-stdin-port`,
   `%process-stdout-port`, `%process-stderr-port`. Reuse existing
   FD/HANDLE→Port helpers.
5. **PR 5** — `%process-run` with capture, drain threads, timeout.
6. **PR 6** — `std.process` module, examples, docs page, snippets,
   `next-steps.md` flip to "Closed", release-notes entry.

Each PR is independently reviewable and ships behind no flag — same
cadence used for `std.json` and `std.log`.

---

## 9. Open Questions

1. **Bytevector vs string for captured output.** Default to
   string-with-replacement-char on invalid UTF-8, or always
   bytevector? *Proposal:* string by default, opt-in bytevector via
   `'binary? #t`. Matches Python `subprocess.run(text=True)`.
2. **Signal name surface on POSIX.** Should `process:terminate` take
   an optional signal symbol (`'sigint`, `'sigterm`, `'sigkill`)?
   *Proposal:* defer; v1 has only `terminate` (SIGTERM) and `kill`
   (SIGKILL). Add `process:signal!` later if needed.
3. **`process:run` shorthand.** Worth adding
   `(process:sh "git rev-parse HEAD")` that splits on whitespace?
   *Proposal:* **no** — encourages quoting bugs. Users who want it
   can write a 3-line wrapper.
4. **Child of child cleanup.** Should the finalizer kill the entire
   process group? *Proposal:* v1 kills only the direct child.
   Document that long-running grandchildren survive. Revisit if a
   real user needs `setpgid` semantics.

