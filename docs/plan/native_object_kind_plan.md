# NativeObject - Open Heap Extension for Sidecars

[Back to README](../../README.md) |
[Native Sidecar Plan](native_sidecar_plan.md) |
[Next Steps](../next-steps.md)

Status: In progress (2026-05-10). NO1 implemented; NO2-NO4 pending. Follows NS10.

---

## 1. Problem

Every sidecar that manages heap-allocated opaque objects (for example
`TensorPtr`, `NNModulePtr`, `OptimizerPtr`) currently requires edits to core:

1. A new `ObjectKind` enumerator in `eta/core/src/eta/runtime/memory/heap.h`.
2. A new `ETA_ENUM_CASE` entry in `heap.h`.
3. A new `case` in `visit_heap_object` in `heap_visit.h` (or at least a new
   entry in the leaf-fallthrough list).

Since `ObjectKind` is a closed `enum class : uint8_t`, a third-party sidecar
cannot allocate a named object kind without patching core. This conflicts with
NS10 and a self-contained sidecar authoring path.

---

## 2. Objective

Add one permanent `ObjectKind::NativeObject` kind, with vtable-based behavior
and SDK callbacks, so future sidecars can allocate typed heap objects without
adding new `ObjectKind` entries.

Existing torch/stats/nng/log sidecars can migrate later; their current kinds
remain until migration is stable.

Packaging boundary for LightGBM:

- LightGBM is not part of stdlib; it lives under `packages/ml/native/lightgbm`.
- This is a peer package category alongside existing `packages/stdlib` and
  `packages/example`.
- Build logic and tests for LightGBM stay package-local (no new coupling into
  core Eta build files).

---

## 3. Design (corrected)

### 3.1 C ABI vtable type (added to `sdk.h`)

`EtaNativeObjectVTable` is part of the sidecar C ABI and must live in
`sdk.h` (not in `heap.h`).

```c
typedef struct EtaNativeObjectVTable {
    /* Human-readable name for inspector/errors. */
    const char* type_name;

    /*
     * Called when the heap object is destroyed.
     * Releases sidecar-owned payload resource.
     */
    void (*destroy)(void* user_data);

    /*
     * Optional GC trace callback.
     * Sidecar calls trace_fn(ctx, boxed_lisp_val) for every referenced Eta value.
     */
    void (*trace)(void* user_data, void* ctx, void (*trace_fn)(void* ctx, uint64_t val));

    /* Optional display callback for heap inspector. */
    void (*display)(void* user_data, FILE* out);
} EtaNativeObjectVTable;
```

### 3.2 In-heap wrapper (added to `heap.h`)

```cpp
struct NativeObjectHeader {
    const EtaNativeObjectVTable* vtable{nullptr};
    void* user_data{nullptr};

    ~NativeObjectHeader() {
        if (vtable != nullptr && vtable->destroy != nullptr) {
            vtable->destroy(user_data);
        }
    }
};
```

This guarantees payload cleanup through the existing `HeapEntry::destructor`
path that already runs `T::~T()`.

### 3.3 `ObjectKind` change (`heap.h`)

Add one enumerator and one enum-string entry:

```cpp
enum class ObjectKind : std::uint8_t {
    // ...existing entries...
    CompoundTerm,
    NativeObject,  // sidecar-managed opaque object
};
```

```cpp
ETA_ENUM_TO_STRING_BEGIN(ObjectKind)
    // ...existing entries...
    ETA_ENUM_CASE(CompoundTerm)
    ETA_ENUM_CASE(NativeObject)
ETA_ENUM_TO_STRING_END("Unknown")
```

### 3.4 `heap_visit.h` behavior

`NativeObject` is a leaf for base visitor traversal.

```cpp
case NativeObject:
    return v.visit_leaf(ObjectKind::NativeObject, payload);
```

Trace policy by stage:

- NO1/NO2: keep leaf behavior only.
- NO4: wire mark-phase tracing through `EtaNativeObjectVTable::trace`.

Until NO4 lands, runtime must reject allocation when `vtable->trace != nullptr`
to avoid unsound GC behavior.

### 3.5 SDK additions (`sdk.h`)

Use `runtime_context` for runtime-owned callbacks. Keep `user_data` semantics
unchanged for sidecar registration bridges (`register_primitive`, `report_error`).

