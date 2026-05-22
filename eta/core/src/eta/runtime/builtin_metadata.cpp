#include "eta/runtime/builtin_metadata.h"

#include <vector>

#include "eta/runtime/builtin_catalog.h"

namespace eta::runtime {
namespace {

[[nodiscard]] bool is_torch_builtin(std::string_view name) {
    return name.starts_with("torch/")
        || name.starts_with("nn/")
        || name.starts_with("optim/");
}

[[nodiscard]] bool is_stats_builtin(std::string_view name) {
    return name.starts_with("%stats-");
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

[[nodiscard]] bool is_actor_builtin(std::string_view name) {
    return name.starts_with("%actor-");
}

[[nodiscard]] std::optional<std::string_view> native_sidecar_from_owner(
    std::string_view owner) {
    constexpr std::string_view prefix = "sidecar:";
    if (!owner.starts_with(prefix)) return std::nullopt;
    return owner.substr(prefix.size());
}

[[nodiscard]] std::optional<std::string_view> catalog_sidecar_package_for_builtin(
    std::string_view name) {
    for (const auto& entry : builtin_catalog()) {
        if (entry.name != name) continue;
        return native_sidecar_from_owner(entry.owner);
    }
    return std::nullopt;
}

[[nodiscard]] std::string category_for_builtin(std::string_view name) {
    if (is_torch_builtin(name)) {
        return "Torch";
    }
    if (name.starts_with("%clp-")
        || name == "register-prop-attr!"
        || name == "%clp-prop-queue-size") {
        return "CLP";
    }
    if (is_stats_builtin(name)) return "Stats";
    if (is_log_builtin(name)) return "Log";
    if (name.starts_with("%regex-")) return "Regex";
    if (name.starts_with("%json-")) return "JSON";
    if (name.starts_with("%time-")) return "Time";
    if (name.starts_with("%process-")) return "Process";
    if (name.starts_with("%csv-")
        || name.starts_with("%fact-table")
        || name == "%make-fact-table"
        || name == "fact-table?") {
        return "Data";
    }
    if (name.starts_with("%atom-")) return "Atom";
    if (name.starts_with("dual")
        || name.starts_with("tape-")
        || name == "make-dual"
        || name == "set-aad-nondiff-policy!"
        || name == "aad-nondiff-policy") {
        return "AD";
    }
    if (name.starts_with("hash-")
        || name.starts_with("make-hash")
        || name.starts_with("list->hash")
        || name == "hash") {
        return "Hash";
    }
    if (name.starts_with("vector")
        || name == "make-vector") {
        return "Vector";
    }
    if (name == "+" || name == "-" || name == "*" || name == "/") return "Arithmetic";
    if (name == "=" || name == "<" || name == ">" || name == "<=" || name == ">=") return "Comparison";
    if (name == "map" || name == "for-each" || name == "apply") return "Higher-order";
    if (name.starts_with("string")
        || name == "symbol->string"
        || name == "string->symbol"
        || name == "number->string"
        || name == "string->number") {
        return "String";
    }
    if (name.find("port") != std::string_view::npos
        || name.starts_with("open-")
        || name.starts_with("close-")
        || name == "read-char"
        || name == "write-char"
        || name == "read-u8"
        || name == "write-u8") {
        return "Port";
    }
    if (is_actor_builtin(name)) return "Actor";
    if (is_nng_builtin(name)) return "NNG";
    return "Builtin";
}

[[nodiscard]] std::string signature_override(std::string_view name) {
    if (name == "+") return "(+ z ...)";
    if (name == "-") return "(- z1 z2 ...)";
    if (name == "*") return "(* z ...)";
    if (name == "/") return "(/ z1 z2 ...)";
    if (name == "map") return "(map proc list ...)";
    if (name == "for-each") return "(for-each proc list ...)";
    if (name == "apply") return "(apply proc arg ... args)";
    if (name == "display") return "(display obj [port])";
    if (name == "write") return "(write obj [port])";
    if (name == "newline") return "(newline [port])";
    if (name == "error") return "(error message irritant ...)";
    if (name == "nng-socket") return "(nng-socket type-symbol)";
    if (name == "send!") return "(send! sock value [flag])";
    if (name == "recv!") return "(recv! sock [flag])";
    if (name == "%actor-self") return "(%actor-self)";
    if (name == "%actor-pid?") return "(%actor-pid? value)";
    if (name == "%actor-alive?") return "(%actor-alive? pid)";
    if (name == "%actor-spawn") return "(%actor-spawn thunk)";
    if (name == "%actor-send") return "(%actor-send pid payload)";
    if (name == "%actor-send-checked") return "(%actor-send-checked pid payload)";
    if (name == "%actor-receive") return "(%actor-receive matcher timeout)";
    if (name == "%actor-mailbox-len") return "(%actor-mailbox-len)";
    if (name == "%actor-process-info") return "(%actor-process-info pid [key])";
    if (name == "%actor-trap-exit!") return "(%actor-trap-exit! enabled?)";
    if (name == "%actor-link") return "(%actor-link pid)";
    if (name == "%actor-unlink") return "(%actor-unlink pid)";
    if (name == "%actor-monitor") return "(%actor-monitor pid)";
    if (name == "%actor-demonitor") return "(%actor-demonitor ref [flush?])";
    if (name == "%actor-exit") return "(%actor-exit pid reason)";
    if (name == "%actor-kill") return "(%actor-kill pid)";
    if (name == "%actor-register") return "(%actor-register name pid)";
    if (name == "%actor-unregister") return "(%actor-unregister name)";
    if (name == "%actor-whereis") return "(%actor-whereis name)";
    if (name == "%actor-registered") return "(%actor-registered)";
    if (name == "%actor-node-name") return "(%actor-node-name)";
    if (name == "%actor-monitor-node") return "(%actor-monitor-node node-name)";
    if (name == "%actor-node-listen") return "(%actor-node-listen endpoint [key value] ...)";
    if (name == "%actor-node-connect") return "(%actor-node-connect endpoint [key value] ...)";
    if (name == "%actor-nodes") return "(%actor-nodes)";
    if (name == "%actor-disconnect-node") return "(%actor-disconnect-node node-name)";
    return {};
}

[[nodiscard]] std::string summary_for_builtin(std::string_view name) {
    if (name == "map") return "Apply a procedure element-wise over lists.";
    if (name == "for-each") return "Apply a procedure for side effects over lists.";
    if (name == "apply") return "Apply a procedure to an explicit tail argument list.";
    if (name == "display") return "Write a human-readable representation to a port.";
    if (name == "write") return "Write a machine-readable representation to a port.";
    if (name == "newline") return "Write a newline to a port.";
    if (name == "error") return "Raise a runtime error.";
    if (is_actor_builtin(name)) return "Local actor runtime primitive.";
    if (is_nng_builtin(name)) return "NNG/message-passing primitive.";
    return "Builtin primitive.";
}

[[nodiscard]] BuiltinMetadata make_builtin_metadata(const BuiltinCatalogEntry& entry) {
    BuiltinMetadata metadata;
    metadata.name = entry.name;
    metadata.arity = entry.arity;
    metadata.has_rest = entry.has_rest;
    metadata.is_blocking = entry.is_blocking;
    metadata.category = entry.category.value_or(category_for_builtin(entry.name));
    metadata.signature = entry.signature.value_or(signature_override(entry.name));
    metadata.summary = entry.summary.value_or(summary_for_builtin(entry.name));
    if (metadata.signature.empty()) {
        metadata.signature = format_builtin_signature(metadata);
    }
    if (metadata.summary.empty()) {
        metadata.summary = format_builtin_summary(metadata);
    }
    return metadata;
}

[[nodiscard]] const std::vector<BuiltinMetadata>& builtin_metadata_storage() {
    static const std::vector<BuiltinMetadata> metadata = [] {
        const auto catalog = builtin_catalog();
        std::vector<BuiltinMetadata> out;
        out.reserve(catalog.size());
        for (const auto& entry : catalog) {
            out.push_back(make_builtin_metadata(entry));
        }
        return out;
    }();
    return metadata;
}

} // namespace

std::span<const BuiltinMetadata> builtin_metadata() {
    const auto& metadata = builtin_metadata_storage();
    return std::span<const BuiltinMetadata>(metadata.data(), metadata.size());
}

std::optional<std::string_view> builtin_native_sidecar_package(std::string_view name) {
    return catalog_sidecar_package_for_builtin(name);
}

std::optional<BuiltinMetadata> lookup_builtin_metadata(std::string_view name) {
    for (const auto& metadata : builtin_metadata_storage()) {
        if (metadata.name == name) {
            return metadata;
        }
    }
    return std::nullopt;
}

std::vector<std::string> missing_builtin_docs(
    std::span<const std::string_view> allowed_missing) {
    auto is_allowed_missing = [&allowed_missing](std::string_view name) {
        for (const auto allowed : allowed_missing) {
            if (allowed == name) return true;
        }
        return false;
    };

    std::vector<std::string> missing;
    for (const auto& builtin : builtin_metadata_storage()) {
        const bool has_all_docs = !builtin.category.empty()
            && !builtin.signature.empty()
            && !builtin.summary.empty();
        if (has_all_docs || is_allowed_missing(builtin.name)) continue;
        missing.push_back(builtin.name);
    }
    return missing;
}

std::string format_builtin_signature(const BuiltinMetadata& builtin) {
    if (!builtin.signature.empty()) return builtin.signature;

    std::string signature;
    signature.reserve(builtin.name.size() + 32);
    signature.push_back('(');
    signature += builtin.name;
    for (uint32_t i = 0; i < builtin.arity; ++i) {
        signature += " arg";
        signature += std::to_string(i + 1);
    }
    if (builtin.has_rest) {
        signature += " ...";
    }
    signature.push_back(')');
    return signature;
}

std::string format_builtin_summary(const BuiltinMetadata& builtin) {
    if (!builtin.summary.empty()) return builtin.summary;
    return "Builtin primitive.";
}

} // namespace eta::runtime
