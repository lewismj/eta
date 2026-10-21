#pragma once

/// Helpers shared by the lsp_server*.cpp request families.

#include <filesystem>
#include <system_error>

namespace eta::lsp {


[[nodiscard]] inline std::filesystem::path canonicalize_path(const std::filesystem::path& path) {
    std::error_code ec;
    const auto canonical = std::filesystem::weakly_canonical(path, ec);
    if (!ec) return canonical;
    return path.lexically_normal();
}

} ///< namespace eta::lsp
