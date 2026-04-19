# Project Status

[← Back to README](../README.md) · [Architecture](architecture.md) ·
[NaN-Boxing](nanboxing.md) · [Bytecode & VM](bytecode-vm.md) ·
[Compiler](compiler.md) · [Runtime & GC](runtime.md) · [Modules & Stdlib](modules.md) ·
[Logic Programming](logic.md) · [CLP](clp.md)

---

## Overview

This document describes the current state of Eta across compiler/runtime,
logic and constraints, networking, and tooling.

---

## 1 · Compiler and VM

### Current state

- Front-end pipeline is stable: lexer, parser, expander, semantic analyser,
  emitter, and VM execution all run on one bytecode format.
- `etac` produces `.etac` bytecode files and `etai` can execute them directly.
- Core optimisations are implemented in dedicated passes (see
  [Optimization](optimization.md)).
- Runtime dispatch, serializer, disassembler, and diagnostics are integrated
  with the same opcode set.

### Notes

- Performance characteristics vary by workload; use the benchmark guidance in
  [Optimization](optimization.md) and project tests for validation.

---

## 2 · Runtime and GC

### Current state

- Heap is managed by a mark-sweep collector with explicit GC roots.
- VM stacks, globals, frames, trail, and runtime-managed structures
  participate in root enumeration.
- Logic and CLP backtracking state is captured on the unified trail and
  restored by `trail-mark` / `unwind-trail`.

### Notes

- GC is stop-the-world at collection boundaries.

---

## 3 · Logic and Constraints

### Current state

- Native unification primitives are built into the VM (`logic-var`, `unify`,
  `deref-lvar`, `trail-mark`, `unwind-trail`, `copy-term`, `ground?`).
- Logic variables support:
  - Optional debug names (`logic-var/named`, `var-name`)
  - Configurable occurs-check policy (`set-occurs-check!`, `occurs-check-mode`)
  - Attributed-variable operations (`put-attr`, `get-attr`, `del-attr`, `attr-var?`)
- Constraint subsystems include:
  - CLP(FD) domain narrowing and propagation
  - Native all-different propagation
  - Boolean constraints in `std.clpb`
  - Linear arithmetic constraints in `std.clpr`
- Runtime hook/propagation mechanisms are unified with VM backtracking.

### Detailed status

- [Logic Programming](logic.md): language-level model and VM behavior.
- [CLP](clp.md): constraints API and solver behavior.
- [Logic & CLP Status](logic-next-steps.md): consolidated subsystem status.

---

## 4 · Networking and Actors

### Current state

- nng-backed messaging primitives are available across IPC/TCP/inproc.
- Actor workflows are supported for both process actors (`spawn`) and
  in-process thread actors (`spawn-thread` / `spawn-thread-with`).
- Serialization/deserialization for message payloads is built in, with binary
  and text formats.
- Lifecycle helpers are available (`spawn-wait`, `spawn-kill`, `thread-join`,
  `thread-alive?`).

### Detailed docs

- [Networking Primitives](networking.md)
- [Message Passing & Actors](message-passing.md)
- [Network & Message-Passing Parallelism](network-message-passing.md)

---

## 5 · Tooling

### Current state

- LSP server provides parsing/analysis-powered editor integration.
- DAP server supports breakpoints, stepping, stack/local inspection, eval, and
  heap/GC-oriented views.
- VS Code extension ships syntax, snippets, and debugger integration.

### Detailed docs

- [Architecture](architecture.md)
- [Build](build.md)
- [Examples](examples.md)

---

## 6 · Known Gaps

The following capabilities are not part of the current baseline:

- WAM-style dedicated logic bytecode layer.
- Native tabling engine for logic relations.
- Distributed actor supervision/name registry as built-in runtime features.

