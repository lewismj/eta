#pragma once

#include <eta/runtime/memory/heap.h>
#include <eta/runtime/types/types.h>

namespace eta::runtime::memory::heap {

    /// Visitor over heap-allocated objects. R defaults to void.
    template <typename R = void>
    struct HeapVisitor {
        using result_type = R;
        virtual ~HeapVisitor() = default;

        /// One method per pointer-containing heap type
        virtual R visit_cons(const eta::runtime::types::Cons& c) = 0;
        virtual R visit_closure(const eta::runtime::types::Closure& c) = 0;
        virtual R visit_vector(const eta::runtime::types::Vector& v) = 0;
        virtual R visit_continuation(const eta::runtime::types::Continuation& c) = 0;
        virtual R visit_multiple_values(const eta::runtime::types::MultipleValues& mv) = 0;
        virtual R visit_logic_var(const eta::runtime::types::LogicVar& lv) = 0;
        virtual R visit_tape(const eta::runtime::types::Tape& t) = 0;
        virtual R visit_primitive(const eta::runtime::types::Primitive& p) = 0;
        virtual R visit_guardian(const eta::runtime::types::Guardian& g) = 0;
        virtual R visit_fact_table(const eta::runtime::types::FactTable& ft) = 0;
        virtual R visit_csv_reader(const eta::runtime::types::CsvReader& reader) = 0;
        virtual R visit_compound_term(const eta::runtime::types::CompoundTerm& ct) = 0;

        /// Fallback for leaf/unknown kinds (no outward edges)
        virtual R visit_leaf(ObjectKind kind, const void* payload) = 0;
    };

    template <typename R>
    inline_always R visit_heap_object(const ObjectHeader& hdr, const void* payload, HeapVisitor<R>& v) {
        using enum ObjectKind;
        switch (hdr.kind) {
            case Cons:         return v.visit_cons(*static_cast<const eta::runtime::types::Cons*>(payload));
            case Closure:      return v.visit_closure(*static_cast<const eta::runtime::types::Closure*>(payload));
            case Vector:       return v.visit_vector(*static_cast<const eta::runtime::types::Vector*>(payload));
            case Continuation: return v.visit_continuation(*static_cast<const eta::runtime::types::Continuation*>(payload));
            case MultipleValues: return v.visit_multiple_values(*static_cast<const eta::runtime::types::MultipleValues*>(payload));
            case LogicVar:     return v.visit_logic_var(*static_cast<const eta::runtime::types::LogicVar*>(payload));
            case Tape:         return v.visit_tape(*static_cast<const eta::runtime::types::Tape*>(payload));
            case Primitive:    return v.visit_primitive(*static_cast<const eta::runtime::types::Primitive*>(payload));
            case Guardian:     return v.visit_guardian(*static_cast<const eta::runtime::types::Guardian*>(payload));
            case FactTable:    return v.visit_fact_table(*static_cast<const eta::runtime::types::FactTable*>(payload));
            case CsvReader:    return v.visit_csv_reader(*static_cast<const eta::runtime::types::CsvReader*>(payload));
            case CompoundTerm: return v.visit_compound_term(*static_cast<const eta::runtime::types::CompoundTerm*>(payload));

            case Fixnum:
            case ByteVector:
            case Port:
            case Tensor:
            case NNModule:
            case Optimizer:
            case CsvWriter:
            case Regex:
            case NngSocket:   ///< leaf: holds only OS handle + raw bytes, no GC refs
            case Unknown:
                return v.visit_leaf(hdr.kind, payload);
        }
    }

    template <typename R>
    inline R visit_heap_object(const HeapEntry& e, HeapVisitor<R>& v) {
        return visit_heap_object<R>(e.header, e.ptr, v);
    }
}
