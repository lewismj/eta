#pragma once

#include <string_view>

namespace eta::docs {

/**
 * @brief Classification for editor-facing language documentation entries.
 */
enum class DocKind {
    SpecialForm,
    Macro,
    Builtin,
    StdlibBinding,
};

/**
 * @brief Shared documentation payload for hover/completion/signature features.
 */
struct DocEntry {
    std::string_view name;
    std::string_view signature;
    std::string_view summary;
    DocKind kind{DocKind::SpecialForm};
    std::string_view details{};
    std::string_view category{};
    std::string_view module{};
};

} // namespace eta::docs
