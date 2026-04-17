#pragma once

#include <bit>
#include <iomanip>
#include <sstream>
#include <string>

#include "eta/runtime/nanbox.h"
#include "eta/runtime/memory/heap.h"
#include "eta/runtime/memory/intern_table.h"
#include "eta/runtime/numeric_value.h"
#include "eta/runtime/string_view.h"
#include "eta/runtime/types/types.h"
#include "eta/runtime/port.h"

namespace eta::runtime {

using namespace eta::runtime::nanbox;
using namespace eta::runtime::memory::heap;
using namespace eta::runtime::memory::intern;

/**
 * @brief Formatting mode for Scheme values.
 *
 * Display: human-readable (strings without quotes, chars as raw characters)
 * Write:   machine-readable (strings quoted, chars with #\ prefix, etc.)
 */
enum class FormatMode { Display, Write };

/**
 * @brief Format a LispVal to its string representation.
 *
 * Handles all value types: nil, booleans, characters, fixnums, flonums,
 * strings, symbols, pairs/lists, vectors, bytevectors, closures, primitives,
 * continuations, ports, and other heap objects.
 */
std::string format_value(LispVal v, FormatMode mode, Heap& heap, InternTable& intern_table);

} ///< namespace eta::runtime

