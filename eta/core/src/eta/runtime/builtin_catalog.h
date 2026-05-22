#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>

namespace eta::runtime {

class BuiltinEnvironment;

/**
 * @brief Canonical catalog entry for one configured builtin primitive.
 */
struct BuiltinCatalogEntry {
    std::string name;
    uint32_t arity{0};
    bool has_rest{false};
    bool is_blocking{false};
    std::string owner;
    std::optional<std::string> category;
    std::optional<std::string> signature;
    std::optional<std::string> summary;
};

/**
 * @brief Enumerate canonical builtin catalog entries in registration order.
 */
[[nodiscard]] std::span<const BuiltinCatalogEntry> builtin_catalog();

/**
 * @brief Return whether @p name is classified as potentially blocking.
 */
[[nodiscard]] bool builtin_is_blocking(std::string_view name);

/**
 * @brief Register analysis-only builtin slots from the canonical catalog.
 */
void register_builtin_specs(BuiltinEnvironment& env);

} // namespace eta::runtime
