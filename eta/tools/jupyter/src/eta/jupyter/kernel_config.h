#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include "eta/jupyter/render_options.h"

namespace eta::jupyter {

/**
 * @brief Runtime kernel configuration loaded from kernel.toml.
 */
struct KernelConfig {
    std::vector<std::string> autoimport_modules{"std.io"};
    display::RenderOptions display{};
    int hard_kill_after_seconds{30};
};

/**
 * @brief Result of kernel configuration loading.
 */
struct KernelConfigLoadResult {
    KernelConfig config{};
    std::filesystem::path path{};
    std::vector<std::string> warnings{};
};

/**
 * @brief Load kernel configuration from user or environment paths.
 *
 * Search order:
 * 1) ETA_KERNEL_CONFIG (explicit file path)
 * 2) XDG_CONFIG_HOME/eta/kernel.toml
 * 3) HOME/.config/eta/kernel.toml
 * 4) USERPROFILE/.config/eta/kernel.toml
 * 5) APPDATA/eta/kernel.toml
 *
 * Missing files are treated as defaults (no warning).
 */
KernelConfigLoadResult load_kernel_config();

} // namespace eta::jupyter
