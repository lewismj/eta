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


    /**
     * Unified helper for heap allocation + boxing.
     * Reduces repetitive pattern: allocate<T, Kind>(...) -> box(HeapObject, id)
     */
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
    std::expected<LispVal, RuntimeError> make_cons(Heap& heap, const LispVal car, const LispVal cdr) {
        auto id = heap.alloc_cons(car, cdr);
        if (id.has_value()) {
            return ops::box(Tag::HeapObject, *id);
        }
        return make_heap_object<types::Cons, ObjectKind::Cons>(
            heap, types::Cons{.car = car, .cdr = cdr});
    }

    inline_always
    std::expected<LispVal, RuntimeError> make_cons(Heap& heap, const LispVal car) {
        return make_cons(heap, car, nanbox::Nil);
    }

    inline_always
    std::expected<LispVal, RuntimeError> make_primitive(Heap& heap, types::PrimitiveFunc func, uint32_t arity, bool has_rest = false) {
        return make_heap_object<types::Primitive, ObjectKind::Primitive>(heap, types::Primitive{.func = std::move(func), .arity = arity, .has_rest = has_rest});
    }

    inline_always
    std::expected<LispVal, RuntimeError> make_primitive(Heap& heap, types::PrimitiveFunc func, uint32_t arity, bool has_rest,
                                                        std::vector<LispVal> gc_roots) {
        return make_heap_object<types::Primitive, ObjectKind::Primitive>(heap, types::Primitive{.func = std::move(func), .arity = arity, .has_rest = has_rest, .gc_roots = std::move(gc_roots)});
    }

    inline_always
    std::expected<LispVal, RuntimeError> make_guardian(Heap& heap) {
        return make_heap_object<types::Guardian, ObjectKind::Guardian>(heap, types::Guardian{});
    }

    inline_always
    std::expected<LispVal, RuntimeError> make_port(Heap& heap, std::shared_ptr<Port> port) {
        return make_heap_object<types::PortObject, ObjectKind::Port>(heap, types::PortObject{.port = std::move(port)});
    }

    inline_always
    std::expected<LispVal, RuntimeError> make_logic_var(Heap& heap) {
        return make_heap_object<types::LogicVar, ObjectKind::LogicVar>(heap, types::LogicVar{});
    }

    inline_always
    std::expected<LispVal, RuntimeError> make_logic_var(Heap& heap, std::string name) {
        return make_heap_object<types::LogicVar, ObjectKind::LogicVar>(
            heap, types::LogicVar{ .binding = std::nullopt, .name = std::move(name) });
    }


    inline_always
    std::expected<LispVal, RuntimeError> make_tape(Heap& heap) {
        return make_heap_object<types::Tape, ObjectKind::Tape>(heap, types::Tape{});
    }

    inline_always
    std::expected<LispVal, RuntimeError> make_fact_table(Heap& heap, std::vector<std::string> col_names) {
        std::size_t ncols = col_names.size();
        types::FactTable ft;
        ft.col_names = std::move(col_names);
        ft.columns.resize(ncols);
        ft.indexes.resize(ncols);
        return make_heap_object<types::FactTable, ObjectKind::FactTable>(heap, std::move(ft));
    }

    inline_always
    std::expected<LispVal, RuntimeError> make_hash_map(Heap& heap, types::HashMap map) {
        return make_heap_object<types::HashMap, ObjectKind::HashMap>(heap, std::move(map));
    }

    inline_always
    std::expected<LispVal, RuntimeError> make_hash_map(Heap& heap,
                                                       const std::size_t requested_capacity,
                                                       const std::uint64_t seed) {
        return make_hash_map(heap, types::make_empty_hash_map(requested_capacity, seed));
    }

    inline_always
    std::expected<LispVal, RuntimeError> make_hash_set(Heap& heap, types::HashSet set) {
        return make_heap_object<types::HashSet, ObjectKind::HashSet>(heap, std::move(set));
    }

    inline_always
    std::expected<LispVal, RuntimeError> make_hash_set(Heap& heap,
                                                       const std::size_t requested_capacity,
                                                       const std::uint64_t seed) {
        return make_hash_set(heap, types::make_empty_hash_set(requested_capacity, seed));
    }

    inline_always
    std::expected<LispVal, RuntimeError> make_csv_reader(Heap& heap, types::CsvReader reader) {
        return make_heap_object<types::CsvReader, ObjectKind::CsvReader>(heap, std::move(reader));
    }

    inline_always
    std::expected<LispVal, RuntimeError> make_csv_writer(Heap& heap, types::CsvWriter writer) {
        return make_heap_object<types::CsvWriter, ObjectKind::CsvWriter>(heap, std::move(writer));
    }

    inline_always
    std::expected<LispVal, RuntimeError> make_regex(
        Heap& heap,
        std::string pattern,
        std::regex::flag_type flags,
        std::vector<std::string> flag_names,
        std::vector<std::pair<std::string, std::size_t>> named_group_indices,
        std::shared_ptr<const std::regex> compiled) {
        return make_heap_object<types::Regex, ObjectKind::Regex>(
            heap,
            types::Regex{
                .pattern = std::move(pattern),
                .flags = flags,
                .flag_names = std::move(flag_names),
                .named_group_indices = std::move(named_group_indices),
                .compiled = std::move(compiled)
            });
    }

    inline_always
    std::expected<LispVal, RuntimeError> make_log_sink(Heap& heap, types::LogSink sink) {
        return make_heap_object<types::LogSink, ObjectKind::LogSink>(heap, std::move(sink));
    }

    inline_always
    std::expected<LispVal, RuntimeError> make_log_logger(Heap& heap, types::LogLogger logger) {
        return make_heap_object<types::LogLogger, ObjectKind::LogLogger>(heap, std::move(logger));
    }

    inline_always
    std::expected<LispVal, RuntimeError> make_process_handle(Heap& heap, std::shared_ptr<types::ProcessHandle> handle) {
        return make_heap_object<types::ProcessHandleObject, ObjectKind::ProcessHandle>(
            heap, types::ProcessHandleObject{.handle = std::move(handle)});
    }

    inline_always
    std::expected<LispVal, RuntimeError> make_compound(Heap& heap, LispVal functor,
                                                       std::vector<LispVal> args) {
        return make_heap_object<types::CompoundTerm, ObjectKind::CompoundTerm>(
            heap, types::CompoundTerm{ .functor = functor, .args = std::move(args) });
    }
}

