#pragma once

#include <vector>
#include <eta/runtime/nanbox.h>
#include <eta/runtime/vm/bytecode.h>

namespace eta::runtime::types {
    using namespace eta::runtime::nanbox;
    using namespace eta::runtime::vm;

    struct Closure {
        const BytecodeFunction* func {};
        std::vector<LispVal> upvals {};
    };

}
