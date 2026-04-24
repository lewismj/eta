#pragma once

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "eta/runtime/nanbox.h"

namespace eta::runtime::types {
    using namespace eta::runtime::nanbox;

    /**
     * @brief Opaque CSV reader runtime object.
     *
     * The parser backend is type-erased behind `state` so only csv_builtins.cpp
     * depends on the csv-parser headers. `column_symbols` is GC-visited to keep
     * interned header symbols alive across streaming reads.
     */
    struct CsvReader {
        std::shared_ptr<void> state;
        std::vector<std::string> columns;
        std::vector<LispVal> column_symbols;
        std::size_t row_index{0};
        std::optional<char> comment;
        std::vector<std::string> null_tokens;
    };

    /**
     * @brief Opaque CSV writer runtime object.
     *
     * The writer backend is type-erased behind `state` so only csv_builtins.cpp
     * depends on csv-parser and file I/O headers.
     */
    struct CsvWriter {
        std::shared_ptr<void> state;
        std::size_t row_index{0};
    };
} ///< namespace eta::runtime::types
