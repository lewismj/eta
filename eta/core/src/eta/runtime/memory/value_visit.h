#pragma once

#include <bit>
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
        virtual R visit_flonum(double) = 0;  // For raw IEEE 754 doubles
        virtual R visit_char(char32_t) = 0;
        virtual R visit_string(uint64_t /*intern id or payload*/) = 0;
        virtual R visit_symbol(uint64_t /*intern id or payload*/) = 0;
        virtual R visit_nan() = 0;
        virtual R visit_heapref(uint64_t /*ObjectId*/) = 0;
        virtual R visit_nil() = 0;
    };

    // Decode a LispVal and dispatch to ValueVisitor. This is centralized and header-only.
    // Returns true if the value was a boxed value, false if it was a raw double (flonum).
    template <typename R> R visit_value(const LispVal v, ValueVisitor<R>& visitor) {
        // Check if this is a boxed value first - raw doubles should not be dispatched
        // through the tag system as their exponent bits would be misinterpreted as tags
        if (!ops::is_boxed(v)) {
            // This is a raw IEEE 754 double (flonum)
            return visitor.visit_flonum(std::bit_cast<double>(v));
        }

        switch (const auto t = tag(v)) {
            case Tag::Fixnum: {
                // Use centralized sign extension helper
                return visitor.visit_fixnum(eta::runtime::nanbox::ops::sign_extend_fixnum(payload(v)));
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
