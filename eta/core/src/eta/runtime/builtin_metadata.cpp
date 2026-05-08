#include "eta/runtime/builtin_metadata.h"

#include <vector>

#include "eta/runtime/builtin_env.h"
#include "eta/runtime/builtin_names.h"

namespace eta::runtime {
namespace {

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

[[nodiscard]] std::string category_for_builtin(std::string_view name) {
    if (name.starts_with("torch/")
        || name.starts_with("nn/")
        || name.starts_with("optim/")) {
        return "Torch";
    }
    if (name.starts_with("%clp-")
        || name == "register-prop-attr!"
        || name == "%clp-prop-queue-size") {
        return "CLP";
    }
    if (name.starts_with("%stats-")) return "Stats";
    if (name.starts_with("%log-")) return "Log";
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
    if (is_nng_builtin(name)) return "NNG/message-passing primitive.";
    return "Builtin primitive.";
}

[[nodiscard]] BuiltinMetadata make_builtin_metadata(const BuiltinSpec& spec) {
    BuiltinMetadata metadata;
    metadata.name = spec.name;
    metadata.arity = spec.arity;
    metadata.has_rest = spec.has_rest;
    metadata.category = category_for_builtin(spec.name);
    metadata.signature = signature_override(spec.name);
    metadata.summary = summary_for_builtin(spec.name);
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
        BuiltinEnvironment env;
        register_builtin_names_legacy(env);
        std::vector<BuiltinMetadata> out;
        out.reserve(env.specs().size());
        for (const auto& spec : env.specs()) {
            out.push_back(make_builtin_metadata(spec));
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
