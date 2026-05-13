#include "eta/native/runtime_binding.h"
#include "eta/native/sdk.h"
#include "eta/runtime/builtin_catalog.h"
#include "eta/runtime/builtin_env.h"
#include "eta/runtime/error.h"
#include "eta/runtime/nanbox.h"
#include "eta/runtime/types/types.h"
#include "eta/log/log_primitives.h"
#include "eta/nng/nng_primitives.h"
#include "eta/stats/stats_primitives.h"
#include "eta/torch/torch_primitives.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <new>
#include <string>
#include <string_view>
#include <vector>

namespace {

using PrimitiveFunc = eta::runtime::types::PrimitiveFunc;
using PrimitiveArgs = std::span<const eta::runtime::nanbox::LispVal>;
using PrimitiveResult = std::expected<eta::runtime::nanbox::LispVal,
                                      eta::runtime::error::RuntimeError>;

struct RegisteredPrimitive {
    std::string name;
    std::uint32_t arity{0};
    std::uint8_t has_rest{0};
    std::unique_ptr<PrimitiveFunc> func;
};

PrimitiveResult native_test_add(PrimitiveArgs) {
    return eta::runtime::nanbox::Nil;
}

PrimitiveResult native_test_rest(PrimitiveArgs) {
    return eta::runtime::nanbox::Nil;
}

PrimitiveFunc g_add_primitive = native_test_add;
PrimitiveFunc g_rest_primitive = native_test_rest;

using PrimitiveBatch = std::vector<RegisteredPrimitive>;

std::vector<std::unique_ptr<PrimitiveBatch>> g_log_primitive_batches;
std::vector<std::unique_ptr<PrimitiveBatch>> g_stats_primitive_batches;
std::vector<std::unique_ptr<PrimitiveBatch>> g_torch_primitive_batches;
std::vector<std::unique_ptr<PrimitiveBatch>> g_nng_primitive_batches;

struct NativeRoundtripPayload {
    std::uint64_t cookie{0};
};

extern "C" void native_roundtrip_destroy(void* user_data) {
    delete static_cast<NativeRoundtripPayload*>(user_data);
}

struct NativeTracePayload {
    std::uint64_t traced_value{0};
};

extern "C" void native_trace_destroy(void* user_data) {
    delete static_cast<NativeTracePayload*>(user_data);
}

extern "C" void native_trace_callback(void* user_data,
                                      void* ctx,
                                      void (*trace_fn)(void* ctx, std::uint64_t val)) {
    if (trace_fn == nullptr) return;
    auto* payload = static_cast<NativeTracePayload*>(user_data);
    if (payload == nullptr) return;
    trace_fn(ctx, payload->traced_value);
}

constexpr EtaNativeObjectVTable kNativeRoundtripVTable{
    .type_name = "eta.native.roundtrip.payload",
    .destroy = &native_roundtrip_destroy,
    .trace = nullptr,
    .display = nullptr,
};

constexpr EtaNativeObjectVTable kNativeRoundtripMismatchVTable{
    .type_name = "eta.native.roundtrip.mismatch",
    .destroy = &native_roundtrip_destroy,
    .trace = nullptr,
    .display = nullptr,
};

constexpr EtaNativeObjectVTable kNativeTraceVTable{
    .type_name = "eta.native.trace.payload",
    .destroy = &native_trace_destroy,
    .trace = &native_trace_callback,
    .display = nullptr,
};

void fill_extension_info(EtaExtensionInfoV1* out_info,
                         const char* abi,
                         const char* extension_id) {
    if (out_info == nullptr) return;
    out_info->struct_size = sizeof(EtaExtensionInfoV1);
    out_info->abi_id = abi;
    out_info->extension_id = extension_id;
    out_info->extension_version = "0.1.0";
}

int register_symbols(const EtaNativeApiV1* api) {
    if (api == nullptr || api->register_primitive == nullptr) {
        return ETA_NATIVE_STATUS_ERROR;
    }

    const int add_status = api->register_primitive(
        api->user_data,
        "native.test.add",
        2u,
        0u,
        static_cast<void*>(&g_add_primitive));
    if (add_status != ETA_NATIVE_STATUS_OK) return add_status;

    return api->register_primitive(
        api->user_data,
        "native.test.rest",
        1u,
        1u,
        static_cast<void*>(&g_rest_primitive));
}

template <typename Predicate>
int register_builtin_symbol_metadata(const EtaNativeApiV1* api, Predicate&& predicate) {
    if (api == nullptr || api->register_primitive == nullptr) {
        return ETA_NATIVE_STATUS_ERROR;
    }

    eta::runtime::BuiltinEnvironment builtins;
    eta::runtime::register_builtin_specs(builtins);
    for (const auto& symbol : builtins.specs()) {
        if (!predicate(symbol.name)) continue;
        const int status = api->register_primitive(
            api->user_data,
            symbol.name.c_str(),
            symbol.arity,
            static_cast<std::uint8_t>(symbol.has_rest),
            nullptr);
        if (status != ETA_NATIVE_STATUS_OK) return status;
    }
    return ETA_NATIVE_STATUS_OK;
}

[[nodiscard]] bool is_log_primitive_name(const std::string_view name) {
    return name.rfind("%log-", 0u) == 0u;
}

[[nodiscard]] bool is_stats_primitive_name(const std::string_view name) {
    return name == "%stats-mean-vec"
        || name == "%stats-var-vec"
        || name == "%stats-cov-matrix"
        || name == "%stats-cor-matrix"
        || name == "%stats-quantile-vec"
        || name == "%stats-ols-multi";
}

[[nodiscard]] bool is_torch_primitive_name(const std::string_view name) {
    return name.rfind("torch/", 0u) == 0u
        || name.rfind("nn/", 0u) == 0u
        || name.rfind("optim/", 0u) == 0u;
}

[[nodiscard]] bool is_nng_primitive_name(const std::string_view name) {
    return name.rfind("nng-", 0u) == 0u
        || name == "send!"
        || name == "recv!"
        || name == "spawn"
        || name == "spawn-kill"
        || name == "spawn-wait"
        || name == "current-mailbox"
        || name == "spawn-thread-with"
        || name == "spawn-thread"
        || name == "thread-join"
        || name == "thread-alive?"
        || name == "monitor"
        || name == "demonitor"
        || name == "enable-heartbeat";
}

[[nodiscard]] eta::native::SidecarRuntimeBindingV1* runtime_binding_or_null(
    const EtaNativeApiV1* api) {
    if (api == nullptr || api->runtime_context == nullptr) return nullptr;
    auto* binding =
        static_cast<eta::native::SidecarRuntimeBindingV1*>(api->runtime_context);
    if (binding->heap == nullptr || binding->intern_table == nullptr) return nullptr;
    return binding;
}

template <typename PopulateEnvironmentFn, typename PredicateFn>
int register_bound_symbol_set(const EtaNativeApiV1* api,
                              PopulateEnvironmentFn&& populate_environment,
                              PredicateFn&& predicate,
                              std::vector<std::unique_ptr<PrimitiveBatch>>& batches) {
    if (api == nullptr || api->register_primitive == nullptr) {
        return ETA_NATIVE_STATUS_ERROR;
    }

    eta::runtime::BuiltinEnvironment env;
    populate_environment(env);

    auto batch = std::make_unique<PrimitiveBatch>();
    batch->reserve(env.specs().size());
    for (const auto& spec : env.specs()) {
        if (!predicate(spec.name)) continue;
        RegisteredPrimitive primitive;
        primitive.name = spec.name;
        primitive.arity = spec.arity;
        primitive.has_rest = static_cast<std::uint8_t>(spec.has_rest ? 1u : 0u);
        primitive.func = std::make_unique<PrimitiveFunc>(spec.func);
        batch->push_back(std::move(primitive));
    }

    for (auto& primitive : *batch) {
        const int status = api->register_primitive(
            api->user_data,
            primitive.name.c_str(),
            primitive.arity,
            primitive.has_rest,
            static_cast<void*>(primitive.func.get()));
        if (status != ETA_NATIVE_STATUS_OK) return status;
    }

    batches.push_back(std::move(batch));
    return ETA_NATIVE_STATUS_OK;
}

int register_log_bound_symbols(const EtaNativeApiV1* api,
                               const eta::native::SidecarRuntimeBindingV1& binding) {
    return register_bound_symbol_set(
        api,
        [&binding](eta::runtime::BuiltinEnvironment& env) {
            eta::log::register_log_primitives(
                env, *binding.heap, *binding.intern_table, binding.vm);
        },
        is_log_primitive_name,
        g_log_primitive_batches);
}

int register_stats_bound_symbols(const EtaNativeApiV1* api,
                                 const eta::native::SidecarRuntimeBindingV1& binding) {
    return register_bound_symbol_set(
        api,
        [&binding](eta::runtime::BuiltinEnvironment& env) {
            eta::stats_bindings::register_stats_primitives(
                env, *binding.heap, *binding.intern_table, binding.vm);
        },
        is_stats_primitive_name,
        g_stats_primitive_batches);
}

int register_torch_bound_symbols(const EtaNativeApiV1* api,
                                 const eta::native::SidecarRuntimeBindingV1& binding) {
    return register_bound_symbol_set(
        api,
        [&binding](eta::runtime::BuiltinEnvironment& env) {
            eta::torch_bindings::register_torch_primitives(
                env, *binding.heap, *binding.intern_table, binding.vm);
        },
        is_torch_primitive_name,
        g_torch_primitive_batches);
}

int register_nng_bound_symbols(const EtaNativeApiV1* api,
                               const eta::native::SidecarRuntimeBindingV1& binding) {
    eta::nng::ProcessManager* process_manager = nullptr;
    if (binding.actor_process_manager != nullptr) {
        process_manager = static_cast<eta::nng::ProcessManager*>(
            binding.actor_process_manager->native_handle());
    } else {
        process_manager = static_cast<eta::nng::ProcessManager*>(binding.process_manager);
    }
    const std::string etai_path = binding.etai_path ? *binding.etai_path : std::string{};
    const std::string module_search_path =
        binding.module_search_path ? *binding.module_search_path : std::string{};

    return register_bound_symbol_set(
        api,
        [&](eta::runtime::BuiltinEnvironment& env) {
            eta::nng::register_nng_primitives(
                env,
                *binding.heap,
                *binding.intern_table,
                process_manager,
                etai_path,
                binding.mailbox_value,
                module_search_path,
                {},
                binding.function_registry,
                binding.vm_globals);
        },
        is_nng_primitive_name,
        g_nng_primitive_batches);
}

[[nodiscard]] bool native_object_api_available(const EtaNativeApiV1* api) {
    if (api == nullptr || api->runtime_context == nullptr) return false;
    const bool has_alloc = ETA_NATIVE_API_V1_HAS_FIELD(api, alloc_native_object)
        && api->alloc_native_object != nullptr;
    const bool has_get = ETA_NATIVE_API_V1_HAS_FIELD(api, get_native_object)
        && api->get_native_object != nullptr;
    return has_alloc && has_get;
}

int require_native_object_api(const EtaNativeApiV1* api) {
    if (native_object_api_available(api)) return ETA_NATIVE_STATUS_OK;
    if (api != nullptr && api->report_error != nullptr) {
        api->report_error(api->user_data, "native-object-api-unavailable");
    }
    return ETA_NATIVE_STATUS_ERROR;
}

int verify_native_object_roundtrip(const EtaNativeApiV1* api) {
    if (require_native_object_api(api) != ETA_NATIVE_STATUS_OK) {
        return ETA_NATIVE_STATUS_ERROR;
    }

    auto* payload = new (std::nothrow) NativeRoundtripPayload{};
    if (payload == nullptr) {
        if (api->report_error != nullptr) {
            api->report_error(api->user_data, "native-roundtrip-payload-allocation-failed");
        }
        return ETA_NATIVE_STATUS_ERROR;
    }
    payload->cookie = 0x4e3032u;

    std::uint64_t boxed_value = 0;
    const int alloc_status = api->alloc_native_object(
        api->runtime_context,
        &kNativeRoundtripVTable,
        payload,
        &boxed_value);
    if (alloc_status != ETA_NATIVE_STATUS_OK) {
        delete payload;
        if (api->report_error != nullptr) {
            api->report_error(api->user_data, "native-roundtrip-alloc-failed");
        }
        return ETA_NATIVE_STATUS_ERROR;
    }

    auto* resolved = static_cast<NativeRoundtripPayload*>(
        api->get_native_object(api->runtime_context, boxed_value, &kNativeRoundtripVTable));
    if (resolved != payload || resolved->cookie != 0x4e3032u) {
        if (api->report_error != nullptr) {
            api->report_error(api->user_data, "native-roundtrip-get-failed");
        }
        return ETA_NATIVE_STATUS_ERROR;
    }

    const void* mismatch = api->get_native_object(
        api->runtime_context,
        boxed_value,
        &kNativeRoundtripMismatchVTable);
    if (mismatch != nullptr) {
        if (api->report_error != nullptr) {
            api->report_error(api->user_data, "native-roundtrip-vtable-mismatch");
        }
        return ETA_NATIVE_STATUS_ERROR;
    }

    return ETA_NATIVE_STATUS_OK;
}

int verify_native_object_trace_alloc(const EtaNativeApiV1* api) {
    if (require_native_object_api(api) != ETA_NATIVE_STATUS_OK) {
        return ETA_NATIVE_STATUS_ERROR;
    }

    auto* payload = new (std::nothrow) NativeTracePayload{};
    if (payload == nullptr) {
        if (api->report_error != nullptr) {
            api->report_error(api->user_data, "native-trace-payload-allocation-failed");
        }
        return ETA_NATIVE_STATUS_ERROR;
    }
    payload->traced_value = 0u;

    std::uint64_t boxed_value = 0;
    const int alloc_status = api->alloc_native_object(
        api->runtime_context,
        &kNativeTraceVTable,
        payload,
        &boxed_value);
    if (alloc_status != ETA_NATIVE_STATUS_OK) {
        delete payload;
        if (api->report_error != nullptr) {
            api->report_error(api->user_data, "native-trace-alloc-failed");
        }
        return ETA_NATIVE_STATUS_ERROR;
    }

    if (boxed_value == 0u) {
        if (api->report_error != nullptr) {
            api->report_error(api->user_data, "native-trace-alloc-invalid-box");
        }
        return ETA_NATIVE_STATUS_ERROR;
    }

    return ETA_NATIVE_STATUS_OK;
}

} // namespace

