#pragma once

#include <cstddef>
#include <memory>
#include <regex>
#include <string>
#include <utility>
#include <vector>

namespace eta::runtime::types {

    /**
     * @brief Compiled regular expression object.
     *
     * Stores the source pattern, compile flags, optional named-capture index
     * metadata, and a shared compiled engine object.
     */
    struct Regex {
        std::string pattern;
        std::regex::flag_type flags{std::regex::ECMAScript};
        std::vector<std::string> flag_names;
        std::vector<std::pair<std::string, std::size_t>> named_group_indices;
        std::shared_ptr<const std::regex> compiled;
    };

} ///< namespace eta::runtime::types
