#pragma once

#include <vector>
#include <eta/runtime/nanbox.h>

namespace eta::runtime::types {
    using namespace eta::runtime::nanbox;

    struct Vector {
        std::vector<LispVal> elements {};
    };

}
