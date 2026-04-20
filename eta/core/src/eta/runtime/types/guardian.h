#pragma once

#include <deque>

#include <eta/runtime/nanbox.h>

namespace eta::runtime::types {
    using namespace eta::runtime::nanbox;

    /**
     * @brief Weak guardian object with a queue of ready-to-collect values.
     *
     * The queue is populated by GC when a tracked object becomes unreachable
     * while the guardian itself is still reachable.
     */
    struct Guardian {
        std::deque<LispVal> ready_queue{};
    };
}

