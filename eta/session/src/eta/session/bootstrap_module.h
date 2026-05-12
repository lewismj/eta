#pragma once

#include <string_view>

namespace eta::session {

/**
 * @brief REPL warning emitted when no module path or bundled stdlib is available.
 */
[[nodiscard]] inline constexpr std::string_view repl_missing_module_path_warning() noexcept {
    return "warning: ETA_MODULE_PATH is not set and no bundled stdlib found next to the executable.\n"
           "         Use --path or set ETA_MODULE_PATH to a directory containing stdlib modules.\n";
}

/**
 * @brief DAP console text emitted when no module-search directories are active.
 */
[[nodiscard]] inline constexpr std::string_view dap_empty_module_path_warning() noexcept {
    return "  (none  -  stdlib modules may be unavailable; set eta.lsp.modulePath in VS Code settings)\n";
}

} // namespace eta::session
