#pragma once

#include <eta/runtime/nanbox.h>

namespace eta::runtime::memory::value_visit {
    using namespace eta::runtime::nanbox;
    using ops::tag;
    using ops::payload;

    template <typename R = void>
    struct ValueVisitor {
        using result_type = R;
        virtual ~ValueVisitor() = default;
        virtual R visit_fixnum(std::int64_t) = 0;
        virtual R visit_char(char32_t) = 0;
        virtual R visit_string(uint64_t /*intern id or payload*/) = 0;
        virtual R visit_symbol(uint64_t /*intern id or payload*/) = 0;
        virtual R visit_nan() = 0;
        virtual R visit_heapref(uint64_t /*ObjectId*/) = 0;
        virtual R visit_nil() = 0;
    };

    // Decode a LispVal and dispatch to ValueVisitor. This is centralized and header-only.
    template <typename R> R visit_value(const LispVal v, ValueVisitor<R>& visitor) {
        switch (const auto t = tag(v)) {
            case Tag::Fixnum: {
                // Sign-extend payload (47-bit) to 64-bit signed
                const uint64_t raw = payload(v);
                std::int64_t signed_val = static_cast<std::int64_t>(raw);
                // If the sign bit (bit 46) is set, extend it through upper bits
                if (raw & eta::runtime::nanbox::constants::FIXNUM_SIGN_BIT) {
                    signed_val |= static_cast<std::int64_t>(eta::runtime::nanbox::constants::FIXNUM_SIGN_EXTEND_MASK);
                }
                return visitor.visit_fixnum(signed_val);
            }
            case Tag::Char: {
                return visitor.visit_char(static_cast<char32_t>(payload(v)));
            }
            case Tag::String: {
                return visitor.visit_string(payload(v));
            }
            case Tag::Symbol: {
                return visitor.visit_symbol(payload(v));
            }
            case Tag::Nan: {
                return visitor.visit_nan();
            }
            case Tag::HeapObject: {
                return visitor.visit_heapref(payload(v));
            }
            case Tag::Nil:
            default:
                return visitor.visit_nil();
        }
    }
}
