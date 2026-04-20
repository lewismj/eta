# Finalizers & Guardians

[<- Back to README](../README.md) · [Runtime & GC](runtime.md) · [Modules](modules.md)

---

## Overview

Eta provides two weak-lifecycle tools for heap objects:

- `register-finalizer!` / `unregister-finalizer!`
- `make-guardian` / `guardian-track!` / `guardian-collect`

Use them when an object has external lifecycle work, such as closing ports or
releasing network handles, and you want cleanup to happen even if user code
forgets an explicit close.

Finalizers and guardians are not deterministic resource-management primitives.
They are a safety net. Explicit close/release calls should remain the primary
path in normal program logic.

---

## Primitive API

| Primitive | Signature | Meaning |
|-----------|-----------|---------|
| `register-finalizer!` | `(obj proc) -> #t` | Register or replace one finalizer for `obj`. Finalizer is invoked as `(proc obj)`. |
| `unregister-finalizer!` | `(obj) -> boolean` | Remove the finalizer registration for `obj`. |
| `make-guardian` | `() -> guardian` | Create a guardian object. |
| `guardian-track!` | `(guardian obj) -> #t` | Track `obj` weakly under `guardian`. |
| `guardian-collect` | `(guardian) -> obj or #f` | Pop one ready object from the guardian queue, or `#f` when empty. |

---

## Semantics

- At-most-once: a finalizer runs at most once for each registration.
- Re-registration replaces the previous finalizer for the same object.
- No ordering guarantee: if multiple objects become ready in one GC cycle,
  finalization order is unspecified.
- Objects are delivered weakly: registration does not keep the key object alive.
- Current scope excludes cons-pool objects for both finalizers and guardians.

### Resurrection

If a finalizer stores its argument into a live root (for example a global or
another reachable object), the object is resurrected and remains live in later
collections until those roots are removed.

---

## When To Use

Prefer finalizers for safety cleanup on objects that wrap external resources:

- ports (`close-port`, `close-input-port`, `close-output-port`)
- network/socket wrappers
- other host-backed handles that need eventual release

Use guardians when user code wants explicit post-mortem handling of dead
objects without executing cleanup code immediately.

---

## Example Pattern (Ports)

```scheme
(module m
  (import std.io)
  (begin
    (define p (open-output-string))

    ;; Safety net: explicit close is still recommended in normal flow.
    (register-finalizer! p
      (lambda (port)
        (close-port port)))

    ;; ... program logic ...
    (close-port p)))
```

The explicit `close-port` handles normal control flow; the finalizer covers
forgotten or exceptional paths.

---

## Runtime Notes

Implementation lives in side tables in:

- `eta/core/src/eta/runtime/memory/heap.h`
- `eta/core/src/eta/runtime/memory/mark_sweep_gc.h`
- `eta/core/src/eta/runtime/vm/vm.cpp`

Behavioral tests are in:

- `eta/test/src/gc_tests.cpp`
- `eta/test/src/vm_tests.cpp`
