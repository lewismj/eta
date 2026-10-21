#include <cstddef>
#include <cstdint>
#include <sstream>
#include <string>

#include "eta/runtime/memory/heap.h"
#include "eta/runtime/memory/intern_table.h"
#include "eta/runtime/vm/bytecode_serializer.h"

/**
 * Fuzz target for the .etac deserializer (BytecodeSerializer::deserialize).
 * A .etac file is loaded from disk or a package cache, so every field is
 * untrusted. Errors must come back as SerializerError, never as a crash,
 * an uncaught exception or an unbounded allocation.
 */

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    using namespace eta::runtime;

    memory::heap::Heap heap{4 * 1024 * 1024};
    memory::intern::InternTable intern;
    vm::BytecodeSerializer serializer(heap, intern);

    std::istringstream in(std::string(reinterpret_cast<const char*>(data), size), std::ios::binary);
    (void)serializer.deserialize(in);
    return 0;
}
