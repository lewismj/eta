# Finalizers and Guardians Proposal for Eta

## Codebase review summary

This proposal is based on the current runtime shape:

- GC is mark/sweep and stop-the-world in `eta/core/src/eta/runtime/memory/mark_sweep_gc.h` and `.cpp`.
- Root enumeration is VM-driven in `eta/core/src/eta/runtime/vm/vm.cpp` (`VM::collect_garbage`), with a Driver wrapper that also roots bytecode constants in `eta/interpreter/src/eta/interpreter/driver.h` (`collect_garbage_with_registry_roots`).
- Heap object flags are tight (`ObjectHeader.flags : 3` in `heap.h`), and cons-pool already uses bit 1 (`POOL_ALLOCATED_BIT` in `cons_pool.h`).
- Builtin registration order is strict and patch-validated (`builtin_names.h`, `all_primitives.h`, `BuiltinEnvironment::begin_patching`/`verify_all_patched`).

Implication: finalizers/guardians should be implemented with side tables and staged GC logic, not by overloading a single extra flag bit.

---

## Scope and semantics for v1

1. `register-finalizer!` attaches one finalizer procedure to one heap object.
2. Finalizer is invoked as `(proc obj)` (arity 1 expected).
3. Finalizer runs at most once per registration.
4. Re-registration replaces prior finalizer for that object.
5. No ordering guarantee between multiple finalizers becoming ready in the same GC.
6. Guardian API is explicit:
   - `(make-guardian)`
   - `(guardian-track! guardian obj)`
   - `(guardian-collect guardian)` -> object or `#f`
7. Recommended v1 constraint: reject cons-pool objects (`heap.cons_pool().owns(id)`) for finalizers/guardians to avoid ObjectId reuse complexity in first delivery.

---

## Implementation plan (4 testable steps)

## Step 1: Finalizer substrate in Heap + GC (internal only)

### Changes

- Add finalizer side tables to `eta/core/src/eta/runtime/memory/heap.h`/`.cpp`:
  - `unordered_map<ObjectId, LispVal> finalizer_table_`
  - `deque<PendingFinalizer> pending_finalizers_` where `PendingFinalizer{ LispVal obj; LispVal proc; }`
  - heap APIs: register/remove/fetch finalizer, enqueue/dequeue pending finalizers.
- Extend GC in `mark_sweep_gc.h`/`.cpp` with a two-stage algorithm:
  1. Normal root marking.
  2. Ephemeron-style pass for finalizers:
     - If key object is marked, mark/traverse finalizer proc.
     - Iterate to fixpoint (needed when marking values causes new keys to become marked).
  3. Detect dead finalized objects:
     - Move `(obj, proc)` to pending queue.
     - Rescue-mark `obj` and `proc` graphs so they survive until VM execution.
  4. Sweep unmarked remainder.
- Keep this step runtime-internal: no Scheme builtins yet.

### Why this step first

It proves GC correctness before introducing user-facing API surface.

### Tests to add now

Add low-level tests in `eta/test/src/gc_tests.cpp`:

1. Dead finalized object is queued and not freed in the same cycle.
2. Finalizer table does not strongly keep key object alive (ephemeron behavior).
3. Pending finalizer entries survive another GC cycle (rescued roots).

---

## Step 2: VM safe-point finalizer execution

### Changes

- In `eta/core/src/eta/runtime/vm/vm.h`/`.cpp` add:
  - `process_pending_finalizers(std::size_t budget = N)`
  - reentrancy guard `processing_finalizers_`
- Drain queue only at VM safe points:
  - at run-loop instruction boundaries when queue is non-empty
  - before `execute()` returns
  - before `call_value()` returns to host C++
- Execute each as `call_value(proc, {obj})`.
- Failure policy: isolate errors (do not crash VM, do not block later finalizers).
- Ensure queue items are rooted during GC (via VM root walk or GC internal roots pass).

### Tests to add now

In VM-oriented tests (`vm_tests.cpp` or a dedicated finalizer test file):

1. Pending finalizer executes once and mutates observable state.
2. One failing finalizer does not prevent later finalizers from running.
3. If finalizer does not resurrect object, object is reclaimed on a later GC.

---

## Step 3: Guardian type + Scheme primitives

### Changes

- Add `types::Guardian` in `eta/core/src/eta/runtime/types/guardian.h`.
  - Contains ready queue (for objects already detected as dead and handed to guardian).
- Wire Guardian through type system:
  - `ObjectKind::Guardian` in `heap.h`
  - include in `types/types.h`
  - factory helper in `factory.h`
  - visitor support in `heap_visit.h` and `mark_sweep_gc.h` visitor
  - printable form in `value_formatter.cpp`
- Add guardian weak-tracking side tables in `Heap`:
  - guardian -> tracked object ids
  - object id -> guardian ids
- GC integration:
  - when tracked object is dead and guardian is live, move object into guardian ready queue
  - rescue-mark object graph for this cycle
  - clean weak indexes when guardian dies or object is delivered
- Add primitives in `core_primitives.h` and `builtin_names.h` (in exact slot order):
  - `register-finalizer!`
  - `unregister-finalizer!`
  - `make-guardian`
  - `guardian-track!`
  - `guardian-collect`

### Tests to add now

1. `guardian-track!` + forced GC makes object retrievable via `guardian-collect`.
2. `guardian-collect` returns `#f` when queue empty.
3. Dead guardian does not retain tracked objects.
4. Builtin order/metadata stays synchronized (`builtin_sync_tests.cpp` continues to pass).

---

## Step 4: Hardening, resurrection, and deterministic testing hooks

### Changes

- Add deterministic test hook:
  - either VM test API (`drain_finalizers_for_test`) or internal primitive (for tests only) to force `collect_garbage` + drain.
- Finalize behavior docs and invariants:
  - resurrection semantics: object revived by finalizer remains alive
  - at-most-once finalizer execution per registration
  - no finalization order guarantee

### Tests to add now

1. Resurrection test: finalizer stores object in a global root; object survives next collection.
2. Finalizer + guardian on same object: both mechanisms behave consistently and do not double-free.
3. Cyclic finalizable objects eventually reclaimed once not resurrected.
4. Type and arity validation tests for new primitives.

---

## Notes on risk control

- Do not use a single `HAS_FINALIZER` bit as the primary state machine; it is insufficient for lifecycle transitions and conflicts with existing bit pressure.
- Keep finalizer/guardian metadata in side tables with explicit transitions:
  - `registered -> queued -> executing -> done`
- Keep early scope narrow (non-cons-pool objects) unless generation tracking is introduced for reused ObjectIds.

---

## Suggested implementation order in git

1. Step 1 + tests
2. Step 2 + tests
3. Step 3 + tests
4. Step 4 + tests/docs

This gives a working, testable increment at each checkpoint and avoids landing a large GC+VM+primitive change set in one patch.

