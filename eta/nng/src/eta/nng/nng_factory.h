#pragma once

/// @file nng_factory.h
/// @brief Factory functions for creating NngSocket heap objects.

#include <expected>

#include <eta/runtime/nanbox.h>
#include <eta/runtime/error.h>
#include <eta/runtime/memory/heap.h>

#include "nng_socket_ptr.h"

namespace eta::nng::factory {

using namespace eta::runtime::nanbox;
using namespace eta::runtime::error;
using namespace eta::runtime::memory::heap;

/// Allocate an NngSocketPtr on the Eta heap and return the boxed LispVal.
inline std::expected<LispVal, RuntimeError>
make_nng_socket(Heap& heap, NngSocketPtr sp) {
    auto id = heap.allocate<NngSocketPtr, ObjectKind::NngSocket>(std::move(sp));
    if (id.has_value()) return ops::box(Tag::HeapObject, *id);
    return std::unexpected(id.error());
}

} // namespace eta::nng::factory

