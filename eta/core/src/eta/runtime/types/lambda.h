#pragma once

#include <vector>
#include <eta/runtime/nanbox.h>

namespace eta::runtime::types {
    using namespace eta::runtime::nanbox;

    struct InterpretedProcedure {
        std::vector<LispVal> formals {};
        LispVal body {};
        std::vector<LispVal> up_values {};
    };

}