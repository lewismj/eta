# Bytecode & Tools

[← Back to Language Guide](./language_guide.md)

This is the task-oriented companion to the reference docs
[`compiler.md`](./reference/compiler.md) and [`bytecode-vm.md`](./reference/bytecode-vm.md).
It covers everyday workflows: compiling, running, inspecting bytecode,
and tracing a function from source to `Call` opcode.

---

## Compile vs interpret

```bash
etai program.eta              # parse, compile, run in one step
etac program.eta -o out.etac  # compile only
etai out.etac                 # run pre-compiled bytecode
```

`.etac` files are a self-contained serialised form of a module: bytecode,
constants, debug spans, and the symbol table.

| Workflow              | When to use                                            |
| :-------------------- | :----------------------------------------------------- |
| `etai source.eta`     | Iteration, scripts, REPL development                   |
| `etac -O … && etai …` | Production / benchmark runs                            |
| Ship `.etac`          | Distribute binaries without source                     |

---

## Optimisation

`etac -O` runs the standard pass set:

- Constant folding
- Dead-code elimination
- Peephole opcode fusion (`Call`/`Return` → `TailCall`, etc.)
- Known-call inlining for small leaf functions
- Macro pre-expansion

See [`optimisations.md`](./reference/optimisations.md) and
[`optimization.md`](./reference/optimization.md) for the full pass list and
heuristics.

---

## Inspecting bytecode

```bash
etac --disasm program.eta
```

Emits a per-function listing of opcodes with PCs, source spans, and
constants. Use this to verify TCO, look for missing inlining, or
correlate a profiler hot spot with its source.

> [!TIP]
> A useful pre-flight check for tight loops: search the disassembly for
> `Call` in your loop body — every recursive iteration should be
> `TailCall`. See [Tail Calls](./tail-calls.md).

---

## Inspecting macro expansions

```bash
etac --dump-expand program.eta
```

Prints the source after macro expansion, before bytecode emission —
useful for hygiene problems and ellipsis debugging. See
[Macros](./macros.md).

---

## Reading a `.etac` file

| Section            | Contents                                            |
| :----------------- | :-------------------------------------------------- |
| Header             | Magic, version, target triple                       |
| Symbol table       | Module exports and import resolutions               |
| Constant pool      | All literal values                                  |
| Functions          | Per-function bytecode + upvalue layout              |
| Debug spans        | `(file-id, start-line, start-col, end-line, end-col)` per opcode |

`etai` validates the header on load and refuses mismatched versions.

---

## Common opcodes

| Opcode         | Purpose                                                  |
| :------------- | :------------------------------------------------------- |
| `LoadConst`    | Push a constant from the pool                            |
| `Load` / `Store` | Local slot read / write                                 |
| `LoadUp` / `StoreUp` | Upvalue read / write                              |
| `Call` / `TailCall` | Function call (the latter reuses the frame)         |
| `Return`       | Pop frame and return                                     |
| `Jump`, `JumpIfFalse` | Unconditional / conditional branches              |
| `SetupCatch`, `Throw` | Exception handler frame; raise                    |
| `Unify`, `BindVar` | Logic-engine primitives                              |

The complete opcode set is in [`bytecode-vm.md`](./reference/bytecode-vm.md).

---

## Build-time vs run-time errors

| Phase     | Examples                                                    |
| :-------- | :---------------------------------------------------------- |
| Compile   | Parse error, macro expansion error, undefined export, arity mismatch on `define` |
| Link      | Missing module on the search path                           |
| Runtime   | `runtime.type-error`, `runtime.invalid-arity`, user `raise` |

Compile and link errors are reported with full source spans; runtime
errors carry a span and a stack trace as the exception payload — see
[Error Handling](./error-handling.md).

---

## Profiling tips

- `etac -O --emit-stats` writes a per-function size summary; large
  closures often indicate accidental upvalue capture.
- VS Code's Disassembly View highlights the current PC during a DAP
  pause; combined with the Heap Inspector this is usually faster than a
  separate profiler.

---

## Related

- [`compiler.md`](./reference/compiler.md), [`bytecode-vm.md`](./reference/bytecode-vm.md)
- [`optimisations.md`](./reference/optimisations.md), [`optimization.md`](./reference/optimization.md)
- [Tail Calls](./tail-calls.md), [Debugging](./debugging.md)

