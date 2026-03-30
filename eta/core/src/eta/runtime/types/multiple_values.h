#pragma once

#include <vector>
#include <eta/runtime/nanbox.h>

namespace eta::runtime::types {
    using namespace eta::runtime::nanbox;

    /**
     * @brief Represents multiple return values from a (values ...) expression.
     *
     * In Scheme, (values v1 v2 ...) returns multiple values that can be
     * consumed by call-with-values. This struct wraps those values.
     */
    struct MultipleValues {
        std::vector<LispVal> vals {};
    };

}

