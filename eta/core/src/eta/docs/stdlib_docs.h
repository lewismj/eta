#pragma once

#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "eta/docs/doc_entry.h"

namespace eta::docs {

/**
 * @brief Metadata for one stdlib binding extracted from ;;@doc blocks.
 */
struct StdlibDocMetadata {
    std::string_view name;
    std::string_view qualified_name;
    std::string_view signature;
    std::string_view summary;
    std::string_view details;
    std::string_view category;
    std::string_view module;
    std::string_view alias_of;
    std::string_view since;
    std::string_view deprecated;
};

/**
 * @brief Enumerate the generated stdlib doc registry.
 */
[[nodiscard]] std::span<const StdlibDocMetadata> stdlib_doc_registry();

/**
 * @brief Lookup stdlib docs by short or module-qualified symbol name.
 */
[[nodiscard]] std::optional<DocEntry> lookup_stdlib_doc(std::string_view symbol);

/**
 * @brief Report duplicate symbol keys in the generated stdlib doc registry.
 *
 * Keys include both short names and module-qualified names.
 */
[[nodiscard]] std::vector<std::string> duplicate_stdlib_doc_symbols();

} // namespace eta::docs
