#pragma once

#include <string>

namespace eta::session {

/**
 * @brief High-level display tags for front-end rendering decisions.
 */
enum class DisplayTag {
    Text,
    Tensor,
    FactTable,
    Error,
};

/**
 * @brief Structured display value returned by Driver evaluation helpers.
 */
struct DisplayValue {
    DisplayTag tag{DisplayTag::Text};
    std::string text;
};

} ///< namespace eta::session