extern "C" ETA_NATIVE_EXPORT int eta_register_extension_v1(const EtaNativeApiV1* api,
                                                           EtaExtensionInfoV1* out_info) {
    fill_extension_info(out_info, ETA_NATIVE_ABI_ID_V1, "eta.test.sidecar");
    return register_symbols(api);
}

extern "C" ETA_NATIVE_EXPORT int eta_register_extension_wrong_abi(const EtaNativeApiV1* api,
                                                                  EtaExtensionInfoV1* out_info) {
    fill_extension_info(out_info, "eta-native-v9", "eta.test.sidecar");
    return register_symbols(api);
}

extern "C" ETA_NATIVE_EXPORT int eta_register_extension_fail(const EtaNativeApiV1* api,
                                                             EtaExtensionInfoV1* out_info) {
    fill_extension_info(out_info, ETA_NATIVE_ABI_ID_V1, "eta.test.sidecar");
    if (api != nullptr && api->report_error != nullptr) {
        api->report_error(api->user_data, "intentional sidecar failure");
    }
    return ETA_NATIVE_STATUS_ERROR;
}

extern "C" ETA_NATIVE_EXPORT int eta_register_log_extension_v1(const EtaNativeApiV1* api,
                                                               EtaExtensionInfoV1* out_info) {
    fill_extension_info(out_info, ETA_NATIVE_ABI_ID_V1, "eta.log.sidecar");
    const auto* binding = runtime_binding_or_null(api);
    if (binding == nullptr) {
        return register_builtin_symbol_metadata(api, is_log_primitive_name);
    }
    return register_log_bound_symbols(api, *binding);
}