```c
#include <stddef.h>
#include <stdint.h>

typedef int (*EtaAllocNativeObjectFnV1)(
    void* runtime_context,
    const EtaNativeObjectVTable* vtable,
    void* payload,
    uint64_t* out_val);

typedef void* (*EtaGetNativeObjectFnV1)(
    void* runtime_context,
    uint64_t val,
    const EtaNativeObjectVTable* vtable);

typedef struct EtaNativeApiV1 {
    uint32_t struct_size;
    const char* abi_id;
    void* user_data;
    void* runtime_context;
    EtaRegisterPrimitiveFnV1 register_primitive;
    EtaReportErrorFnV1 report_error;

    /* New in ABI v1 revision 2 */
    EtaAllocNativeObjectFnV1 alloc_native_object;
    EtaGetNativeObjectFnV1 get_native_object;
} EtaNativeApiV1;

#define ETA_NATIVE_API_V1_HAS_FIELD(api_ptr, field) \
    ((api_ptr) != NULL && \
     (api_ptr)->struct_size >= \
         (uint32_t)(offsetof(EtaNativeApiV1, field) + sizeof((api_ptr)->field)))
```

Compatibility requirement for sidecars:

1. Check `ETA_NATIVE_API_V1_HAS_FIELD(api, alloc_native_object)`.
2. Check `ETA_NATIVE_API_V1_HAS_FIELD(api, get_native_object)`.
3. Then check field values are non-null.

Do not read appended fields without the `struct_size` gate.

### 3.6 `sidecar_loader.cpp` wiring

`api.user_data` remains registration-local bridge data.  
New callbacks accept and use `api.runtime_context`.

```cpp
// alloc_native_object callback
[](void* runtime_context,
   const EtaNativeObjectVTable* vtable,
   void* payload,
   uint64_t* out) -> int {
    if (runtime_context == nullptr || vtable == nullptr || out == nullptr) {
        return ETA_NATIVE_STATUS_ERROR;
    }
    if (vtable->trace != nullptr) {
        // Temporary NO1/NO2 guard until NO4 mark integration.
        return ETA_NATIVE_STATUS_ERROR;
    }

    auto* binding = static_cast<SidecarRuntimeBindingV1*>(runtime_context);
    auto result = binding->heap->allocate<NativeObjectHeader, ObjectKind::NativeObject>(
        NativeObjectHeader{vtable, payload});
    if (!result) return ETA_NATIVE_STATUS_ERROR;
    *out = ops::box(Tag::HeapObject, *result).bits;
    return ETA_NATIVE_STATUS_OK;
};

// get_native_object callback
[](void* runtime_context, uint64_t raw_val, const EtaNativeObjectVTable* vtable) -> void* {
    if (runtime_context == nullptr || vtable == nullptr) return nullptr;
    auto* binding = static_cast<SidecarRuntimeBindingV1*>(runtime_context);
    LispVal v{raw_val};
    if (!ops::is_boxed(v) || ops::tag(v) != Tag::HeapObject) return nullptr;
    auto* hdr = binding->heap->try_get_as<ObjectKind::NativeObject, NativeObjectHeader>(
        ops::payload(v));
    if (!hdr || hdr->vtable != vtable) return nullptr;
    return hdr->user_data;
};
```

### 3.7 Package and build isolation rules

For any new sidecar package (including LightGBM):

1. Package path is outside stdlib when domain-specific:
   `packages/ml/native/lightgbm`.
2. All native build logic lives under the package folder, with a package-local
   `CMakeLists.txt`.
3. Package-native unit tests and Eta smoke tests live under that package folder.
4. No NO3 changes to `eta/CMakeLists.txt`, `eta/core/*`, or root `cmake/*`
   beyond NO1/NO2 NativeObject runtime support.
5. LightGBM source/binary fetch is pinned to tag `v4.6.0`.

---

## 4. How a sidecar uses this (LightGBM example)

