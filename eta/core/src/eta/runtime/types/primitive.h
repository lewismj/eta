#pragma once

#include <functional>
#include <expected>
#include <eta/runtime/nanbox.h>
#include <eta/runtime/error.h>

namespace eta::runtime::types {
    using namespace eta::runtime::nanbox;
    using namespace eta::runtime::error;

    using PrimitiveFunc = std::function<std::expected<LispVal, RuntimeError>(const std::vector<LispVal>&)>;

    struct Primitive {
        PrimitiveFunc func {};
        uint32_t arity {};
        bool has_rest {};
        std::vector<LispVal> gc_roots {};  ///< LispVals captured by func that GC must trace
    };

}
