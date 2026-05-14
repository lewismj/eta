#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <optional>
#include <random>
#include <string>
#include <utility>
#include <vector>

#include "eta/runtime/core_primitives_internal.h"
#include "eta/runtime/factory.h"
#include "eta/runtime/numeric_value.h"
#include "eta/runtime/string_view.h"
#include "eta/runtime/util/siphash.h"
#include "eta/runtime/vm/vm.h"

namespace eta::runtime {

void PrimReg::register_pair_list_bridge() {
    using Args = PrimReg::Args;
    auto& env = this->env;
    auto& heap = this->heap;

    /**
     * Pairs / Lists: cons car cdr pair? null? list
     */

    env.register_builtin("cons", 2, false, [&heap](Args args) -> std::expected<LispVal, RuntimeError> {
        return make_cons(heap, args[0], args[1]);
    });

    env.register_builtin("car", 1, false, [&heap](Args args) -> std::expected<LispVal, RuntimeError> {
        if (!ops::is_boxed(args[0]) || ops::tag(args[0]) != Tag::HeapObject) {
            return std::unexpected(RuntimeError{VMError{
                RuntimeErrorCode::TypeError, "car: not a pair"}});
        }
        auto* cons =
            heap.try_get_as<ObjectKind::Cons, types::Cons>(ops::payload(args[0]));
        if (!cons) {
            return std::unexpected(RuntimeError{VMError{
                RuntimeErrorCode::TypeError, "car: not a pair"}});
        }
        return cons->car;
    });

    env.register_builtin("cdr", 1, false, [&heap](Args args) -> std::expected<LispVal, RuntimeError> {
        if (!ops::is_boxed(args[0]) || ops::tag(args[0]) != Tag::HeapObject) {
            return std::unexpected(RuntimeError{VMError{
                RuntimeErrorCode::TypeError, "cdr: not a pair"}});
        }
        auto* cons =
            heap.try_get_as<ObjectKind::Cons, types::Cons>(ops::payload(args[0]));
        if (!cons) {
            return std::unexpected(RuntimeError{VMError{
                RuntimeErrorCode::TypeError, "cdr: not a pair"}});
        }
        return cons->cdr;
    });

    env.register_builtin("pair?", 1, false, [&heap](Args args) -> std::expected<LispVal, RuntimeError> {
        if (!ops::is_boxed(args[0]) || ops::tag(args[0]) != Tag::HeapObject) {
            return nanbox::False;
        }
        return heap.try_get_as<ObjectKind::Cons, types::Cons>(ops::payload(args[0]))
                   ? nanbox::True
                   : nanbox::False;
    });

    env.register_builtin("null?", 1, false, [](Args args) -> std::expected<LispVal, RuntimeError> {
        return (args[0] == nanbox::Nil) ? nanbox::True : nanbox::False;
    });

    env.register_builtin("list", 0, true, [&heap](Args args) -> std::expected<LispVal, RuntimeError> {
        LispVal result = nanbox::Nil;
        auto roots = heap.make_external_root_frame();
        for (auto value : args) roots.push(value);
        for (auto it = args.rbegin(); it != args.rend(); ++it) {
            auto cons = make_cons(heap, *it, result);
            if (!cons) return cons;
            result = *cons;
            roots.push(result);
        }
        return result;
    });
}

void PrimReg::register_sequences() {
    using Args = PrimReg::Args;
    auto& env = this->env;
    auto& heap = this->heap;
    auto& intern_table = this->intern_table;

    /**
     * List operations: length append reverse list-ref
     */

    env.register_builtin("length", 1, false, [&heap](Args args) -> std::expected<LispVal, RuntimeError> {
        int64_t count = 0;
        LispVal cur = args[0];
        while (cur != nanbox::Nil) {
            if (!ops::is_boxed(cur) || ops::tag(cur) != Tag::HeapObject)
                return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError, "length: not a proper list"}});
            auto* cons = heap.try_get_as<ObjectKind::Cons, types::Cons>(ops::payload(cur));
            if (!cons) return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError, "length: not a proper list"}});
            ++count;
            cur = cons->cdr;
        }
        return make_fixnum(heap, count);
    });

    env.register_builtin("append", 0, true, [&heap](Args args) -> std::expected<LispVal, RuntimeError> {
        if (args.empty()) return nanbox::Nil;
        if (args.size() == 1) return args[0];

        /// Start from the last argument (which doesn't need to be a list)
        LispVal result = args.back();

        /// Process arguments right-to-left (all but the last must be proper lists)
        for (auto it = args.rbegin() + 1; it != args.rend(); ++it) {
            /// Collect elements of this list
            std::vector<LispVal> elems;
            LispVal cur = *it;
            while (cur != nanbox::Nil) {
                if (!ops::is_boxed(cur) || ops::tag(cur) != Tag::HeapObject)
                    return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError, "append: not a proper list"}});
                auto* cons = heap.try_get_as<ObjectKind::Cons, types::Cons>(ops::payload(cur));
                if (!cons) return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError, "append: not a proper list"}});
                elems.push_back(cons->car);
                cur = cons->cdr;
            }
            /// Build from right to left
            for (auto rit = elems.rbegin(); rit != elems.rend(); ++rit) {
                auto cons = make_cons(heap, *rit, result);
                if (!cons) return cons;
                result = *cons;
            }
        }
        return result;
    });

    env.register_builtin("reverse", 1, false, [&heap](Args args) -> std::expected<LispVal, RuntimeError> {
        LispVal result = nanbox::Nil;
        LispVal cur = args[0];
        while (cur != nanbox::Nil) {
            if (!ops::is_boxed(cur) || ops::tag(cur) != Tag::HeapObject)
                return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError, "reverse: not a proper list"}});
            auto* cons = heap.try_get_as<ObjectKind::Cons, types::Cons>(ops::payload(cur));
            if (!cons) return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError, "reverse: not a proper list"}});
            auto new_cons = make_cons(heap, cons->car, result);
            if (!new_cons) return new_cons;
            result = *new_cons;
            cur = cons->cdr;
        }
        return result;
    });

    env.register_builtin("list-ref", 2, false, [&heap](Args args) -> std::expected<LispVal, RuntimeError> {
        auto idx = classify_numeric(args[1], heap);
        if (!idx.is_valid() || idx.is_flonum() || idx.int_val < 0)
            return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError, "list-ref: index must be a non-negative integer"}});
        LispVal cur = args[0];
        for (int64_t i = 0; i < idx.int_val; ++i) {
            if (!ops::is_boxed(cur) || ops::tag(cur) != Tag::HeapObject)
                return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError, "list-ref: index out of bounds"}});
            auto* cons = heap.try_get_as<ObjectKind::Cons, types::Cons>(ops::payload(cur));
            if (!cons) return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError, "list-ref: index out of bounds"}});
            cur = cons->cdr;
        }
        if (!ops::is_boxed(cur) || ops::tag(cur) != Tag::HeapObject)
            return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError, "list-ref: index out of bounds"}});
        auto* cons = heap.try_get_as<ObjectKind::Cons, types::Cons>(ops::payload(cur));
        if (!cons) return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError, "list-ref: index out of bounds"}});
        return cons->car;
    });

    env.register_builtin("list-tail", 2, false, [&heap](Args args) -> std::expected<LispVal, RuntimeError> {
        auto idx = classify_numeric(args[1], heap);
        if (!idx.is_valid() || idx.is_flonum() || idx.int_val < 0)
            return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError, "list-tail: index must be a non-negative integer"}});
        LispVal cur = args[0];
        for (int64_t i = 0; i < idx.int_val; ++i) {
            if (!ops::is_boxed(cur) || ops::tag(cur) != Tag::HeapObject)
                return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError, "list-tail: index out of bounds"}});
            auto* cons = heap.try_get_as<ObjectKind::Cons, types::Cons>(ops::payload(cur));
            if (!cons) return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError, "list-tail: index out of bounds"}});
            cur = cons->cdr;
        }
        return cur;
    });

    env.register_builtin("set-car!", 2, false, [&heap](Args args) -> std::expected<LispVal, RuntimeError> {
        if (!ops::is_boxed(args[0]) || ops::tag(args[0]) != Tag::HeapObject)
            return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError, "set-car!: not a pair"}});
        auto* cons = heap.try_get_as<ObjectKind::Cons, types::Cons>(ops::payload(args[0]));
        if (!cons) return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError, "set-car!: not a pair"}});
        cons->car = args[1];
        return nanbox::Nil;
    });

    env.register_builtin("set-cdr!", 2, false, [&heap](Args args) -> std::expected<LispVal, RuntimeError> {
        if (!ops::is_boxed(args[0]) || ops::tag(args[0]) != Tag::HeapObject)
            return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError, "set-cdr!: not a pair"}});
        auto* cons = heap.try_get_as<ObjectKind::Cons, types::Cons>(ops::payload(args[0]));
        if (!cons) return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError, "set-cdr!: not a pair"}});
        cons->cdr = args[1];
        return nanbox::Nil;
    });

    env.register_builtin("assq", 2, false, [&heap](Args args) -> std::expected<LispVal, RuntimeError> {
        LispVal key = args[0];
        LispVal cur = args[1];
        while (cur != nanbox::Nil) {
            if (!ops::is_boxed(cur) || ops::tag(cur) != Tag::HeapObject)
                return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError, "assq: not a proper alist"}});
            auto* outer = heap.try_get_as<ObjectKind::Cons, types::Cons>(ops::payload(cur));
            if (!outer) return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError, "assq: not a proper alist"}});
            /// Each element must be a pair
            LispVal pair = outer->car;
            if (ops::is_boxed(pair) && ops::tag(pair) == Tag::HeapObject) {
                auto* inner = heap.try_get_as<ObjectKind::Cons, types::Cons>(ops::payload(pair));
                if (inner && inner->car == key) {
                    return pair; ///< Return the whole pair
                }
            }
            cur = outer->cdr;
        }
        return nanbox::False;
    });

    env.register_builtin("assoc", 2, false, [&heap, &intern_table](Args args) -> std::expected<LispVal, RuntimeError> {
        LispVal key = args[0];
        LispVal cur = args[1];

        std::function<bool(LispVal, LispVal)> equal_impl = [&](LispVal a, LispVal b) -> bool {
            if (a == b) return true;
            if (!ops::is_boxed(a) || !ops::is_boxed(b)) return false;
            if (ops::tag(a) == Tag::String && ops::tag(b) == Tag::String) {
                auto sa = StringView::try_from(a, intern_table);
                auto sb = StringView::try_from(b, intern_table);
                if (sa && sb) return sa->view() == sb->view();
                return false;
            }
            if (ops::tag(a) != Tag::HeapObject || ops::tag(b) != Tag::HeapObject) return false;
            auto na = classify_numeric(a, heap);
            auto nb = classify_numeric(b, heap);
            if (na.is_valid() && nb.is_valid()) {
                if (na.is_flonum() || nb.is_flonum()) return na.as_double() == nb.as_double();
                return na.int_val == nb.int_val;
            }
            return false;
        };

        while (cur != nanbox::Nil) {
            if (!ops::is_boxed(cur) || ops::tag(cur) != Tag::HeapObject)
                return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError, "assoc: not a proper alist"}});
            auto* outer = heap.try_get_as<ObjectKind::Cons, types::Cons>(ops::payload(cur));
            if (!outer) return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError, "assoc: not a proper alist"}});
            LispVal pair = outer->car;
            if (ops::is_boxed(pair) && ops::tag(pair) == Tag::HeapObject) {
                auto* inner = heap.try_get_as<ObjectKind::Cons, types::Cons>(ops::payload(pair));
                if (inner && equal_impl(inner->car, key)) {
                    return pair;
                }
            }
            cur = outer->cdr;
        }
        return nanbox::False;
    });

    env.register_builtin("member", 2, false, [&heap](Args args) -> std::expected<LispVal, RuntimeError> {
        LispVal obj = args[0];
        LispVal cur = args[1];
        while (cur != nanbox::Nil) {
            if (!ops::is_boxed(cur) || ops::tag(cur) != Tag::HeapObject)
                return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError, "member: not a proper list"}});
            auto* cons = heap.try_get_as<ObjectKind::Cons, types::Cons>(ops::payload(cur));
            if (!cons) return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError, "member: not a proper list"}});
            if (cons->car == obj) return cur;
            cur = cons->cdr;
        }
        return nanbox::False;
    });
}

