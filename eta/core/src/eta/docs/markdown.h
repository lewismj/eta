#pragma once

#include <string>

#include "eta/docs/doc_entry.h"
#include "eta/runtime/builtin_metadata.h"

namespace eta::docs {

/**
 * @brief Render generic language documentation as markdown text.
 */
inline std::string render_markdown(const DocEntry& entry) {
    std::string out;
    out.reserve(128);
    out += "**";
    out += std::string(entry.name);
    out += "**";
    if (!entry.summary.empty()) {
        out += "  -  ";
        out += std::string(entry.summary);
    }
    if (!entry.signature.empty()) {
        out += "\n\n`";
        out += std::string(entry.signature);
        out += "`";
    }
    if (!entry.details.empty()) {
        out += "\n\n";
        out += std::string(entry.details);
    }
    return out;
}

/**
 * @brief Render builtin documentation as markdown text.
 */
inline std::string render_builtin_markdown(const eta::runtime::BuiltinMetadata& builtin) {
    std::string out;
    out.reserve(128);
    out += "**";
    out += builtin.name;
    out += "**";
    out += "  -  ";
    if (!builtin.summary.empty()) {
        out += builtin.summary;
    } else {
        out += "Builtin primitive.";
    }

    const std::string signature = eta::runtime::format_builtin_signature(builtin);
    if (!signature.empty()) {
        out += "\n\n`";
        out += signature;
        out += "`";
    }
    return out;
}

} // namespace eta::docs
