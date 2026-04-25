#pragma once

/**
 * Enum-to-string utility macros.
 *
 * Usage:
 *
 * In header (declaration only):
 *   ETA_ENUM_TO_STRING_DECL(MyEnum)
 *
 * In header or source (inline definition):
 *   ETA_ENUM_TO_STRING_BEGIN(MyEnum)
 *       ETA_ENUM_CASE(MyEnum, Value1)
 *       ETA_ENUM_CASE(MyEnum, Value2)
 *   ETA_ENUM_TO_STRING_END("Unknown")
 */

/// Declaration macro for use in headers - declares the to_string function
#define ETA_ENUM_TO_STRING_DECL(EnumType) \
    constexpr const char* to_string(EnumType e) noexcept

/// Begin an inline to_string definition
#define ETA_ENUM_TO_STRING_BEGIN(EnumType) \
    constexpr const char* to_string(const EnumType e) noexcept { \
        using enum EnumType; \
        switch (e) {

/// Case for each enum value - uses 'using enum' so unqualified names work
#define ETA_ENUM_CASE(Value) \
            case Value: return #Value;

/// End the to_string definition with a default case
#define ETA_ENUM_TO_STRING_END(DefaultStr) \
            default: return DefaultStr; \
        } \
    }

