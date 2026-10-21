#pragma once

/// Small parsing helpers shared by the dap_server*.cpp request families.

#include <cctype>
#include <charconv>
#include <cstdint>
#include <string>
#include <string_view>
#include <system_error>

namespace eta::dap {

inline std::string trim_copy(std::string s) {
    auto is_ws = [](unsigned char c) { return std::isspace(c) != 0; };
    while (!s.empty() && is_ws(static_cast<unsigned char>(s.front()))) s.erase(s.begin());
    while (!s.empty() && is_ws(static_cast<unsigned char>(s.back()))) s.pop_back();
    return s;
}

inline bool try_parse_i64(std::string_view text, int64_t& out) {
    if (text.empty()) return false;
    const char* begin = text.data();
    const char* end = text.data() + text.size();
    auto parsed = std::from_chars(begin, end, out, 10);
    return parsed.ec == std::errc{} && parsed.ptr == end;
}

inline bool iequals(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        const auto ac = static_cast<unsigned char>(a[i]);
        const auto bc = static_cast<unsigned char>(b[i]);
        if (std::tolower(ac) != std::tolower(bc)) return false;
    }
    return true;
}

} ///< namespace eta::dap
