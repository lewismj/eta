#pragma once

#include <string_view>
#include <ostream>

#define ETA_ENUM_TO_STRING_DECL(EnumType) \
    constexpr std::string_view to_string(EnumType value) noexcept; \
    inline std::ostream& operator<<(std::ostream& os, EnumType value) { \
        return os << to_string(value); \
    }

#define ETA_ENUM_TO_STRING_BEGIN(EnumType) \
    constexpr std::string_view to_string(EnumType value) noexcept { \
        using enum EnumType; \
        switch (value) {

#define ETA_ENUM_CASE(Case) case Case: return #Case;

#define ETA_ENUM_TO_STRING_END(DefaultValue) \
        } \
        return DefaultValue; \
    }
