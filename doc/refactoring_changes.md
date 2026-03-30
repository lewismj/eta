# Refactoring Changes - Code Quality Improvements

This document summarizes the refactoring changes made to address the code review concerns.

## Changes Made

### 1. Fixed Incomplete Closure Emission (`emitter.cpp`)

**Problem:** The `Emitter::emit_node` for `core::Lambda` had a TODO for emitting code to push upvalues onto the stack before `OpCode::MakeClosure`. Without this, `MakeClosure` would pop unrelated values, causing corruption.

**Solution:** 
- Implemented proper upvalue emission by checking if each captured binding is:
  - A local in the current frame (`binding_to_slot` map) → emit `LoadLocal`
  - An upvalue from outer scope (`binding_to_upval` map) → emit `LoadUpval`
  - A global → emit `LoadGlobal`
- Updated `emit_lambda` to populate `binding_to_slot` and `binding_to_upval` maps from lambda parameters and upvalues.

### 2. Extended VM Arithmetic for All Numeric Types (`vm.cpp`)

**Problem:** Large integers that don't fit in 47-bit NaN-box payload are heap-allocated as `ObjectKind::Fixnum`, and raw doubles (flonums) weren't supported at all.

**Solution:**
- Created a `classify_number` helper that handles:
  - Immediate fixnums (`Tag::Fixnum`)
  - Heap-allocated fixnums (`Tag::HeapObject` + `ObjectKind::Fixnum`)
  - Raw IEEE 754 doubles (flonums - unboxed values)
- Mixed operations follow numeric tower rules: if either operand is a flonum, result is flonum
- Integer division that produces non-integer result returns flonum

### 3. Added String Type Unification Helpers (`factory.h`)

**Problem:** Small strings are interned (`Tag::String`) while large strings are heap-allocated (`Tag::HeapObject`). Every part of the system processing strings had to redundantly check both tags.

**Solution:** Added three helper functions:
- `is_string(LispVal, Heap&)` - Check if a value is any kind of string
- `get_string_value(LispVal, InternTable&, Heap&)` - Extract string value uniformly
- `string_equal(LispVal, LispVal, InternTable&, Heap&)` - Compare strings with fast path for identical interned strings

### 4. Refactored GC Sweep to Single-Pass (`mark_sweep_gc.cpp`)

**Problem:** `MarkSweepGC::sweep` used a two-pass approach: first iterating to collect IDs into a `std::vector`, then iterating to deallocate.

**Solution:**
- Implemented batched single-pass sweep using `std::array<ObjectId, 256>` fixed buffer
- Avoids dynamic memory allocation during GC
- Iterates multiple times only if more than 256 objects need freeing

### 5. Fixed InternTable Race Condition (`intern_table.cpp`)

**Problem:** `InternId` was fetched via `fetch_add` before entering the critical section. Multiple threads racing to intern the same string would leak IDs.

**Solution:**
- Moved `fetch_add` inside the critical section (after acquiring the lock)
- Double-check pattern now prevents ID waste: if another thread inserted while waiting, return their ID
- Removed redundant MSVC-specific locks on concurrent map reads

### 6. Type-Safe Function Indices (`emitter.h`, `emitter.cpp`, `vm.h`, `vm.cpp`)

**Problem:** Raw `BytecodeFunction*` pointers were stored in `LispVal` constants, making the system fragile to memory management changes.

**Solution:**
- `BytecodeFunctionRegistry` now returns indices (`uint32_t`) instead of pointers
- Indices are stored with a high-bit tag (`1ULL << 63`) to distinguish from legacy pointers
- Added `FunctionResolver` callback type to VM for index resolution
- VM's `MakeClosure` opcode now resolves indices via the resolver
- Backward compatible: still handles legacy raw pointers

### 7. Refactored Semantic Analyzer to Handler Registry (`semantic_analyzer.cpp`)

**Problem:** `analyze_list` was a 400+ line monolithic if-else chain handling all special forms.

**Solution:**
- Extracted each form handler to a separate function: `handle_if`, `handle_begin`, `handle_set`, `handle_lambda`, `handle_quote`, `handle_dynamic_wind`, `handle_values`, `handle_call_with_values`, `handle_call_cc`, `handle_let`, `handle_letrec`, `handle_case`, `handle_do`
- Created `get_form_handlers()` returning `std::unordered_map<std::string_view, SpecialFormHandler>`
- `analyze_list` now uses simple dispatch: lookup handler by name, call if found, else generic application
- Clear separation between core forms (required) and derived forms (optional, normally expanded)

### 8. Refactored GC Visitors (`mark_sweep_gc.h`)

**Problem:** Multiple nested visitors duplicated the same "is it a heap pointer?" check.

**Solution:**
- Extracted `push_if_heap_ref` static utility function
- Removed `RootScan` and `PushIfHeap` nested visitors
- Removed unused includes

### 9. Fixed Missing `is_boxed` Check (`value_visit.h`)

**Problem:** `visit_value` switched on `tag(v)` without first checking `is_boxed(v)`.

**Solution:**
- Added `is_boxed(v)` check at the start of `visit_value`
- Added `visit_flonum(double)` method to `ValueVisitor` interface
- Raw doubles are now properly dispatched to `visit_flonum`

### 10. Implemented Missing Opcodes (`vm.cpp`)

**Problem:** `Values`, `CallWithValues`, `DynamicWind` returned `NotImplemented`.

**Solution:**
- Added `MultipleValues` type for wrapping multiple return values
- Implemented `OpCode::Values`: Packs top N stack values
- Implemented `OpCode::CallWithValues`: Producer/consumer pattern
- Implemented `OpCode::DynamicWind`: Sequential before/body/after execution

## Future Considerations

1. **Full Dynamic-Wind Support:** Current implementation handles sequential execution but doesn't track winding/unwinding state for continuation jumps across dynamic-wind boundaries.

2. **Boolean/Nil Representation:** `True`, `False`, and `Nil` share `Tag::Nil` with different payloads, complicating debugging.

