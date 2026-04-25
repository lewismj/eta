#pragma once

#include <string>

namespace eta::jupyter {

/**
 * @brief Minimal display payload used by the scaffold executable.
 */
struct DisplayValue {
    std::string text_repr; ///< Text fallback always available to callers.
};

/**
 * @brief Create a plain-text display payload.
 *
 * @param text_repr Text to expose to front-ends.
 * @return Display payload with the provided text.
 */
DisplayValue make_plain_display(std::string text_repr);

} // namespace eta::jupyter
