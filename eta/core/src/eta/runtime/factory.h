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


    // Unified helper for heap allocation + boxing.
    // Reduces repetitive pattern: allocate<T, Kind>(...) -> box(HeapObject, id)
    template<typename T, ObjectKind Kind, typename... Args>
    inline_always
    std::expected<LispVal, RuntimeError> make_heap_object(Heap& heap, Args&&... args) {
        auto allocated = heap.allocate<T, Kind>(T{std::forward<Args>(args)...});
        if (allocated.has_value()) {
            return ops::box(Tag::HeapObject, allocated.value());
        }
        return allocated;
    }

    template<typename T>
    requires std::is_integral_v<T>
    inline_always
    std::expected<LispVal, RuntimeError> make_fixnum(Heap& heap, const T fixnum) {
        const auto enc = nanbox::ops::encode<T>(fixnum);
        if (!enc.has_value()) {
            //! too big to encode directly, put onto heap.
            return make_heap_object<T, ObjectKind::Fixnum>(heap, fixnum);
        }
        return enc.value();
    }

    inline_always
    std::expected<LispVal, RuntimeError> make_flonum(const double f) {
        return ops::encode(f);
    }

    inline_always
    std::expected<types::Symbol, RuntimeError> make_symbol(InternTable& table, const std::string& str) {
        const auto res = table.intern(str);
        if (!res.has_value()) {
            return res;
        }
        return ops::box(Tag::Symbol, res.value());
    }

    inline_always
    std::expected<types::String, RuntimeError> make_string(Heap&, InternTable& table, const std::string& str) {
        const auto res = table.intern(str);
        if (!res.has_value()) {
            return res;
        }
        return ops::box(Tag::String, res.value());
    }

    inline_always
    std::expected<LispVal, RuntimeError> make_vector(Heap& heap, std::vector<LispVal> elements) {
        return make_heap_object<types::Vector, ObjectKind::Vector>(heap, types::Vector{.elements = std::move(elements)});
    }

    inline_always
    std::expected<LispVal, RuntimeError> make_bytevector(Heap& heap, std::vector<std::uint8_t> data) {
        return make_heap_object<types::ByteVector, ObjectKind::ByteVector>(heap, types::ByteVector{.data = std::move(data)});
    }

    inline_always
    std::expected<LispVal, RuntimeError> make_closure(Heap& heap, const vm::BytecodeFunction* func, std::vector<LispVal> upvals) {
        return make_heap_object<types::Closure, ObjectKind::Closure>(heap, types::Closure{.func = func, .upvals = std::move(upvals)});
    }

    inline_always
    std::expected<LispVal, RuntimeError> make_continuation(Heap& heap, std::vector<LispVal> stack, std::vector<vm::Frame> frames, std::vector<vm::WindFrame> winding_stack) {
        return make_heap_object<types::Continuation, ObjectKind::Continuation>(heap, types::Continuation{.stack = std::move(stack), .frames = std::move(frames), .winding_stack = std::move(winding_stack)});
    }

    inline_always
    std::expected<LispVal, RuntimeError> make_multiple_values(Heap& heap, std::vector<LispVal> vals) {
        return make_heap_object<types::MultipleValues, ObjectKind::MultipleValues>(heap, types::MultipleValues{.vals = std::move(vals)});
    }

    inline_always
    std::expected<LispVal, RuntimeError> make_cons(Heap& heap, const LispVal car) {
        return make_heap_object<types::Cons, ObjectKind::Cons>(heap, types::Cons{.car = car, .cdr = nanbox::Nil});
    }

    inline_always
    std::expected<LispVal, RuntimeError> make_cons(Heap& heap, const LispVal car, const LispVal cdr) {
        return make_heap_object<types::Cons, ObjectKind::Cons>(heap, types::Cons{.car = car, .cdr = cdr});
    }

    inline_always
    std::expected<LispVal, RuntimeError> make_primitive(Heap& heap, types::PrimitiveFunc func, uint32_t arity, bool has_rest = false) {
        return make_heap_object<types::Primitive, ObjectKind::Primitive>(heap, types::Primitive{.func = std::move(func), .arity = arity, .has_rest = has_rest});
    }

    inline_always
    std::expected<LispVal, RuntimeError> make_port(Heap& heap, std::shared_ptr<Port> port) {
        return make_heap_object<types::PortObject, ObjectKind::Port>(heap, types::PortObject{.port = std::move(port)});
    }
}


