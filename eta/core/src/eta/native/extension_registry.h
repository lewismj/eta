#pragma once

#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace eta::native {

/**
 * @brief One primitive descriptor exported by a native extension.
 */
struct ExtensionSymbolDescriptor {
    std::string name;
    std::uint32_t arity{0};
    bool has_rest{false};
    void* callable{nullptr};
};

/**
 * @brief Registered metadata for a loaded native extension.
 */
struct ExtensionMetadata {
    std::string id;
    std::string version;
    std::string abi;
    std::vector<ExtensionSymbolDescriptor> symbols;
};

/**
 * @brief Error raised while mutating extension registry state.
 */
struct ExtensionRegistryError {
    enum class Code {
        InvalidExtensionId,
        UnknownExtension,
        InvalidSymbolName,
        DuplicateExtensionId,
        DuplicateSymbolName,
    };

    Code code{Code::InvalidExtensionId};
    std::string message;
};

using ExtensionRegistryResult = std::expected<void, ExtensionRegistryError>;

/**
 * @brief Runtime registry for loaded extension metadata and symbol ownership.
 */
class ExtensionRegistry {
public:
    /**
     * @brief Insert a new extension row.
     */
    ExtensionRegistryResult register_extension(std::string id,
                                               std::string version,
                                               std::string abi);

    /**
     * @brief Register one primitive descriptor under an existing extension id.
     */
    ExtensionRegistryResult register_symbol(std::string_view extension_id,
                                            ExtensionSymbolDescriptor symbol);

    /**
     * @brief Lookup one extension by id.
     */
    [[nodiscard]] const ExtensionMetadata* find_extension(std::string_view id) const;

    /**
     * @brief Return the owning extension id for a symbol, when present.
     */
    [[nodiscard]] std::optional<std::string_view> symbol_owner(
        std::string_view symbol_name) const;

    /**
     * @brief Return all registered extensions in insertion order.
     */
    [[nodiscard]] const std::vector<ExtensionMetadata>& extensions() const noexcept;

private:
    std::vector<ExtensionMetadata> extensions_;
    std::unordered_map<std::string, std::size_t> index_by_extension_id_;
    std::unordered_map<std::string, std::string> symbol_owner_by_name_;
};

} // namespace eta::native
