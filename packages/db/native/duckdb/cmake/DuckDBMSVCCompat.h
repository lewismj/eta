#pragma once

// Let the STL derive _ITERATOR_DEBUG_LEVEL before hiding its legacy macro.
// This preserves iterator debugging and the STL ABI in both Debug and Release.
#include <yvals.h>

// DuckDB's bundled fmt uses #ifdef _SECURE_SCL to select removed stdext
// iterators. An undefined macro selects its portable raw-pointer implementation
// on both older and newer MSVC versions.
#undef _SECURE_SCL
