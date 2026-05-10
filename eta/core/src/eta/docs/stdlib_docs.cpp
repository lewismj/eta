#include "eta/docs/stdlib_docs.h"

#include <array>
#include <unordered_set>

namespace eta::docs {

namespace generated {
#include "eta/docs/generated/stdlib_docs.inc"
} // namespace generated

namespace {

[[nodiscard]] DocEntry stdlib_doc_to_entry(const StdlibDocMetadata& metadata) {
    DocEntry entry;
    entry.name = metadata.name;
    entry.signature = metadata.signature;
    entry.summary = metadata.summary;
    entry.kind = DocKind::StdlibBinding;
    entry.details = metadata.details;
    entry.category = metadata.category;
    entry.module = metadata.module;
    return entry;
}

} // namespace

std::span<const StdlibDocMetadata> stdlib_doc_registry() {
    return std::span<const StdlibDocMetadata>(
        generated::kStdlibDocs.data(),
        generated::kStdlibDocs.size());
}

std::optional<DocEntry> lookup_stdlib_doc(std::string_view symbol) {
    if (symbol.empty()) return std::nullopt;

    for (const auto& metadata : generated::kStdlibDocs) {
        if (metadata.name == symbol || metadata.qualified_name == symbol) {
            return stdlib_doc_to_entry(metadata);
        }
    }
    return std::nullopt;
}

std::vector<std::string> duplicate_stdlib_doc_symbols() {
    std::unordered_set<std::string> seen;
    std::vector<std::string> duplicates;

    auto record_key = [&seen, &duplicates](std::string_view key) {
        if (key.empty()) return;
        if (seen.insert(std::string(key)).second) return;
        duplicates.push_back(std::string(key));
    };

    for (const auto& metadata : generated::kStdlibDocs) {
        record_key(metadata.qualified_name);
    }

    return duplicates;
}

} // namespace eta::docs