extern "C" ETA_NATIVE_EXPORT int eta_register_stats_extension_v1(const EtaNativeApiV1* api,
                                                                 EtaExtensionInfoV1* out_info) {
    fill_extension_info(out_info, ETA_NATIVE_ABI_ID_V1, "eta.stats.sidecar");
    const auto* binding = runtime_binding_or_null(api);
    if (binding == nullptr) {
        return register_builtin_symbol_metadata(api, is_stats_primitive_name);
    }
    return register_stats_bound_symbols(api, *binding);
}

extern "C" ETA_NATIVE_EXPORT int eta_register_torch_extension_v1(const EtaNativeApiV1* api,
                                                                 EtaExtensionInfoV1* out_info) {
    fill_extension_info(out_info, ETA_NATIVE_ABI_ID_V1, "eta.torch.sidecar");
    const auto* binding = runtime_binding_or_null(api);
    if (binding == nullptr) {
        return register_builtin_symbol_metadata(api, is_torch_primitive_name);
    }
    return register_torch_bound_symbols(api, *binding);
}

extern "C" ETA_NATIVE_EXPORT int eta_register_nng_extension_v1(const EtaNativeApiV1* api,
                                                               EtaExtensionInfoV1* out_info) {
    fill_extension_info(out_info, ETA_NATIVE_ABI_ID_V1, "eta.nng.sidecar");
    const auto* binding = runtime_binding_or_null(api);
    if (binding == nullptr) {
        return register_builtin_symbol_metadata(api, is_nng_primitive_name);
    }
    return register_nng_bound_symbols(api, *binding);
}

