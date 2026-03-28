#pragma once

#include <expected>
#include <eta/arch.h>

#include "nanbox.h"
#include "error.h"
#include "memory/heap.h"
#include "memory/intern_table.h"
#include "types/types.h"

namespace eta::runtime::memory::factory {
    using namespace eta::runtime::nanbox;
    using namespace eta::runtime::error;


    inline_always
    std::expected<LispVal, RuntimeError> make_fixnum(Heap& heap, const uint64_t fixnum) {
        const auto enc = nanbox::ops::encode<uint64_t>(fixnum);
        if (!enc.has_value()) {
            //! too big to encode directly, put onto heap.
            auto allocated = heap.allocate<uint64_t, ObjectKind::Fixnum>(fixnum);
            if (allocated.has_value()) {
                return ops::box(Tag::HeapObject, allocated.value());
            }
            return allocated;
        }
        return enc.value();
    }

    inline_always
    std::expected<LispVal, RuntimeError> make_fixnum(Heap& heap, const int64_t fixnum) {
        const auto enc = nanbox::ops::encode<int64_t>(fixnum);
        if (!enc.has_value()) {
            auto allocated = heap.allocate<int64_t, ObjectKind::Fixnum>(fixnum);
            if (allocated.has_value()) {
                return ops::box(Tag::HeapObject, allocated.value());
            }
            return allocated;
        }
        return enc.value();
    }

    inline_always
    std::expected<LispVal, RuntimeError> make_flonum(const double f) {
        return ops::encode(f);
    }

    inline_always
    std::expected<LispVal, RuntimeError> make_symbol(InternTable& table, const std::string& str) {
        const auto res = table.intern(str);
        if (!res.has_value()) {
            return res;
        }
        return ops::box(Tag::Symbol, res.value());
    }

    //! Currently, all strings are intern strings - we will want to change this so that only smaller strings
    //! are automatically interned. Large strings should be heap-allocated.

    inline_always
    std::expected<LispVal, RuntimeError> make_string(Heap& heap, InternTable& table, const std::string& str) {
        if (str.length() > 32) {
            auto allocated = heap.allocate<types::String, ObjectKind::String>(types::String{.value = str});
            if (allocated.has_value()) {
                return ops::box(Tag::HeapObject, allocated.value());
            }
            return allocated;
        }
        const auto res = table.intern(str);
        if (!res.has_value()) {
            return res;
        }
        return ops::box(Tag::String, res.value());
    }

    inline_always
    std::expected<LispVal, RuntimeError> make_vector(Heap& heap, std::vector<LispVal> elements) {
        auto allocated = heap.allocate<types::Vector, ObjectKind::Vector>(types::Vector{.elements = std::move(elements)});
        if (allocated.has_value()) {
            return ops::box(Tag::HeapObject, allocated.value());
        }
        return allocated;
    }

    inline_always
    std::expected<LispVal, RuntimeError> make_bytevector(Heap& heap, std::vector<std::uint8_t> data) {
        auto allocated = heap.allocate<types::ByteVector, ObjectKind::ByteVector>(types::ByteVector{.data = std::move(data)});
        if (allocated.has_value()) {
            return ops::box(Tag::HeapObject, allocated.value());
        }
        return allocated;
    }

    inline_always
    std::expected<LispVal, RuntimeError> make_closure(Heap& heap, const vm::BytecodeFunction* func, std::vector<LispVal> upvals) {
        auto allocated = heap.allocate<types::Closure, ObjectKind::Closure>(types::Closure{.func = func, .upvals = std::move(upvals)});
        if (allocated.has_value()) {
            return ops::box(Tag::HeapObject, allocated.value());
        }
        return allocated;
    }

    inline_always
    std::expected<LispVal, RuntimeError> make_continuation(Heap& heap, std::vector<LispVal> stack, std::vector<vm::Frame> frames) {
        auto allocated = heap.allocate<types::Continuation, ObjectKind::Continuation>(types::Continuation{.stack = std::move(stack), .frames = std::move(frames)});
        if (allocated.has_value()) {
            return ops::box(Tag::HeapObject, allocated.value());
        }
        return allocated;
    }

    inline_always
    std::expected<LispVal, RuntimeError> make_cons(Heap& heap, const LispVal car) {
        auto allocated = heap.allocate<types::Cons, ObjectKind::Cons>(types::Cons {.car = car, .cdr = nanbox::Nil});
        if (allocated.has_value()) {
            return ops::box(Tag::HeapObject, allocated.value());
        }
        return allocated;
    }

    inline_always
    std::expected<LispVal, RuntimeError> make_cons(Heap& heap, const LispVal car, const LispVal cdr) {
        auto allocated = heap.allocate<types::Cons, ObjectKind::Cons>(types::Cons {.car = car, .cdr = cdr});
        if (allocated.has_value()) {
            return ops::box(Tag::HeapObject, allocated.value());
        }
        return allocated;
    }

    inline_always
    std::expected<LispVal, RuntimeError> make_lambda(Heap& heap,
                                                    std::vector<LispVal> formals,
                                                    const LispVal& body,
                                                    std::vector<LispVal> up_values) {
        auto allocated = heap.allocate<types::Lambda, ObjectKind::Lambda>(
            types::Lambda {
                .formals = std::move(formals),
                .body = body,
                .up_values = std::move(up_values)
            });
        if (allocated.has_value()) {
            return ops::box(Tag::HeapObject, allocated.value());
        }
        return allocated;
    }

}