```cpp
#include <LightGBM/c_api.h>
#include <eta/native/sdk.h>

static const EtaNativeObjectVTable lgbm_dataset_vtable = {
    "lgbm-dataset",
    [](void* p) { LGBM_DatasetFree(static_cast<DatasetHandle>(p)); },
    nullptr,
    nullptr,
};

static const EtaNativeObjectVTable lgbm_booster_vtable = {
    "lgbm-booster",
    [](void* p) { LGBM_BoosterFree(static_cast<BoosterHandle>(p)); },
    nullptr,
    nullptr,
};

struct LgbmNativeRuntime {
    void* runtime_context{nullptr};
    EtaAllocNativeObjectFnV1 alloc_native_object{nullptr};
    EtaGetNativeObjectFnV1 get_native_object{nullptr};
};

static LgbmNativeRuntime g_rt{};

static DatasetHandle get_dataset(uint64_t val) {
    return static_cast<DatasetHandle>(
        g_rt.get_native_object(g_rt.runtime_context, val, &lgbm_dataset_vtable));
}

static BoosterHandle get_booster(uint64_t val) {
    return static_cast<BoosterHandle>(
        g_rt.get_native_object(g_rt.runtime_context, val, &lgbm_booster_vtable));
}

ETA_NATIVE_EXPORT int eta_register_lgbm_extension_v1(
    const EtaNativeApiV1* api, EtaExtensionInfoV1* out_info)
{
    const bool has_alloc =
        ETA_NATIVE_API_V1_HAS_FIELD(api, alloc_native_object) &&
        api->alloc_native_object != nullptr;
    const bool has_get =
        ETA_NATIVE_API_V1_HAS_FIELD(api, get_native_object) &&
        api->get_native_object != nullptr;

    if (!has_alloc || !has_get || api->runtime_context == nullptr) {
        api->report_error(api->user_data,
            "lgbm sidecar requires NativeObject API (runtime too old)");
        return ETA_NATIVE_STATUS_ERROR;
    }

    g_rt.runtime_context = api->runtime_context;
    g_rt.alloc_native_object = api->alloc_native_object;
    g_rt.get_native_object = api->get_native_object;

    out_info->struct_size       = sizeof(EtaExtensionInfoV1);
    out_info->abi_id            = ETA_NATIVE_ABI_ID_V1;
    out_info->extension_id      = "eta.lgbm.sidecar";
    out_info->extension_version = "0.1.0";
    return ETA_NATIVE_STATUS_OK;
}
```

Do not store `api` pointer itself. Loader currently constructs `EtaNativeApiV1`
as a stack value during registration; pointer lifetime ends when entrypoint
returns.

---

## 5. Files changed

| File | Change | Size |
|---|---|---|
| `eta/core/src/eta/runtime/memory/heap.h` | Add `ObjectKind::NativeObject` and `NativeObjectHeader` with destructor | +30 lines |
| `eta/core/src/eta/runtime/memory/heap_visit.h` | Add `NativeObject` leaf case | +3 lines |
| `eta/core/src/eta/native/sdk.h` | Add `EtaNativeObjectVTable`, alloc/get typedefs, API fields, size-check macro | +55 lines |
| `eta/core/src/eta/native/sidecar_loader.cpp` | Wire callbacks using `runtime_context` | +35 lines |

Total core delta: about 120 lines (NO1/NO2 only).

NO3 package files are isolated under `packages/ml/native/lightgbm`.

---

## 6. What does NOT change

- Existing `ObjectKind` entries (`Tensor`, `NNModule`, `Optimizer`, etc.) stay
  unchanged.
- Existing sidecars continue to use `try_get_as<ObjectKind::X, T>` until
  migrated.
- `try_get_as<Kind, T>` API is unchanged.
- ABI remains `eta-native-v1`; extension fields are additive and gated by
  `struct_size`.

---

## 7. Migration path for existing sidecars (optional, not blocking)

Example torch migration:

1. Replace `heap.allocate<TensorPtr, ObjectKind::Tensor>(...)` with
   `api->alloc_native_object(api->runtime_context, &tensor_vtable, ptr, &out)`.
2. Replace `heap.try_get_as<ObjectKind::Tensor, TensorPtr>(id)` with
   `api->get_native_object(api->runtime_context, val, &tensor_vtable)`.
3. After all sidecars migrate, old kinds can be deprecated and removed in a
   major version.

---

## 8. Staged work items

### NO1 - Core NativeObject support (Implemented 2026-05-10)

Scope:

1. Add `NativeObjectHeader` to `heap.h`.
2. Add `ObjectKind::NativeObject` enumerator and enum string entry.
3. Add `NativeObject` leaf case to `heap_visit.h`.
4. Ensure `NativeObjectHeader` destructor calls `vtable->destroy`.

Gate:

1. Existing heap tests pass.
2. New tests:
   - allocate/deallocate NativeObject and verify destroy callback called once
   - heap teardown runs NativeObject destroy callback
   - null vtable is handled without calling destroy

Implemented in:

- `eta/core/src/eta/native/sdk.h`
- `eta/core/src/eta/runtime/memory/heap.h`
- `eta/core/src/eta/runtime/memory/heap_visit.h`
- `eta/qa/test/src/heap_tests.cpp`