extern "C" ETA_NATIVE_EXPORT int eta_register_native_object_gate_extension_v1(
    const EtaNativeApiV1* api,
    EtaExtensionInfoV1* out_info) {
    fill_extension_info(out_info, ETA_NATIVE_ABI_ID_V1, "eta.native.gate.sidecar");
    return require_native_object_api(api);
}

extern "C" ETA_NATIVE_EXPORT int eta_register_native_object_gate_extension_legacy_runtime_v1(
    const EtaNativeApiV1* api,
    EtaExtensionInfoV1* out_info) {
    fill_extension_info(out_info, ETA_NATIVE_ABI_ID_V1, "eta.native.gate.legacy.sidecar");
    if (api == nullptr) return ETA_NATIVE_STATUS_ERROR;

    EtaNativeApiV1 legacy_api = *api;
    legacy_api.struct_size = static_cast<std::uint32_t>(
        offsetof(EtaNativeApiV1, report_error) + sizeof(legacy_api.report_error));
    legacy_api.alloc_native_object = nullptr;
    legacy_api.get_native_object = nullptr;
    return require_native_object_api(&legacy_api);
}

extern "C" ETA_NATIVE_EXPORT int eta_register_native_object_roundtrip_extension_v1(
    const EtaNativeApiV1* api,
    EtaExtensionInfoV1* out_info) {
    fill_extension_info(out_info, ETA_NATIVE_ABI_ID_V1, "eta.native.roundtrip.sidecar");
    return verify_native_object_roundtrip(api);
}

extern "C" ETA_NATIVE_EXPORT int eta_register_native_object_trace_extension_v1(
    const EtaNativeApiV1* api,
    EtaExtensionInfoV1* out_info) {
    fill_extension_info(out_info, ETA_NATIVE_ABI_ID_V1, "eta.native.trace.sidecar");
    return verify_native_object_trace_alloc(api);
}
