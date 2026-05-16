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

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {

using PrimitiveFunc = eta::runtime::types::PrimitiveFunc;

struct RegisteredPrimitive {
    std::string name;
    std::uint32_t arity{0};
    std::uint8_t has_rest{0};
    std::unique_ptr<PrimitiveFunc> func;
};

using PrimitiveBatch = std::vector<RegisteredPrimitive>;

std::vector<std::unique_ptr<PrimitiveBatch>> g_log_primitive_batches;
std::vector<std::unique_ptr<PrimitiveBatch>> g_stats_primitive_batches;
std::vector<std::unique_ptr<PrimitiveBatch>> g_torch_primitive_batches;
std::vector<std::unique_ptr<PrimitiveBatch>> g_nng_primitive_batches;

void fill_extension_info(EtaExtensionInfoV1* out_info,
                         const char* abi,
                         const char* extension_id) {
    if (out_info == nullptr) return;
    out_info->struct_size = sizeof(EtaExtensionInfoV1);
    out_info->abi_id = abi;
    out_info->extension_id = extension_id;
    out_info->extension_version = "0.1.0";
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
            static_cast<std::uint8_t>(symbol.has_rest ? 1u : 0u),
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

} // namespace

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
