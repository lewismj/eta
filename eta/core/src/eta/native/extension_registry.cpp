#include "eta/native/extension_registry.h"

#include <utility>

namespace eta::native {

ExtensionRegistryResult ExtensionRegistry::register_extension(std::string id,
                                                              std::string version,
                                                              std::string abi) {
    if (id.empty()) {
        return std::unexpected(ExtensionRegistryError{
            ExtensionRegistryError::Code::InvalidExtensionId,
            "extension id must not be empty",
        });
    }
    if (index_by_extension_id_.contains(id)) {
        return std::unexpected(ExtensionRegistryError{
            ExtensionRegistryError::Code::DuplicateExtensionId,
            "duplicate extension id '" + id + "'",
        });
    }

    ExtensionMetadata metadata;
    metadata.id = std::move(id);
    metadata.version = std::move(version);
    metadata.abi = std::move(abi);
    const std::size_t index = extensions_.size();
    extensions_.push_back(std::move(metadata));
    index_by_extension_id_.emplace(extensions_[index].id, index);
    return {};
}

ExtensionRegistryResult ExtensionRegistry::register_symbol(
    std::string_view extension_id,
    ExtensionSymbolDescriptor symbol) {
    if (symbol.name.empty()) {
        return std::unexpected(ExtensionRegistryError{
            ExtensionRegistryError::Code::InvalidSymbolName,
            "extension symbol name must not be empty",
        });
    }

    const auto extension_it = index_by_extension_id_.find(std::string(extension_id));
    if (extension_it == index_by_extension_id_.end()) {
        return std::unexpected(ExtensionRegistryError{
            ExtensionRegistryError::Code::UnknownExtension,
            "unknown extension id '" + std::string(extension_id) + "'",
        });
    }

    if (const auto owner_it = symbol_owner_by_name_.find(symbol.name);
        owner_it != symbol_owner_by_name_.end()) {
        return std::unexpected(ExtensionRegistryError{
            ExtensionRegistryError::Code::DuplicateSymbolName,
            "duplicate extension symbol '" + symbol.name + "' already owned by '"
                + owner_it->second + "'",
        });
    }

    auto& extension = extensions_[extension_it->second];
    extension.symbols.push_back(std::move(symbol));
    symbol_owner_by_name_.emplace(extension.symbols.back().name, extension.id);
    return {};
}

const ExtensionMetadata* ExtensionRegistry::find_extension(std::string_view id) const {
    const auto it = index_by_extension_id_.find(std::string(id));
    if (it == index_by_extension_id_.end()) return nullptr;
    return &extensions_[it->second];
}

std::optional<std::string_view> ExtensionRegistry::symbol_owner(
    std::string_view symbol_name) const {
    const auto it = symbol_owner_by_name_.find(std::string(symbol_name));
    if (it == symbol_owner_by_name_.end()) return std::nullopt;
    return std::string_view(it->second);
}

const std::vector<ExtensionMetadata>& ExtensionRegistry::extensions() const noexcept {
    return extensions_;
}

} // namespace eta::native
