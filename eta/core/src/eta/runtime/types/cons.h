#pragma once

#include <eta/runtime/nanbox.h>

namespace eta::runtime::types {
    using namespace eta::runtime::nanbox;

    struct Cons {
        LispVal car {};
        LispVal cdr {};
    };

}