#include "eta/runtime/builtin_catalog.h"

#include <string_view>
#include <vector>

#include "eta/runtime/builtin_env.h"
#include "eta/runtime/builtin_specs_seed.h"

namespace eta::runtime {
namespace {

[[nodiscard]] bool is_torch_builtin(std::string_view name) {
    return name.starts_with("torch/")
        || name.starts_with("nn/")
        || name.starts_with("optim/");
}

[[nodiscard]] bool is_stats_sidecar_builtin(std::string_view name) {
    return name == "%stats-mean-vec"
        || name == "%stats-var-vec"
        || name == "%stats-cov-matrix"
        || name == "%stats-cor-matrix"
        || name == "%stats-quantile-vec"
        || name == "%stats-ols-multi";
}

[[nodiscard]] bool is_log_builtin(std::string_view name) {
    return name.starts_with("%log-");
}

[[nodiscard]] bool is_nng_builtin(std::string_view name) {
    return name.starts_with("nng-")
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

[[nodiscard]] bool is_blocking_builtin(std::string_view name) {
    return name == "%process-run"
        || name == "%process-wait"
        || name == "%time-sleep-ms"
        || name == "recv!"
        || name == "spawn-wait"
        || name == "thread-join";
}

[[nodiscard]] std::string owner_for_builtin(std::string_view name) {
    if (is_torch_builtin(name)) return "sidecar:eta-torch";
    if (is_stats_sidecar_builtin(name)) return "sidecar:eta-stats";
    if (is_log_builtin(name)) return "sidecar:eta-log";
    if (is_nng_builtin(name)) return "sidecar:eta-nng";
    return "core";
}

[[nodiscard]] const std::vector<BuiltinCatalogEntry>& builtin_catalog_storage() {
    static const std::vector<BuiltinCatalogEntry> catalog = [] {
        BuiltinEnvironment env;
        detail::register_builtin_specs_seed(env);

        std::vector<BuiltinCatalogEntry> out;
        out.reserve(env.specs().size());
        for (const auto& spec : env.specs()) {
            BuiltinCatalogEntry entry;
            entry.name = spec.name;
            entry.arity = spec.arity;
            entry.has_rest = spec.has_rest;
            entry.is_blocking = is_blocking_builtin(spec.name);
            entry.owner = owner_for_builtin(spec.name);
            out.push_back(std::move(entry));
        }
        return out;
    }();
    return catalog;
}

} // namespace

std::span<const BuiltinCatalogEntry> builtin_catalog() {
    const auto& catalog = builtin_catalog_storage();
    return std::span<const BuiltinCatalogEntry>(catalog.data(), catalog.size());
}

bool builtin_is_blocking(std::string_view name) {
    return is_blocking_builtin(name);
}

void register_builtin_specs(BuiltinEnvironment& env) {
    for (const auto& entry : builtin_catalog()) {
        env.register_builtin(entry.name, entry.arity, entry.has_rest, PrimitiveFunc{});
    }
}

} // namespace eta::runtime
