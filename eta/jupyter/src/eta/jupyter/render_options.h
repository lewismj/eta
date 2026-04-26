#pragma once

#include <cstddef>
#include <string>

namespace eta::jupyter::display {

/**
 * @brief Runtime rendering controls loaded from kernel configuration.
 */
struct RenderOptions {
    std::size_t table_max_rows{1000};
    std::size_t tensor_preview{8};
    std::string plot_theme{"light"};
};

} // namespace eta::jupyter::display