void PrimReg::register_sequences_higher_order_bridge() {
    using Args = PrimReg::Args;
    auto& env = this->env;
    auto& heap = this->heap;
    auto& intern_table = this->intern_table;
    auto* vm = this->vm;

    /**
     * Higher-order: apply map for-each
     *
     * NOTE: (apply proc arg1 ... list) is now a VM-level special form
     * (OpCode::Apply / TailApply).  The SA intercepts calls to `apply` and
     * emits the dedicated opcode, so this primitive is only reachable when
     * `apply` is used as a first-class value (e.g. passed to another function).
     *
     * When a VM pointer is provided both map and for-each use call_value() to
     * invoke any callable (primitive or closure).  Without a VM pointer they
     * fall back to primitive-only mode.
     */

    env.register_builtin("apply", 2, true, [](Args /*args*/) -> std::expected<LispVal, RuntimeError> {
        return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError,
            "apply: cannot be used as a first-class value yet (use direct apply syntax instead)"}});
    });

    env.register_builtin("map", 2, false, [&heap, vm](Args args) -> std::expected<LispVal, RuntimeError> {
        LispVal proc = args[0];
        if (!ops::is_boxed(proc) || ops::tag(proc) != Tag::HeapObject)
            return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError, "map: first argument must be a procedure"}});

        std::vector<LispVal> results;
        LispVal cur = args[1];
        while (cur != nanbox::Nil) {
            if (!ops::is_boxed(cur) || ops::tag(cur) != Tag::HeapObject)
                return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError, "map: not a proper list"}});
            auto* cons = heap.try_get_as<ObjectKind::Cons, types::Cons>(ops::payload(cur));
            if (!cons) return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError, "map: not a proper list"}});

            std::expected<LispVal, RuntimeError> res;
            if (vm) {
                res = vm->call_value(proc, {cons->car});
            } else {
                auto* prim = heap.try_get_as<ObjectKind::Primitive, types::Primitive>(ops::payload(proc));
                if (!prim)
                    return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError,
                        "map: closures require a VM context (internal error  -  VM not provided)"}});
                std::vector<LispVal> call_args = {cons->car};
                res = prim->func(call_args);
            }
            if (!res) return res;
            results.push_back(*res);
            cur = cons->cdr;
        }
        /// Build result list in order
        LispVal result = nanbox::Nil;
        for (auto it = results.rbegin(); it != results.rend(); ++it) {
            auto cons_val = make_cons(heap, *it, result);
            if (!cons_val) return cons_val;
            result = *cons_val;
        }
        return result;
    });

    env.register_builtin("for-each", 2, false, [&heap, vm](Args args) -> std::expected<LispVal, RuntimeError> {
        LispVal proc = args[0];
        if (!ops::is_boxed(proc) || ops::tag(proc) != Tag::HeapObject)
            return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError, "for-each: first argument must be a procedure"}});

        LispVal cur = args[1];
        while (cur != nanbox::Nil) {
            if (!ops::is_boxed(cur) || ops::tag(cur) != Tag::HeapObject)
                return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError, "for-each: not a proper list"}});
            auto* cons = heap.try_get_as<ObjectKind::Cons, types::Cons>(ops::payload(cur));
            if (!cons) return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError, "for-each: not a proper list"}});

            std::expected<LispVal, RuntimeError> res;
            if (vm) {
                res = vm->call_value(proc, {cons->car});
            } else {
                auto* prim = heap.try_get_as<ObjectKind::Primitive, types::Primitive>(ops::payload(proc));
                if (!prim)
                    return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError,
                        "for-each: closures require a VM context (internal error  -  VM not provided)"}});
                std::vector<LispVal> call_args = {cons->car};
                res = prim->func(call_args);
            }
            if (!res) return res;
            cur = cons->cdr;
        }
        return nanbox::Nil;
    });

    /**
     * Deep equality: equal?
     */

    auto hash_key_equal = std::make_shared<std::function<bool(LispVal, LispVal)>>();
    auto equal_impl = std::make_shared<std::function<bool(LispVal, LispVal)>>();

    auto map_find_linear = [](const types::HashMap& map,
                              const LispVal key,
                              const std::function<bool(LispVal, LispVal)>& eq) -> std::optional<std::size_t> {
        for (std::size_t i = 0; i < map.state.size(); ++i) {
            if (map.state[i] != static_cast<std::uint8_t>(types::HashSlotState::Occupied)) continue;
            if (eq(map.keys[i], key)) return i;
        }
        return std::nullopt;
    };

    *hash_key_equal = [&, hash_key_equal, map_find_linear](LispVal a, LispVal b) -> bool {
        if (a == b) return true;
        if (!ops::is_boxed(a) || !ops::is_boxed(b)) return false;

        if (ops::tag(a) == Tag::String && ops::tag(b) == Tag::String) {
            auto sa = StringView::try_from(a, intern_table);
            auto sb = StringView::try_from(b, intern_table);
            return sa && sb && sa->view() == sb->view();
        }

        auto na = classify_numeric(a, heap);
        auto nb = classify_numeric(b, heap);
        if (na.is_valid() && nb.is_valid()) {
            if (na.is_fixnum() != nb.is_fixnum()) return false;
            if (na.is_flonum()) return na.float_val == nb.float_val;
            return na.int_val == nb.int_val;
        }

        if (ops::tag(a) != Tag::HeapObject || ops::tag(b) != Tag::HeapObject) return false;

        auto* ca = heap.try_get_as<ObjectKind::Cons, types::Cons>(ops::payload(a));
        auto* cb = heap.try_get_as<ObjectKind::Cons, types::Cons>(ops::payload(b));
        if (ca && cb) return (*hash_key_equal)(ca->car, cb->car) && (*hash_key_equal)(ca->cdr, cb->cdr);

        auto* va = heap.try_get_as<ObjectKind::Vector, types::Vector>(ops::payload(a));
        auto* vb = heap.try_get_as<ObjectKind::Vector, types::Vector>(ops::payload(b));
        if (va && vb) {
            if (va->elements.size() != vb->elements.size()) return false;
            for (std::size_t i = 0; i < va->elements.size(); ++i) {
                if (!(*hash_key_equal)(va->elements[i], vb->elements[i])) return false;
            }
            return true;
        }

        auto* bva = heap.try_get_as<ObjectKind::ByteVector, types::ByteVector>(ops::payload(a));
        auto* bvb = heap.try_get_as<ObjectKind::ByteVector, types::ByteVector>(ops::payload(b));
        if (bva && bvb) return bva->data == bvb->data;

        auto* hma = heap.try_get_as<ObjectKind::HashMap, types::HashMap>(ops::payload(a));
        auto* hmb = heap.try_get_as<ObjectKind::HashMap, types::HashMap>(ops::payload(b));
        if (hma && hmb) {
            if (hma->size != hmb->size) return false;
            for (std::size_t i = 0; i < hma->state.size(); ++i) {
                if (hma->state[i] != static_cast<std::uint8_t>(types::HashSlotState::Occupied)) continue;
                auto hit = map_find_linear(*hmb, hma->keys[i], *hash_key_equal);
                if (!hit) return false;
                if (!(*hash_key_equal)(hma->values[i], hmb->values[*hit])) return false;
            }
            return true;
        }

        auto* hsa = heap.try_get_as<ObjectKind::HashSet, types::HashSet>(ops::payload(a));
        auto* hsb = heap.try_get_as<ObjectKind::HashSet, types::HashSet>(ops::payload(b));
        if (hsa && hsb) {
            if (hsa->table.size != hsb->table.size) return false;
            for (std::size_t i = 0; i < hsa->table.state.size(); ++i) {
                if (hsa->table.state[i] != static_cast<std::uint8_t>(types::HashSlotState::Occupied)) continue;
                if (!map_find_linear(hsb->table, hsa->table.keys[i], *hash_key_equal)) return false;
            }
            return true;
        }

        auto* ra = heap.try_get_as<ObjectKind::Regex, types::Regex>(ops::payload(a));
        auto* rb = heap.try_get_as<ObjectKind::Regex, types::Regex>(ops::payload(b));
        if (ra && rb) return ra->pattern == rb->pattern && ra->flags == rb->flags;

        return false;
    };

    *equal_impl = [&, hash_key_equal, equal_impl, map_find_linear](LispVal a, LispVal b) -> bool {
        if (a == b) return true;
        if (!ops::is_boxed(a) || !ops::is_boxed(b)) return false;

        if (ops::tag(a) == Tag::String && ops::tag(b) == Tag::String) {
            auto sa = StringView::try_from(a, intern_table);
            auto sb = StringView::try_from(b, intern_table);
            return sa && sb && sa->view() == sb->view();
        }

        auto na = classify_numeric(a, heap);
        auto nb = classify_numeric(b, heap);
        if (na.is_valid() && nb.is_valid()) {
            if (na.is_flonum() || nb.is_flonum()) return na.as_double() == nb.as_double();
            return na.int_val == nb.int_val;
        }

        if (ops::tag(a) != Tag::HeapObject || ops::tag(b) != Tag::HeapObject) return false;

        auto* ca = heap.try_get_as<ObjectKind::Cons, types::Cons>(ops::payload(a));
        auto* cb = heap.try_get_as<ObjectKind::Cons, types::Cons>(ops::payload(b));
        if (ca && cb) return (*equal_impl)(ca->car, cb->car) && (*equal_impl)(ca->cdr, cb->cdr);

        auto* va = heap.try_get_as<ObjectKind::Vector, types::Vector>(ops::payload(a));
        auto* vb = heap.try_get_as<ObjectKind::Vector, types::Vector>(ops::payload(b));
        if (va && vb) {
            if (va->elements.size() != vb->elements.size()) return false;
            for (std::size_t i = 0; i < va->elements.size(); ++i) {
                if (!(*equal_impl)(va->elements[i], vb->elements[i])) return false;
            }
            return true;
        }

        auto* bva = heap.try_get_as<ObjectKind::ByteVector, types::ByteVector>(ops::payload(a));
        auto* bvb = heap.try_get_as<ObjectKind::ByteVector, types::ByteVector>(ops::payload(b));
        if (bva && bvb) return bva->data == bvb->data;

        auto* hma = heap.try_get_as<ObjectKind::HashMap, types::HashMap>(ops::payload(a));
        auto* hmb = heap.try_get_as<ObjectKind::HashMap, types::HashMap>(ops::payload(b));
        if (hma && hmb) {
            if (hma->size != hmb->size) return false;
            for (std::size_t i = 0; i < hma->state.size(); ++i) {
                if (hma->state[i] != static_cast<std::uint8_t>(types::HashSlotState::Occupied)) continue;
                auto hit = map_find_linear(*hmb, hma->keys[i], *hash_key_equal);
                if (!hit) return false;
                if (!(*equal_impl)(hma->values[i], hmb->values[*hit])) return false;
            }
            return true;
        }

        auto* hsa = heap.try_get_as<ObjectKind::HashSet, types::HashSet>(ops::payload(a));
        auto* hsb = heap.try_get_as<ObjectKind::HashSet, types::HashSet>(ops::payload(b));
        if (hsa && hsb) {
            if (hsa->table.size != hsb->table.size) return false;
            for (std::size_t i = 0; i < hsa->table.state.size(); ++i) {
                if (hsa->table.state[i] != static_cast<std::uint8_t>(types::HashSlotState::Occupied)) continue;
                if (!map_find_linear(hsb->table, hsa->table.keys[i], *hash_key_equal)) return false;
            }
            return true;
        }

        auto* ra = heap.try_get_as<ObjectKind::Regex, types::Regex>(ops::payload(a));
        auto* rb = heap.try_get_as<ObjectKind::Regex, types::Regex>(ops::payload(b));
        if (ra && rb) return ra->pattern == rb->pattern && ra->flags == rb->flags;

        return false;
    };

    env.register_builtin("equal?", 2, false, [equal_impl](Args args) -> std::expected<LispVal, RuntimeError> {
        return (*equal_impl)(args[0], args[1]) ? nanbox::True : nanbox::False;
    });
}

