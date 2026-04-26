#pragma once

#include <string>

#include "eta/runtime/nanbox.h"

namespace eta::session {

/**
 * @brief High-level display tags for front-end rendering decisions.
 */
enum class DisplayTag {
    Text,
    Tensor,
    FactTable,
    Html,
    Markdown,
    Latex,
    Svg,
    Png,
    VegaLite,
    Error,
};

/**
 * @brief Structured display value returned by Driver evaluation helpers.
 */
struct DisplayValue {
    DisplayTag tag{DisplayTag::Text};
    std::string text;
    runtime::nanbox::LispVal value{runtime::nanbox::Nil};
};

} ///< namespace eta::session
