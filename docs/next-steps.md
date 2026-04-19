# Project Status and Next Steps

[Back to README](../README.md) | [Architecture](architecture.md) |
[NaN-Boxing](nanboxing.md) | [Bytecode and VM](bytecode-vm.md) |
[Compiler](compiler.md) | [Runtime and GC](runtime.md) |
[Modules and Stdlib](modules.md) | [Logic Programming](logic.md) | [CLP](clp.md)

---

## Overview

This page tracks two things:

1. What is already delivered in the current Eta baseline.
2. What remains as next-step work.

---

## Delivered Baseline

### 1) Compiler and VM

- Stable end-to-end pipeline (lexer, parser, expander, module linker,
  semantic analyzer, emitter, VM).
- `etac` AOT bytecode output (`.etac`) and direct `etai` execution.
- Optimization pipeline integrated (including constant folding and dead-code elimination).
- Bytecode serializer/disassembler and diagnostics aligned to the same opcode set.

### 2) Runtime and GC

- Mark-sweep heap with explicit root enumeration.
- VM stacks, globals, frames, and trail-backed logic/CLP state are rooted.
- Unified backtracking recovery via `trail-mark` and `unwind-trail`.

### 3) Logic and constraints

- VM-native unification and logic primitives (`logic-var`, `unify`, `copy-term`,
  `ground?`, and related trail primitives).
- Attributed variables and runtime-configurable occurs-check policy
  (`set-occurs-check!`, `occurs-check-mode`, `put-attr`, `get-attr`, `del-attr`, `attr-var?`).
- CLP(FD) propagation, including native all-different support.
- CLP(B) in `std.clpb`.
- CLP(R) in `std.clpr`, including optimization workflows.
- Relation database support in `std.db` (`defrel`, `assert`, `retract`, `tabled`).

See: [Logic Programming](logic.md) and [CLP](clp.md).

### 4) Networking and actors

- nng-backed messaging over IPC/TCP/inproc.
- Process actors (`spawn`) and in-process thread actors (`spawn-thread`,
  `spawn-thread-with`).
- Lifecycle controls (`spawn-wait`, `spawn-kill`, `thread-join`, `thread-alive?`).
- Monitoring primitives (`monitor`, `demonitor`) and stdlib supervision helpers in
  `std.supervisor` (one-for-one and one-for-all strategies).

See: [Networking Primitives](networking.md), [Message Passing and Actors](message-passing.md),
and [Network and Message-Passing Parallelism](network-message-passing.md).

### 5) Tooling and tests

- LSP server with parse/analysis-driven diagnostics.
- DAP server with breakpoints, stepping, stack/local inspection, evaluation,
  hover-eval support, and completions.
- VS Code extension with syntax, snippets, debugger views, and test integration.
- C++ unit suite (`eta_core_test`) and stdlib test runner (`eta_test`).
- `eta_test` is wired into CTest (`eta_stdlib_tests`) and the convenience target
  `eta_rebuild_and_test`.

See: [Build](build.md), [Architecture](architecture.md), and [Examples](examples.md).

---

## Remaining Next Steps

### 1) Performance work

- Add a repeatable benchmark harness and track it in CI.
- Expand optimizer coverage beyond current passes (for example, inlining and
  stronger interprocedural propagation).
- Evaluate VM dispatch improvements (for example, super-instructions and inline caches).

### 2) Runtime and GC evolution

- Investigate generational/incremental GC strategies to reduce pause times.
- Improve adaptive GC triggering and long-running fragmentation behavior.

### 3) Logic/CLP internals

- Native VM-level tabling engine (current tabled workflow is stdlib-level in `std.db`).
- Optional WAM-style logic bytecode path for specialized logic workloads.

### 4) Distributed actor runtime

- Built-in distributed supervision and name-registry semantics at runtime level
  (beyond current library-level helpers).

### 5) Notebook workflow

- Optional Jupyter kernel integration for notebook-first exploratory workflows.