void PrimReg::register_sequences_collections_and_atoms_bridge() {
    using Args = PrimReg::Args;
    auto& env = this->env;
    auto& heap = this->heap;
    auto& intern_table = this->intern_table;
    auto* vm = this->vm;

    auto splitmix64 = [](std::uint64_t x) -> std::uint64_t {
        x += 0x9e3779b97f4a7c15ULL;
        x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
        x = (x ^ (x >> 27)) * 0x94d049bb133111ebULL;
        return x ^ (x >> 31);
    };

    static const std::array<std::uint64_t, 2> hash_seed = []() {
        std::random_device rd;
        const std::uint64_t s0 =
            (static_cast<std::uint64_t>(rd()) << 32) ^ static_cast<std::uint64_t>(rd());
        std::uint64_t s1 =
            (static_cast<std::uint64_t>(rd()) << 32) ^ static_cast<std::uint64_t>(rd());
        if (s0 == 0 && s1 == 0) s1 = 1;
        return std::array<std::uint64_t, 2>{s0, s1};
    }();

    auto hash_key_equal = std::make_shared<std::function<bool(LispVal, LispVal)>>();

    auto map_find_linear = [](const types::HashMap& map,
                              const LispVal key,
                              const std::function<bool(LispVal, LispVal)>& eq) -> std::optional<std::size_t> {
        for (std::size_t i = 0; i < map.state.size(); ++i) {
            if (map.state[i] != static_cast<std::uint8_t>(types::HashSlotState::Occupied)) continue;
            if (eq(map.keys[i], key)) return i;
        }
        return std::nullopt;
    };

    *hash_key_equal = [&, hash_key_equal, map_find_linear](LispVal a, LispVal b) -> bool {
        if (a == b) return true;
        if (!ops::is_boxed(a) || !ops::is_boxed(b)) return false;

        if (ops::tag(a) == Tag::String && ops::tag(b) == Tag::String) {
            auto sa = StringView::try_from(a, intern_table);
            auto sb = StringView::try_from(b, intern_table);
            return sa && sb && sa->view() == sb->view();
        }

        auto na = classify_numeric(a, heap);
        auto nb = classify_numeric(b, heap);
        if (na.is_valid() && nb.is_valid()) {
            if (na.is_fixnum() != nb.is_fixnum()) return false;
            if (na.is_flonum()) return na.float_val == nb.float_val;
            return na.int_val == nb.int_val;
        }

        if (ops::tag(a) != Tag::HeapObject || ops::tag(b) != Tag::HeapObject) return false;

        auto* ca = heap.try_get_as<ObjectKind::Cons, types::Cons>(ops::payload(a));
        auto* cb = heap.try_get_as<ObjectKind::Cons, types::Cons>(ops::payload(b));
        if (ca && cb) return (*hash_key_equal)(ca->car, cb->car) && (*hash_key_equal)(ca->cdr, cb->cdr);

        auto* va = heap.try_get_as<ObjectKind::Vector, types::Vector>(ops::payload(a));
        auto* vb = heap.try_get_as<ObjectKind::Vector, types::Vector>(ops::payload(b));
        if (va && vb) {
            if (va->elements.size() != vb->elements.size()) return false;
            for (std::size_t i = 0; i < va->elements.size(); ++i) {
                if (!(*hash_key_equal)(va->elements[i], vb->elements[i])) return false;
            }
            return true;
        }

        auto* bva = heap.try_get_as<ObjectKind::ByteVector, types::ByteVector>(ops::payload(a));
        auto* bvb = heap.try_get_as<ObjectKind::ByteVector, types::ByteVector>(ops::payload(b));
        if (bva && bvb) return bva->data == bvb->data;

        auto* hma = heap.try_get_as<ObjectKind::HashMap, types::HashMap>(ops::payload(a));
        auto* hmb = heap.try_get_as<ObjectKind::HashMap, types::HashMap>(ops::payload(b));
        if (hma && hmb) {
            if (hma->size != hmb->size) return false;
            for (std::size_t i = 0; i < hma->state.size(); ++i) {
                if (hma->state[i] != static_cast<std::uint8_t>(types::HashSlotState::Occupied)) continue;
                auto hit = map_find_linear(*hmb, hma->keys[i], *hash_key_equal);
                if (!hit) return false;
                if (!(*hash_key_equal)(hma->values[i], hmb->values[*hit])) return false;
            }
            return true;
        }

        auto* hsa = heap.try_get_as<ObjectKind::HashSet, types::HashSet>(ops::payload(a));
        auto* hsb = heap.try_get_as<ObjectKind::HashSet, types::HashSet>(ops::payload(b));
        if (hsa && hsb) {
            if (hsa->table.size != hsb->table.size) return false;
            for (std::size_t i = 0; i < hsa->table.state.size(); ++i) {
                if (hsa->table.state[i] != static_cast<std::uint8_t>(types::HashSlotState::Occupied)) continue;
                if (!map_find_linear(hsb->table, hsa->table.keys[i], *hash_key_equal)) return false;
            }
            return true;
        }

        auto* ra = heap.try_get_as<ObjectKind::Regex, types::Regex>(ops::payload(a));
        auto* rb = heap.try_get_as<ObjectKind::Regex, types::Regex>(ops::payload(b));
        if (ra && rb) return ra->pattern == rb->pattern && ra->flags == rb->flags;

        return false;
    };

    auto hash_value_impl =
        std::make_shared<std::function<std::expected<std::uint64_t, RuntimeError>(
            LispVal, int, std::uint64_t, const char*)>>();
    *hash_value_impl =
        [&heap, &intern_table, &splitmix64, hash_value_impl](
            const LispVal value,
            const int depth,
            const std::uint64_t seed,
            const char* op_name) -> std::expected<std::uint64_t, RuntimeError> {
            if (depth <= 0) {
                return splitmix64(static_cast<std::uint64_t>(value));
            }

            if (value == Nil) return splitmix64(0x4E494CULL);
            if (value == True) return splitmix64(0x54525545ULL);
            if (value == False) return splitmix64(0x46414C5345ULL);

            if (!ops::is_boxed(value)) {
                return splitmix64(std::bit_cast<std::uint64_t>(value));
            }

            const Tag t = ops::tag(value);
            if (t == Tag::Fixnum) {
                return splitmix64(static_cast<std::uint64_t>(ops::sign_extend_fixnum(ops::payload(value))));
            }
            if (t == Tag::Char) {
                auto ch = ops::decode<char32_t>(value).value_or(U'\0');
                return splitmix64(static_cast<std::uint64_t>(ch));
            }
            if (t == Tag::Symbol) {
                return splitmix64(static_cast<std::uint64_t>(ops::payload(value)));
            }
            if (t == Tag::String) {
                auto sv = intern_table.get_string(ops::payload(value));
                if (!sv) {
                    return std::unexpected(RuntimeError{VMError{
                        RuntimeErrorCode::TypeError,
                        std::string(op_name) + ": invalid string value"}});
                }
                const auto* bytes = reinterpret_cast<const std::uint8_t*>(sv->data());
                return util::siphash24(bytes, sv->size(), seed, splitmix64(seed ^ 0xA5A5A5A5A5A5A5A5ULL));
            }
            if (t == Tag::Nan) {
                return splitmix64(0x7FF8000000000000ULL);
            }
            if (t != Tag::HeapObject) {
                return std::unexpected(RuntimeError{VMError{
                    RuntimeErrorCode::TypeError,
                    std::string(op_name) + ": unhashable value"}});
            }

            const auto id = ops::payload(value);
            const auto n = classify_numeric(value, heap);
            if (n.is_fixnum()) return splitmix64(static_cast<std::uint64_t>(n.int_val));
            if (n.is_flonum()) return splitmix64(std::bit_cast<std::uint64_t>(n.float_val));

            if (auto* bv = heap.try_get_as<ObjectKind::ByteVector, types::ByteVector>(id)) {
                return util::siphash24(bv->data.data(), bv->data.size(), seed, splitmix64(seed ^ 0x5A5A5A5A5A5A5A5AULL));
            }
            if (auto* cons = heap.try_get_as<ObjectKind::Cons, types::Cons>(id)) {
                auto car_hash = (*hash_value_impl)(cons->car, depth - 1, seed, op_name);
                if (!car_hash) return std::unexpected(car_hash.error());
                auto cdr_hash = (*hash_value_impl)(cons->cdr, depth - 1, seed, op_name);
                if (!cdr_hash) return std::unexpected(cdr_hash.error());
                return splitmix64(*car_hash ^ std::rotl(*cdr_hash, 7) ^ 0xC0DEC0DEULL);
            }
            if (auto* vec = heap.try_get_as<ObjectKind::Vector, types::Vector>(id)) {
                std::uint64_t acc = splitmix64(0x766563ULL ^ static_cast<std::uint64_t>(vec->elements.size()));
                for (auto elem : vec->elements) {
                    auto elem_hash = (*hash_value_impl)(elem, depth - 1, seed, op_name);
                    if (!elem_hash) return std::unexpected(elem_hash.error());
                    acc = splitmix64(acc ^ *elem_hash);
                }
                return acc;
            }
            if (auto* hm = heap.try_get_as<ObjectKind::HashMap, types::HashMap>(id)) {
                std::uint64_t acc = splitmix64(0x6d6170ULL ^ static_cast<std::uint64_t>(hm->size));
                for (std::size_t i = 0; i < hm->state.size(); ++i) {
                    if (hm->state[i] != static_cast<std::uint8_t>(types::HashSlotState::Occupied)) continue;
                    auto kh = (*hash_value_impl)(hm->keys[i], depth - 1, seed, op_name);
                    if (!kh) return std::unexpected(kh.error());
                    auto vh = (*hash_value_impl)(hm->values[i], depth - 1, seed, op_name);
                    if (!vh) return std::unexpected(vh.error());
                    acc ^= splitmix64(*kh ^ std::rotl(*vh, 13));
                }
                return acc;
            }
            if (auto* hs = heap.try_get_as<ObjectKind::HashSet, types::HashSet>(id)) {
                std::uint64_t acc = splitmix64(0x736574ULL ^ static_cast<std::uint64_t>(hs->table.size));
                for (std::size_t i = 0; i < hs->table.state.size(); ++i) {
                    if (hs->table.state[i] != static_cast<std::uint8_t>(types::HashSlotState::Occupied)) continue;
                    auto kh = (*hash_value_impl)(hs->table.keys[i], depth - 1, seed, op_name);
                    if (!kh) return std::unexpected(kh.error());
                    acc ^= splitmix64(*kh);
                }
                return acc;
            }
            if (auto* ct = heap.try_get_as<ObjectKind::CompoundTerm, types::CompoundTerm>(id)) {
                auto functor_hash = (*hash_value_impl)(ct->functor, depth - 1, seed, op_name);
                if (!functor_hash) return std::unexpected(functor_hash.error());
                std::uint64_t acc = splitmix64(*functor_hash ^ static_cast<std::uint64_t>(ct->args.size()));
                for (auto arg : ct->args) {
                    auto ah = (*hash_value_impl)(arg, depth - 1, seed, op_name);
                    if (!ah) return std::unexpected(ah.error());
                    acc = splitmix64(acc ^ *ah);
                }
                return acc;
            }

            return std::unexpected(RuntimeError{VMError{
                RuntimeErrorCode::TypeError,
                std::string(op_name) + ": unhashable value"}});
        };

    auto hash_key = [hash_value_impl](const LispVal key, const std::uint64_t seed, const char* op_name)
        -> std::expected<std::uint64_t, RuntimeError> {
        return (*hash_value_impl)(key, 8, seed, op_name);
    };

    auto map_capacity_for_size = [](const std::size_t count) -> std::size_t {
        std::size_t cap = 8;
        while (count * 10 > cap * 7) cap <<= 1;
        return cap;
    };

    auto map_find_index_by_hash = [hash_key_equal](const types::HashMap& map,
                                                    const LispVal key,
                                                    const std::uint64_t key_hash) -> std::optional<std::size_t> {
        const auto cap = map.state.size();
        if (cap == 0) return std::nullopt;
        std::size_t idx = static_cast<std::size_t>(key_hash & (cap - 1));
        for (std::size_t probe = 0; probe < cap; ++probe) {
            const auto slot = map.state[idx];
            if (slot == static_cast<std::uint8_t>(types::HashSlotState::Empty)) return std::nullopt;
            if (slot == static_cast<std::uint8_t>(types::HashSlotState::Occupied)
                && (*hash_key_equal)(map.keys[idx], key)) {
                return idx;
            }
            idx = (idx + 1) & (cap - 1);
        }
        return std::nullopt;
    };

    auto map_insert_no_grow = [hash_key_equal](types::HashMap& map,
                                                const LispVal key,
                                                const LispVal value,
                                                const std::uint64_t key_hash) {
        const auto cap = map.state.size();
        std::size_t idx = static_cast<std::size_t>(key_hash & (cap - 1));
        std::optional<std::size_t> first_tombstone;

        for (std::size_t probe = 0; probe < cap; ++probe) {
            const auto slot = map.state[idx];
            if (slot == static_cast<std::uint8_t>(types::HashSlotState::Empty)) {
                const auto target = first_tombstone.value_or(idx);
                if (map.state[target] == static_cast<std::uint8_t>(types::HashSlotState::Tombstone)) {
                    --map.tombstones;
                }
                map.state[target] = static_cast<std::uint8_t>(types::HashSlotState::Occupied);
                map.keys[target] = key;
                map.values[target] = value;
                ++map.size;
                return;
            }

            if (slot == static_cast<std::uint8_t>(types::HashSlotState::Tombstone)) {
                if (!first_tombstone) first_tombstone = idx;
            } else if ((*hash_key_equal)(map.keys[idx], key)) {
                map.values[idx] = value;
                return;
            }

            idx = (idx + 1) & (cap - 1);
        }

        if (first_tombstone) {
            const auto target = *first_tombstone;
            map.state[target] = static_cast<std::uint8_t>(types::HashSlotState::Occupied);
            map.keys[target] = key;
            map.values[target] = value;
            --map.tombstones;
            ++map.size;
        }
    };

    auto map_collect_entries = [](const types::HashMap& map) -> std::vector<std::pair<LispVal, LispVal>> {
        std::vector<std::pair<LispVal, LispVal>> entries;
        entries.reserve(map.size);
        for (std::size_t i = 0; i < map.state.size(); ++i) {
            if (map.state[i] != static_cast<std::uint8_t>(types::HashSlotState::Occupied)) continue;
            entries.emplace_back(map.keys[i], map.values[i]);
        }
        return entries;
    };

    auto map_build_from_entries =
        [map_capacity_for_size, hash_key, map_insert_no_grow](
            const std::vector<std::pair<LispVal, LispVal>>& entries,
            const std::uint64_t seed,
            const char* op_name) -> std::expected<types::HashMap, RuntimeError> {
            types::HashMap out = types::make_empty_hash_map(map_capacity_for_size(entries.size()), seed);
            for (const auto& [k, v] : entries) {
                auto kh = hash_key(k, seed, op_name);
                if (!kh) return std::unexpected(kh.error());
                map_insert_no_grow(out, k, v, *kh);
            }
            return out;
        };

    auto map_assoc_copy =
        [hash_key_equal, map_collect_entries, map_build_from_entries](
            const types::HashMap& src,
            const LispVal key,
            const LispVal value,
            const char* op_name) -> std::expected<types::HashMap, RuntimeError> {
            auto entries = map_collect_entries(src);
            bool replaced = false;
            for (auto& entry : entries) {
                if (!(*hash_key_equal)(entry.first, key)) continue;
                entry.second = value;
                replaced = true;
                break;
            }
            if (!replaced) entries.emplace_back(key, value);
            return map_build_from_entries(entries, src.seed, op_name);
        };

    auto map_dissoc_copy =
        [hash_key_equal, map_collect_entries, map_build_from_entries](
            const types::HashMap& src,
            const LispVal key,
            const char* op_name) -> std::expected<types::HashMap, RuntimeError> {
            auto entries = map_collect_entries(src);
            const auto old_size = entries.size();
            entries.erase(
                std::remove_if(entries.begin(), entries.end(),
                               [hash_key_equal, key](const auto& entry) { return (*hash_key_equal)(entry.first, key); }),
                entries.end());
            if (entries.size() == old_size) return src;
            return map_build_from_entries(entries, src.seed, op_name);
        };

    /**
     * Vector operations: vector vector-length vector-ref vector-set!
     */

    env.register_builtin("vector", 0, true, [&heap](Args args) -> std::expected<LispVal, RuntimeError> {
        std::vector<LispVal> elems(args.begin(), args.end());
        return make_vector(heap, std::move(elems));
    });

    env.register_builtin("vector-length", 1, false, [&heap](Args args) -> std::expected<LispVal, RuntimeError> {
        if (!ops::is_boxed(args[0]) || ops::tag(args[0]) != Tag::HeapObject)
            return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError, "vector-length: not a vector"}});
        auto* vec = heap.try_get_as<ObjectKind::Vector, types::Vector>(ops::payload(args[0]));
        if (!vec) return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError, "vector-length: not a vector"}});
        return make_fixnum(heap, static_cast<int64_t>(vec->elements.size()));
    });

    env.register_builtin("vector-ref", 2, false, [&heap](Args args) -> std::expected<LispVal, RuntimeError> {
        if (!ops::is_boxed(args[0]) || ops::tag(args[0]) != Tag::HeapObject)
            return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError, "vector-ref: not a vector"}});
        auto* vec = heap.try_get_as<ObjectKind::Vector, types::Vector>(ops::payload(args[0]));
        if (!vec) return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError, "vector-ref: not a vector"}});
        auto idx = classify_numeric(args[1], heap);
        if (!idx.is_valid() || idx.is_flonum() || idx.int_val < 0 || static_cast<size_t>(idx.int_val) >= vec->elements.size())
            return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError, "vector-ref: index out of bounds"}});
        return vec->elements[static_cast<size_t>(idx.int_val)];
    });

    env.register_builtin("vector-set!", 3, false, [&heap](Args args) -> std::expected<LispVal, RuntimeError> {
        if (!ops::is_boxed(args[0]) || ops::tag(args[0]) != Tag::HeapObject)
            return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError, "vector-set!: not a vector"}});
        auto* vec = heap.try_get_as<ObjectKind::Vector, types::Vector>(ops::payload(args[0]));
        if (!vec) return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError, "vector-set!: not a vector"}});
        auto idx = classify_numeric(args[1], heap);
        if (!idx.is_valid() || idx.is_flonum() || idx.int_val < 0 || static_cast<size_t>(idx.int_val) >= vec->elements.size())
            return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError, "vector-set!: index out of bounds"}});
        vec->elements[static_cast<size_t>(idx.int_val)] = args[2];
        return nanbox::Nil;
    });

    env.register_builtin("vector?", 1, false, [&heap](Args args) -> std::expected<LispVal, RuntimeError> {
        if (!ops::is_boxed(args[0]) || ops::tag(args[0]) != Tag::HeapObject) return nanbox::False;
        return heap.try_get_as<ObjectKind::Vector, types::Vector>(ops::payload(args[0])) ? nanbox::True : nanbox::False;
    });

    env.register_builtin("make-vector", 2, false, [&heap](Args args) -> std::expected<LispVal, RuntimeError> {
        auto len = classify_numeric(args[0], heap);
        if (!len.is_valid() || len.is_flonum() || len.int_val < 0)
            return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError, "make-vector: length must be a non-negative integer"}});
        std::vector<LispVal> elems(static_cast<size_t>(len.int_val), args[1]);
        return make_vector(heap, std::move(elems));
    });

    auto expect_hash_map = [&heap](const LispVal value, const char* op_name)
        -> std::expected<types::HashMap*, RuntimeError> {
        if (!ops::is_boxed(value) || ops::tag(value) != Tag::HeapObject) {
            return std::unexpected(RuntimeError{VMError{
                RuntimeErrorCode::TypeError, std::string(op_name) + ": expected a hash-map"}});
        }
        auto* map = heap.try_get_as<ObjectKind::HashMap, types::HashMap>(ops::payload(value));
        if (!map) {
            return std::unexpected(RuntimeError{VMError{
                RuntimeErrorCode::TypeError, std::string(op_name) + ": expected a hash-map"}});
        }
        return map;
    };

    auto expect_hash_set = [&heap](const LispVal value, const char* op_name)
        -> std::expected<types::HashSet*, RuntimeError> {
        if (!ops::is_boxed(value) || ops::tag(value) != Tag::HeapObject) {
            return std::unexpected(RuntimeError{VMError{
                RuntimeErrorCode::TypeError, std::string(op_name) + ": expected a hash-set"}});
        }
        auto* set = heap.try_get_as<ObjectKind::HashSet, types::HashSet>(ops::payload(value));
        if (!set) {
            return std::unexpected(RuntimeError{VMError{
                RuntimeErrorCode::TypeError, std::string(op_name) + ": expected a hash-set"}});
        }
        return set;
    };

    auto expect_atom = [&heap](const LispVal value, const char* op_name)
        -> std::expected<types::Atom*, RuntimeError> {
        if (!ops::is_boxed(value) || ops::tag(value) != Tag::HeapObject) {
            return std::unexpected(RuntimeError{VMError{
                RuntimeErrorCode::TypeError, std::string(op_name) + ": expected an atom"}});
        }
        auto* atom = heap.try_get_as<ObjectKind::Atom, types::Atom>(ops::payload(value));
        if (!atom) {
            return std::unexpected(RuntimeError{VMError{
                RuntimeErrorCode::TypeError, std::string(op_name) + ": expected an atom"}});
        }
        return atom;
    };

    auto list_to_elements = [&heap](const LispVal list, const char* op_name)
        -> std::expected<std::vector<LispVal>, RuntimeError> {
        std::vector<LispVal> out;
        LispVal cur = list;
        while (cur != Nil) {
            if (!ops::is_boxed(cur) || ops::tag(cur) != Tag::HeapObject) {
                return std::unexpected(RuntimeError{VMError{
                    RuntimeErrorCode::TypeError, std::string(op_name) + ": expected a proper list"}});
            }
            auto* cell = heap.try_get_as<ObjectKind::Cons, types::Cons>(ops::payload(cur));
            if (!cell) {
                return std::unexpected(RuntimeError{VMError{
                    RuntimeErrorCode::TypeError, std::string(op_name) + ": expected a proper list"}});
            }
            out.push_back(cell->car);
            cur = cell->cdr;
        }
        return out;
    };

    auto decode_alist_pair = [&heap](const LispVal row, LispVal& key, LispVal& value) -> bool {
        if (!ops::is_boxed(row) || ops::tag(row) != Tag::HeapObject) return false;
        auto* pair = heap.try_get_as<ObjectKind::Cons, types::Cons>(ops::payload(row));
        if (!pair) return false;

        key = pair->car;
        if (pair->cdr == Nil) return false;

        if (ops::is_boxed(pair->cdr) && ops::tag(pair->cdr) == Tag::HeapObject) {
            if (auto* as_list = heap.try_get_as<ObjectKind::Cons, types::Cons>(ops::payload(pair->cdr))) {
                if (as_list->cdr == Nil) {
                    value = as_list->car;
                    return true;
                }
            }
        }

        value = pair->cdr;
        return true;
    };

    auto make_map_from_kv_args =
        [map_build_from_entries](Args args,
                                  const std::size_t start,
                                  const std::uint64_t seed,
                                  const char* op_name) -> std::expected<types::HashMap, RuntimeError> {
            const auto count = args.size() - start;
            if ((count % 2) != 0) {
                return std::unexpected(RuntimeError{VMError{
                    RuntimeErrorCode::InvalidArity,
                    std::string(op_name) + ": expected an even number of key/value arguments"}});
            }
            std::vector<std::pair<LispVal, LispVal>> entries;
            entries.reserve(count / 2);
            for (std::size_t i = start; i + 1 < args.size(); i += 2) {
                entries.emplace_back(args[i], args[i + 1]);
            }
            return map_build_from_entries(entries, seed, op_name);
        };

    auto make_set_from_values =
        [map_capacity_for_size, hash_key, map_insert_no_grow](
            Args args,
            const std::size_t start,
            const std::uint64_t seed,
            const char* op_name) -> std::expected<types::HashSet, RuntimeError> {
            types::HashMap table = types::make_empty_hash_map(map_capacity_for_size(args.size() - start), seed);
            for (std::size_t i = start; i < args.size(); ++i) {
                auto kh = hash_key(args[i], seed, op_name);
                if (!kh) return std::unexpected(kh.error());
                map_insert_no_grow(table, args[i], True, *kh);
            }
            return types::HashSet{.table = std::move(table)};
        };

    env.register_builtin("hash-map", 0, true, [&heap, make_map_from_kv_args](Args args)
        -> std::expected<LispVal, RuntimeError> {
        auto built = make_map_from_kv_args(args, 0, hash_seed[0], "hash-map");
        if (!built) return std::unexpected(built.error());
        return make_hash_map(heap, std::move(*built));
    });

    env.register_builtin("make-hash-map", 0, false, [&heap](Args)
        -> std::expected<LispVal, RuntimeError> {
        return make_hash_map(heap, 8, hash_seed[0]);
    });

    env.register_builtin("hash-map?", 1, false, [&heap](Args args) -> std::expected<LispVal, RuntimeError> {
        if (!ops::is_boxed(args[0]) || ops::tag(args[0]) != Tag::HeapObject) return False;
        return heap.try_get_as<ObjectKind::HashMap, types::HashMap>(ops::payload(args[0])) ? True : False;
    });

    env.register_builtin("hash-map-ref", 2, true,
        [hash_key, expect_hash_map, map_find_index_by_hash](Args args) -> std::expected<LispVal, RuntimeError> {
            if (args.size() > 3) {
                return std::unexpected(RuntimeError{VMError{
                    RuntimeErrorCode::InvalidArity, "hash-map-ref: expected 2 or 3 arguments"}});
            }
            auto map_res = expect_hash_map(args[0], "hash-map-ref");
            if (!map_res) return std::unexpected(map_res.error());

            auto kh = hash_key(args[1], (*map_res)->seed, "hash-map-ref");
            if (!kh) return std::unexpected(kh.error());
            auto hit = map_find_index_by_hash(**map_res, args[1], *kh);
            if (hit) return (*map_res)->values[*hit];

            if (args.size() == 3) return args[2];
            return std::unexpected(RuntimeError{VMError{
                RuntimeErrorCode::UserError, "hash-map-ref: key not found"}});
        });

    env.register_builtin("hash-map-assoc", 3, false,
        [&heap, expect_hash_map, map_assoc_copy](Args args) -> std::expected<LispVal, RuntimeError> {
            auto map_res = expect_hash_map(args[0], "hash-map-assoc");
            if (!map_res) return std::unexpected(map_res.error());
            auto next = map_assoc_copy(**map_res, args[1], args[2], "hash-map-assoc");
            if (!next) return std::unexpected(next.error());
            return make_hash_map(heap, std::move(*next));
        });

    env.register_builtin("hash-map-dissoc", 2, false,
        [&heap, hash_key, expect_hash_map, map_find_index_by_hash, map_dissoc_copy](Args args)
            -> std::expected<LispVal, RuntimeError> {
            auto map_res = expect_hash_map(args[0], "hash-map-dissoc");
            if (!map_res) return std::unexpected(map_res.error());

            auto kh = hash_key(args[1], (*map_res)->seed, "hash-map-dissoc");
            if (!kh) return std::unexpected(kh.error());
            if (!map_find_index_by_hash(**map_res, args[1], *kh)) return args[0];

            auto next = map_dissoc_copy(**map_res, args[1], "hash-map-dissoc");
            if (!next) return std::unexpected(next.error());
            return make_hash_map(heap, std::move(*next));
        });

    env.register_builtin("hash-map-keys", 1, false, [&heap, expect_hash_map](Args args)
        -> std::expected<LispVal, RuntimeError> {
        auto map_res = expect_hash_map(args[0], "hash-map-keys");
        if (!map_res) return std::unexpected(map_res.error());

        auto roots = heap.make_external_root_frame();
        LispVal out = Nil;
        roots.push(out);

        for (std::size_t i = (*map_res)->state.size(); i > 0; --i) {
            const auto slot = i - 1;
            if ((*map_res)->state[slot] != static_cast<std::uint8_t>(types::HashSlotState::Occupied)) continue;
            auto cell = make_cons(heap, (*map_res)->keys[slot], out);
            if (!cell) return std::unexpected(cell.error());
            out = *cell;
            roots.push(out);
        }

        return out;
    });

    env.register_builtin("hash-map-values", 1, false, [&heap, expect_hash_map](Args args)
        -> std::expected<LispVal, RuntimeError> {
        auto map_res = expect_hash_map(args[0], "hash-map-values");
        if (!map_res) return std::unexpected(map_res.error());

        auto roots = heap.make_external_root_frame();
        LispVal out = Nil;
        roots.push(out);

        for (std::size_t i = (*map_res)->state.size(); i > 0; --i) {
            const auto slot = i - 1;
            if ((*map_res)->state[slot] != static_cast<std::uint8_t>(types::HashSlotState::Occupied)) continue;
            auto cell = make_cons(heap, (*map_res)->values[slot], out);
            if (!cell) return std::unexpected(cell.error());
            out = *cell;
            roots.push(out);
        }

        return out;
    });

    env.register_builtin("hash-map-size", 1, false, [&heap, expect_hash_map](Args args)
        -> std::expected<LispVal, RuntimeError> {
        auto map_res = expect_hash_map(args[0], "hash-map-size");
        if (!map_res) return std::unexpected(map_res.error());
        return make_fixnum(heap, static_cast<std::int64_t>((*map_res)->size));
    });

    env.register_builtin("hash-map->list", 1, false, [&heap, expect_hash_map](Args args)
        -> std::expected<LispVal, RuntimeError> {
        auto map_res = expect_hash_map(args[0], "hash-map->list");
        if (!map_res) return std::unexpected(map_res.error());

        auto roots = heap.make_external_root_frame();
        LispVal out = Nil;
        roots.push(out);

        for (std::size_t i = (*map_res)->state.size(); i > 0; --i) {
            const auto slot = i - 1;
            if ((*map_res)->state[slot] != static_cast<std::uint8_t>(types::HashSlotState::Occupied)) continue;

            auto pair = make_cons(heap, (*map_res)->keys[slot], (*map_res)->values[slot]);
            if (!pair) return std::unexpected(pair.error());
            roots.push(*pair);

            auto cell = make_cons(heap, *pair, out);
            if (!cell) return std::unexpected(cell.error());
            out = *cell;
            roots.push(out);
        }

        return out;
    });

    env.register_builtin("list->hash-map", 1, false,
        [&heap, list_to_elements, decode_alist_pair, map_build_from_entries](Args args)
            -> std::expected<LispVal, RuntimeError> {
            auto elems = list_to_elements(args[0], "list->hash-map");
            if (!elems) return std::unexpected(elems.error());

            std::vector<std::pair<LispVal, LispVal>> entries;
            entries.reserve(elems->size());

            bool all_alist_pairs = true;
            for (auto elem : *elems) {
                LispVal k = Nil;
                LispVal v = Nil;
                if (!decode_alist_pair(elem, k, v)) {
                    all_alist_pairs = false;
                    break;
                }
                entries.emplace_back(k, v);
            }

            if (!all_alist_pairs) {
                entries.clear();
                if ((elems->size() % 2) != 0) {
                    return std::unexpected(RuntimeError{VMError{
                        RuntimeErrorCode::InvalidArity,
                        "list->hash-map: flat input must contain key/value pairs"}});
                }
                entries.reserve(elems->size() / 2);
                for (std::size_t i = 0; i + 1 < elems->size(); i += 2) {
                    entries.emplace_back((*elems)[i], (*elems)[i + 1]);
                }
            }

            auto map = map_build_from_entries(entries, hash_seed[0], "list->hash-map");
            if (!map) return std::unexpected(map.error());
            return make_hash_map(heap, std::move(*map));
        });

    env.register_builtin("hash-map-fold", 3, false,
        [&heap, vm, expect_hash_map](Args args) -> std::expected<LispVal, RuntimeError> {
            auto map_res = expect_hash_map(args[2], "hash-map-fold");
            if (!map_res) return std::unexpected(map_res.error());

            const LispVal fn = args[0];
            if (!ops::is_boxed(fn) || ops::tag(fn) != Tag::HeapObject) {
                return std::unexpected(RuntimeError{VMError{
                    RuntimeErrorCode::TypeError, "hash-map-fold: first argument must be a procedure"}});
            }

            LispVal acc = args[1];
            for (std::size_t i = 0; i < (*map_res)->state.size(); ++i) {
                if ((*map_res)->state[i] != static_cast<std::uint8_t>(types::HashSlotState::Occupied)) continue;

                std::expected<LispVal, RuntimeError> call_result;
                if (vm) {
                    call_result = vm->call_value(fn, {acc, (*map_res)->keys[i], (*map_res)->values[i]});
                } else {
                    auto* prim = heap.try_get_as<ObjectKind::Primitive, types::Primitive>(ops::payload(fn));
                    if (!prim) {
                        return std::unexpected(RuntimeError{VMError{
                            RuntimeErrorCode::TypeError,
                            "hash-map-fold: closures require a VM context"}});
                    }
                    std::vector<LispVal> call_args = {acc, (*map_res)->keys[i], (*map_res)->values[i]};
                    call_result = prim->func(call_args);
                }
                if (!call_result) return std::unexpected(call_result.error());
                acc = *call_result;
            }

            return acc;
        });

    env.register_builtin("hash", 1, false,
        [hash_value_impl, &heap](Args args) -> std::expected<LispVal, RuntimeError> {
            auto hv = (*hash_value_impl)(args[0], 8, hash_seed[0], "hash");
            if (!hv) return std::unexpected(hv.error());
            constexpr std::uint64_t mask = (1ULL << 46) - 1ULL;
            return make_fixnum(heap, static_cast<std::int64_t>(*hv & mask));
        });

    env.register_builtin("make-hash-set", 0, false, [&heap](Args)
        -> std::expected<LispVal, RuntimeError> {
        return make_hash_set(heap, 8, hash_seed[0]);
    });

    env.register_builtin("hash-set", 0, true, [&heap, make_set_from_values](Args args)
        -> std::expected<LispVal, RuntimeError> {
        auto set = make_set_from_values(args, 0, hash_seed[0], "hash-set");
        if (!set) return std::unexpected(set.error());
        return make_hash_set(heap, std::move(*set));
    });

    env.register_builtin("hash-set?", 1, false, [&heap](Args args) -> std::expected<LispVal, RuntimeError> {
        if (!ops::is_boxed(args[0]) || ops::tag(args[0]) != Tag::HeapObject) return False;
        return heap.try_get_as<ObjectKind::HashSet, types::HashSet>(ops::payload(args[0])) ? True : False;
    });

    env.register_builtin("hash-set-add", 2, false,
        [&heap, hash_key, expect_hash_set, map_find_index_by_hash, map_assoc_copy](Args args)
            -> std::expected<LispVal, RuntimeError> {
            auto set_res = expect_hash_set(args[0], "hash-set-add");
            if (!set_res) return std::unexpected(set_res.error());

            auto kh = hash_key(args[1], (*set_res)->table.seed, "hash-set-add");
            if (!kh) return std::unexpected(kh.error());
            if (map_find_index_by_hash((*set_res)->table, args[1], *kh)) return args[0];

            auto table = map_assoc_copy((*set_res)->table, args[1], True, "hash-set-add");
            if (!table) return std::unexpected(table.error());
            return make_hash_set(heap, types::HashSet{.table = std::move(*table)});
        });

    env.register_builtin("hash-set-remove", 2, false,
        [&heap, hash_key, expect_hash_set, map_find_index_by_hash, map_dissoc_copy](Args args)
            -> std::expected<LispVal, RuntimeError> {
            auto set_res = expect_hash_set(args[0], "hash-set-remove");
            if (!set_res) return std::unexpected(set_res.error());

            auto kh = hash_key(args[1], (*set_res)->table.seed, "hash-set-remove");
            if (!kh) return std::unexpected(kh.error());
            if (!map_find_index_by_hash((*set_res)->table, args[1], *kh)) return args[0];

            auto table = map_dissoc_copy((*set_res)->table, args[1], "hash-set-remove");
            if (!table) return std::unexpected(table.error());
            return make_hash_set(heap, types::HashSet{.table = std::move(*table)});
        });

    env.register_builtin("hash-set-contains?", 2, false,
        [hash_key, expect_hash_set, map_find_index_by_hash](Args args) -> std::expected<LispVal, RuntimeError> {
            auto set_res = expect_hash_set(args[0], "hash-set-contains?");
            if (!set_res) return std::unexpected(set_res.error());

            auto kh = hash_key(args[1], (*set_res)->table.seed, "hash-set-contains?");
            if (!kh) return std::unexpected(kh.error());
            return map_find_index_by_hash((*set_res)->table, args[1], *kh) ? True : False;
        });

    env.register_builtin("hash-set-union", 2, false,
        [&heap, expect_hash_set, map_build_from_entries](Args args) -> std::expected<LispVal, RuntimeError> {
            auto a = expect_hash_set(args[0], "hash-set-union");
            if (!a) return std::unexpected(a.error());
            auto b = expect_hash_set(args[1], "hash-set-union");
            if (!b) return std::unexpected(b.error());

            std::vector<std::pair<LispVal, LispVal>> entries;
            entries.reserve((*a)->table.size + (*b)->table.size);
            for (std::size_t i = 0; i < (*a)->table.state.size(); ++i) {
                if ((*a)->table.state[i] != static_cast<std::uint8_t>(types::HashSlotState::Occupied)) continue;
                entries.emplace_back((*a)->table.keys[i], True);
            }
            for (std::size_t i = 0; i < (*b)->table.state.size(); ++i) {
                if ((*b)->table.state[i] != static_cast<std::uint8_t>(types::HashSlotState::Occupied)) continue;
                entries.emplace_back((*b)->table.keys[i], True);
            }

            auto table = map_build_from_entries(entries, (*a)->table.seed, "hash-set-union");
            if (!table) return std::unexpected(table.error());
            return make_hash_set(heap, types::HashSet{.table = std::move(*table)});
        });

    env.register_builtin("hash-set-intersect", 2, false,
        [&heap, hash_key, expect_hash_set, map_find_index_by_hash, map_build_from_entries](Args args)
            -> std::expected<LispVal, RuntimeError> {
            auto a = expect_hash_set(args[0], "hash-set-intersect");
            if (!a) return std::unexpected(a.error());
            auto b = expect_hash_set(args[1], "hash-set-intersect");
            if (!b) return std::unexpected(b.error());

            std::vector<std::pair<LispVal, LispVal>> entries;
            entries.reserve((*a)->table.size);
            for (std::size_t i = 0; i < (*a)->table.state.size(); ++i) {
                if ((*a)->table.state[i] != static_cast<std::uint8_t>(types::HashSlotState::Occupied)) continue;
                auto kh = hash_key((*a)->table.keys[i], (*b)->table.seed, "hash-set-intersect");
                if (!kh) return std::unexpected(kh.error());
                if (!map_find_index_by_hash((*b)->table, (*a)->table.keys[i], *kh)) continue;
                entries.emplace_back((*a)->table.keys[i], True);
            }

            auto table = map_build_from_entries(entries, (*a)->table.seed, "hash-set-intersect");
            if (!table) return std::unexpected(table.error());
            return make_hash_set(heap, types::HashSet{.table = std::move(*table)});
        });

    env.register_builtin("hash-set-diff", 2, false,
        [&heap, hash_key, expect_hash_set, map_find_index_by_hash, map_build_from_entries](Args args)
            -> std::expected<LispVal, RuntimeError> {
            auto a = expect_hash_set(args[0], "hash-set-diff");
            if (!a) return std::unexpected(a.error());
            auto b = expect_hash_set(args[1], "hash-set-diff");
            if (!b) return std::unexpected(b.error());

            std::vector<std::pair<LispVal, LispVal>> entries;
            entries.reserve((*a)->table.size);
            for (std::size_t i = 0; i < (*a)->table.state.size(); ++i) {
                if ((*a)->table.state[i] != static_cast<std::uint8_t>(types::HashSlotState::Occupied)) continue;
                auto kh = hash_key((*a)->table.keys[i], (*b)->table.seed, "hash-set-diff");
                if (!kh) return std::unexpected(kh.error());
                if (map_find_index_by_hash((*b)->table, (*a)->table.keys[i], *kh)) continue;
                entries.emplace_back((*a)->table.keys[i], True);
            }

            auto table = map_build_from_entries(entries, (*a)->table.seed, "hash-set-diff");
            if (!table) return std::unexpected(table.error());
            return make_hash_set(heap, types::HashSet{.table = std::move(*table)});
        });

    env.register_builtin("hash-set->list", 1, false, [&heap, expect_hash_set](Args args)
        -> std::expected<LispVal, RuntimeError> {
        auto set_res = expect_hash_set(args[0], "hash-set->list");
        if (!set_res) return std::unexpected(set_res.error());

        auto roots = heap.make_external_root_frame();
        LispVal out = Nil;
        roots.push(out);

        for (std::size_t i = (*set_res)->table.state.size(); i > 0; --i) {
            const auto slot = i - 1;
            if ((*set_res)->table.state[slot] != static_cast<std::uint8_t>(types::HashSlotState::Occupied)) continue;
            auto cell = make_cons(heap, (*set_res)->table.keys[slot], out);
            if (!cell) return std::unexpected(cell.error());
            out = *cell;
            roots.push(out);
        }

        return out;
    });

    env.register_builtin("list->hash-set", 1, false,
        [&heap, list_to_elements, map_build_from_entries](Args args)
            -> std::expected<LispVal, RuntimeError> {
            auto elems = list_to_elements(args[0], "list->hash-set");
            if (!elems) return std::unexpected(elems.error());

            std::vector<std::pair<LispVal, LispVal>> entries;
            entries.reserve(elems->size());
            for (auto elem : *elems) entries.emplace_back(elem, True);

            auto table = map_build_from_entries(entries, hash_seed[0], "list->hash-set");
            if (!table) return std::unexpected(table.error());
            return make_hash_set(heap, types::HashSet{.table = std::move(*table)});
        });

    env.register_builtin("%atom-new", 1, false, [&heap](Args args) -> std::expected<LispVal, RuntimeError> {
        return make_atom(heap, args[0]);
    });

    env.register_builtin("%atom?", 1, false, [&heap](Args args) -> std::expected<LispVal, RuntimeError> {
        if (!ops::is_boxed(args[0]) || ops::tag(args[0]) != Tag::HeapObject) return False;
        return heap.try_get_as<ObjectKind::Atom, types::Atom>(ops::payload(args[0])) ? True : False;
    });

    env.register_builtin("%atom-deref", 1, false,
        [expect_atom](Args args) -> std::expected<LispVal, RuntimeError> {
            auto atom_res = expect_atom(args[0], "%atom-deref");
            if (!atom_res) return std::unexpected(atom_res.error());
            return (*atom_res)->cell.load(std::memory_order_seq_cst);
        });

    env.register_builtin("%atom-reset!", 2, false,
        [expect_atom](Args args) -> std::expected<LispVal, RuntimeError> {
            auto atom_res = expect_atom(args[0], "%atom-reset!");
            if (!atom_res) return std::unexpected(atom_res.error());
            (*atom_res)->cell.store(args[1], std::memory_order_seq_cst);
            return args[1];
        });

    env.register_builtin("%atom-compare-and-set!", 3, false,
        [expect_atom](Args args) -> std::expected<LispVal, RuntimeError> {
            auto atom_res = expect_atom(args[0], "%atom-compare-and-set!");
            if (!atom_res) return std::unexpected(atom_res.error());

            LispVal expected = args[1];
            const bool swapped = (*atom_res)->cell.compare_exchange_strong(
                expected, args[2], std::memory_order_seq_cst, std::memory_order_seq_cst);
            return swapped ? True : False;
        });

    env.register_builtin("%atom-swap!", 2, true,
        [&heap, vm, expect_atom](Args args) -> std::expected<LispVal, RuntimeError> {
            auto atom_res = expect_atom(args[0], "%atom-swap!");
            if (!atom_res) return std::unexpected(atom_res.error());

            const LispVal callable = args[1];
            if (!ops::is_boxed(callable) || ops::tag(callable) != Tag::HeapObject) {
                return std::unexpected(RuntimeError{VMError{
                    RuntimeErrorCode::TypeError,
                    "%atom-swap!: second argument must be a procedure"}});
            }

            auto* primitive = heap.try_get_as<ObjectKind::Primitive, types::Primitive>(ops::payload(callable));
            if (!vm && !primitive) {
                return std::unexpected(RuntimeError{VMError{
                    RuntimeErrorCode::TypeError,
                    "%atom-swap!: closures require a VM context"}});
            }

            std::vector<LispVal> rest_args;
            rest_args.reserve(args.size() > 2 ? args.size() - 2 : 0);
            for (std::size_t i = 2; i < args.size(); ++i) {
                rest_args.push_back(args[i]);
            }

            while (true) {
                auto roots = heap.make_external_root_frame();
                roots.push(args[0]);
                roots.push(callable);
                for (auto value : rest_args) roots.push(value);

                const LispVal old_value = (*atom_res)->cell.load(std::memory_order_seq_cst);
                roots.push(old_value);

                std::vector<LispVal> call_args;
                call_args.reserve(1 + rest_args.size());
                call_args.push_back(old_value);
                call_args.insert(call_args.end(), rest_args.begin(), rest_args.end());
                for (auto value : call_args) roots.push(value);

                std::expected<LispVal, RuntimeError> call_result;
                if (vm) {
                    call_result = vm->call_value(callable, call_args);
                } else {
                    call_result = primitive->func(call_args);
                }
                if (!call_result) return std::unexpected(call_result.error());

                const LispVal new_value = *call_result;
                roots.push(new_value);

                LispVal expected = old_value;
                if ((*atom_res)->cell.compare_exchange_strong(
                    expected, new_value, std::memory_order_seq_cst, std::memory_order_seq_cst)) {
                    return new_value;
                }
            }
        });
}

} // namespace eta::runtime
