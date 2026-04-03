#pragma once

#include <memory>
#include "eta/runtime/port.h"

namespace eta::runtime::types {

/**
 * @brief Wrapper for Port objects stored on the heap.
 */
struct PortObject {
    std::shared_ptr<Port> port;
};

}  // namespace eta::runtime::types