### NO2 - SDK API surface

Scope:

1. Add `EtaNativeObjectVTable` and alloc/get typedefs to `sdk.h`.
2. Add two new fields to `EtaNativeApiV1`.
3. Add `ETA_NATIVE_API_V1_HAS_FIELD` macro and document mandatory usage.
4. Wire callbacks in loader with `runtime_context` (not `user_data`).

Gate:

1. Existing sidecar loader tests pass.
2. New tests:
   - sidecar compiled with new SDK loads on old runtime and fails gracefully
   - sidecar using `struct_size` gate succeeds on both old/new runtime
   - callback round-trip alloc/get works and vtable mismatch returns null

### NO3 - LightGBM sidecar scaffold

Scope:

1. Create package hierarchy:
   - `packages/ml/native/lightgbm/eta.toml`
   - `packages/ml/native/lightgbm/src/...`
   - `packages/ml/native/lightgbm/tests/...`
2. Add package-local native implementation:
   - `packages/ml/native/lightgbm/src/eta/lightgbm/lightgbm_primitives.h`
   - (and companion `.cpp`/headers as needed)
3. Add package-local build system:
   - `packages/ml/native/lightgbm/CMakeLists.txt`
   - `packages/ml/native/lightgbm/cmake/FetchLightGBM.cmake`
   - pin upstream LightGBM to tag `v4.6.0`
4. Add package Eta wrapper module (inside package `src`, not stdlib) and bind
   `dataset-from-list`, `booster-create`, `train!`, `predict`, `save`, `load`,
   `eval`, `num-trees`, `feature-importance`.
5. Add package-local tests:
   - native unit tests under `packages/ml/native/lightgbm/tests/unit`
   - Eta smoke/integration tests under `packages/ml/native/lightgbm/tests/eta`
6. Add cookbook example `cookbook/ml/lightgbm.eta` using the package import
   path (not `std.*`).

Gate:

1. The package builds via its own `packages/ml/native/lightgbm/CMakeLists.txt`
   without adding new build wiring to core Eta CMake files.
2. Package-local native unit tests pass.
3. Package-local Eta smoke/integration tests pass.
4. Cookbook example runs on Windows and Linux once the package is built and
   resolved.
5. LightGBM dependency is pinned to `v4.6.0`.
6. No additional core `ObjectKind` edits beyond NO1/NO2.

Notes:

- Do not add `stdlib/std/lgbm.eta`.
- Do not place LightGBM under `packages/stdlib/native/*`.
- Package remains independent; built artifact is usable through package
  resolution/loading.

### NO4 - NativeObject GC trace + inspector

Scope:

1. Integrate `vtable->trace` into mark phase.
2. DAP/Heap Inspector shows `type_name` and optional display output.
3. `eta doctor` reports NativeObject allocations by type.

Gate:

1. Tracing test: sidecar object that references Eta values keeps them live.
2. Inspector test shows `lgbm-dataset` / `lgbm-booster`.

---

## 9. Risks and mitigations

| Risk | Mitigation |
|---|---|
| New sidecar reads appended API fields on old runtime | Mandatory `struct_size` + `offsetof` gate before field access |
| Type confusion from non-static vtable pointers | Document static-storage requirement; add debug assert |
| Payload leak/double-free | `NativeObjectHeader` owns destroy callback in destructor; add unit tests |
| Unsound GC when sidecar stores Eta refs | Reject non-null `trace` until NO4; then test mark integration |
| Sidecar stores dangling `EtaNativeApiV1*` | Document API-table lifetime; store copied callbacks/context only |
| Inspector regression for existing torch objects | Existing object kinds and inspector paths remain unchanged |
| Domain package accidentally coupled to core build | Require package-local CMake/tests under `packages/ml/native/lightgbm`; no NO3 edits to core CMake |

---

## 10. Review-driven recommendations (must-fix)

1. Keep ABI-owned types in `sdk.h` (`EtaNativeObjectVTable`).
2. Use `runtime_context` for alloc/get callback first argument.
3. Keep `user_data` behavior unchanged for registration bridges.
4. Require `struct_size` gating macro in all new sidecars.
5. Do not defer destroy wiring; ship it in NO1.
6. Do not ship `trace` field as active behavior before GC mark integration.
7. Prohibit storing raw `EtaNativeApiV1*` beyond registration call scope.
