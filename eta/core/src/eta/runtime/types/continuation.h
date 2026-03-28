#pragma once

#include <vector>
#include <eta/runtime/nanbox.h>
#include <eta/runtime/vm/vm.h>

namespace eta::runtime::types {
    using namespace eta::runtime::nanbox;
    using namespace eta::runtime::vm;

    struct Continuation {
        std::vector<LispVal> stack {};
        std::vector<Frame> frames {};
    };

}
