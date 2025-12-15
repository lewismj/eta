#pragma once

#include <eta/runtime/memory/heap.h>
#include <eta/runtime/types/types.h>

namespace eta::runtime::memory::heap {

    // Visitor over heap-allocated objects. R defaults to void.
    template <typename R = void>
    struct HeapVisitor {
        using result_type = R;
        virtual ~HeapVisitor() = default;

        // One method per pointer-containing heap type
        virtual R visit_cons(const eta::runtime::types::Cons& c) = 0;
        virtual R visit_lambda(const eta::runtime::types::Lambda& l) = 0;

        // Fallback for leaf/unknown kinds (no outward edges)
        virtual R visit_leaf(ObjectKind kind, const void* payload) = 0;
    };

    // Centralized dispatcher – single switch over ObjectKind
    template <typename R>
    inline R visit_heap_object(const ObjectHeader& hdr, const void* payload, HeapVisitor<R>& v) {
        using enum ObjectKind;
        switch (hdr.kind) {
            case Cons:   return v.visit_cons(*static_cast<const eta::runtime::types::Cons*>(payload));
            case Lambda: return v.visit_lambda(*static_cast<const eta::runtime::types::Lambda*>(payload));
            case Fixnum: return v.visit_leaf(ObjectKind::Fixnum, payload);
            case Unknown:
            default:
                return v.visit_leaf(hdr.kind, payload);
        }
    }

    template <typename R>
    inline R visit_heap_object(const HeapEntry& e, HeapVisitor<R>& v) {
        return visit_heap_object<R>(e.header, e.ptr, v);
    }
}
