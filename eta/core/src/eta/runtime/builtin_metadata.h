#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace eta::runtime {

/**
 * @brief Canonical editor-facing metadata for one configured builtin.
 */
struct BuiltinMetadata {
    std::string name;
    uint32_t arity{0};
    bool has_rest{false};
    std::string category;
    std::string signature;
    std::string summary;
};

/**
 * @brief Enumerate metadata for all configured builtins in registration order.
 */
[[nodiscard]] std::span<const BuiltinMetadata> builtin_metadata();

/**
 * @brief Return the native sidecar package that provides @p name, if any.
 */
[[nodiscard]] std::optional<std::string_view> builtin_native_sidecar_package(
    std::string_view name);

/**
 * @brief Lookup builtin metadata by exact symbol name.
 */
[[nodiscard]] std::optional<BuiltinMetadata> lookup_builtin_metadata(std::string_view name);

/**
 * @brief Report builtins that are missing category/signature/summary metadata.
 *
 * @param allowed_missing Names intentionally allowed to be incomplete.
 * @return Builtin names with missing documentation fields.
 */
[[nodiscard]] std::vector<std::string> missing_builtin_docs(
    std::span<const std::string_view> allowed_missing = {});

/**
 * @brief Build a fallback signature label for a builtin entry.
 */
[[nodiscard]] std::string format_builtin_signature(const BuiltinMetadata& builtin);

/**
 * @brief Build a fallback summary text for a builtin entry.
 */
[[nodiscard]] std::string format_builtin_summary(const BuiltinMetadata& builtin);

} // namespace eta::runtime
