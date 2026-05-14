#include <functional>
#include <string>
#include <vector>

#include "eta/runtime/core_primitives_internal.h"
#include "eta/runtime/primitives/core_primitives_logic_helpers.h"
#include "eta/runtime/factory.h"
#include "eta/runtime/string_view.h"
#include "eta/runtime/vm/vm.h"

namespace eta::runtime {

void PrimReg::register_logic() {
    using Args = PrimReg::Args;
    auto& env = this->env;
    auto& heap = this->heap;
    auto& intern_table = this->intern_table;
    auto* vm = this->vm;

    using detail::core_primitives_logic::get_symbol_id;

    /**
     * Logic variable type predicate: logic-var?
     */

    env.register_builtin("logic-var?", 1, false, [&heap](Args args) -> std::expected<LispVal, RuntimeError> {
        const LispVal v = args[0];
        if (!ops::is_boxed(v) || ops::tag(v) != Tag::HeapObject) return False;
        auto id = ops::payload(v);
        return heap.try_get_as<ObjectKind::LogicVar, types::LogicVar>(id) ? True : False;
    });

    /**
     * Keep finalizer/guardian registrations in-slot between logic-var? and
     * attributed-variable builtins while their implementation remains in misc.
     */
    register_misc_lifecycle_bridge();

    /**
     * Attributed variables
     *                               at least one attribute
     * (register-attr-hook! 'module proc)
     *                               (proc var bound-value attr-value)
     *                               when `var` with attribute 'module is
     *                               bound by unify.  Returns #f on failure
     *                               (which unifies fails).  Hook registry
     *                               is VM-lifetime and NOT trailed.
     */

    env.register_builtin("put-attr", 3, false,
        [&heap, vm](Args args) -> std::expected<LispVal, RuntimeError> {
            const LispVal v = args[0];
            const LispVal mod = args[1];
            const LispVal val = args[2];
            if (!ops::is_boxed(v) || ops::tag(v) != Tag::HeapObject)
                return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError,
                    "put-attr: first arg must be a logic variable"}});
            auto* lv = heap.try_get_as<ObjectKind::LogicVar, types::LogicVar>(ops::payload(v));
            if (!lv)
                return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError,
                    "put-attr: first arg must be a logic variable"}});
            auto key = get_symbol_id(mod);
            if (!key)
                return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError,
                    "put-attr: second arg must be a symbol (module name)"}});
            /// Trail the prior state so backtracking undoes the write.
            auto it = lv->attrs.find(*key);
            if (vm) {
                vm::TrailEntry e{};
                e.kind = vm::TrailEntry::Kind::Attr;
                e.var = v;
                e.module_key = *key;
                if (it != lv->attrs.end()) {
                    e.had_prev = true;
                    e.prev_value = it->second;
                } else {
                    e.had_prev = false;
                    e.prev_value = nanbox::Nil;
                }
                vm->trail_stack().push_back(e);
            }
            lv->attrs[*key] = val;
            return True;
        });

    env.register_builtin("get-attr", 2, false,
        [&heap](Args args) -> std::expected<LispVal, RuntimeError> {
            const LispVal v = args[0];
            const LispVal mod = args[1];
            if (!ops::is_boxed(v) || ops::tag(v) != Tag::HeapObject) return False;
            auto* lv = heap.try_get_as<ObjectKind::LogicVar, types::LogicVar>(ops::payload(v));
            if (!lv) return False;
            auto key = get_symbol_id(mod);
            if (!key)
                return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError,
                    "get-attr: second arg must be a symbol (module name)"}});
            auto it = lv->attrs.find(*key);
            return (it == lv->attrs.end()) ? False : it->second;
        });

    env.register_builtin("del-attr", 2, false,
        [&heap, vm](Args args) -> std::expected<LispVal, RuntimeError> {
            const LispVal v = args[0];
            const LispVal mod = args[1];
            if (!ops::is_boxed(v) || ops::tag(v) != Tag::HeapObject) return False;
            auto* lv = heap.try_get_as<ObjectKind::LogicVar, types::LogicVar>(ops::payload(v));
            if (!lv) return False;
            auto key = get_symbol_id(mod);
            if (!key)
                return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError,
                    "del-attr: second arg must be a symbol (module name)"}});
            auto it = lv->attrs.find(*key);
            if (it == lv->attrs.end()) return False;
            /// Trail so backtracking re-installs the attribute.
            if (vm) {
                vm::TrailEntry e{};
                e.kind = vm::TrailEntry::Kind::Attr;
                e.var = v;
                e.module_key = *key;
                e.had_prev = true;
                e.prev_value = it->second;
                vm->trail_stack().push_back(e);
            }
            lv->attrs.erase(it);
            return True;
        });

    env.register_builtin("attr-var?", 1, false,
        [&heap](Args args) -> std::expected<LispVal, RuntimeError> {
            const LispVal v = args[0];
            if (!ops::is_boxed(v) || ops::tag(v) != Tag::HeapObject) return False;
            auto* lv = heap.try_get_as<ObjectKind::LogicVar, types::LogicVar>(ops::payload(v));
            if (!lv) return False;
            /// An attributed variable must be unbound and have at least one attr.
            return (!lv->binding.has_value() && !lv->attrs.empty()) ? True : False;
        });

    env.register_builtin("register-attr-hook!", 2, false,
        [vm](Args args) -> std::expected<LispVal, RuntimeError> {
            if (!vm) return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError,
                "register-attr-hook!: requires a running VM"}});
            auto key = get_symbol_id(args[0]);
            if (!key)
                return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError,
                    "register-attr-hook!: first arg must be a symbol (module name)"}});
            vm->attr_unify_hooks()[*key] = args[1];
            return True;
        });

    /**
     * logic-var/named : create a fresh unbound LogicVar with a debug name
     *
     * `(var-name v)` introspection, tracing, and future error messages.
     */
    env.register_builtin("logic-var/named", 1, false,
        [&heap, &intern_table](Args args) -> std::expected<LispVal, RuntimeError> {
            std::string name;
            const LispVal v = args[0];
            if (ops::is_boxed(v) && ops::tag(v) == Tag::Symbol) {
                auto s = get_symbol_name(v, intern_table);
                if (s) name = std::string(*s);
            } else if (auto sv = StringView::try_from(v, intern_table)) {
                name = std::string(sv->view());
            } else {
                return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError,
                    "logic-var/named: name must be a symbol or string"}});
            }
            return memory::factory::make_logic_var(heap, std::move(name));
        });

    /**
     * var-name : return the debug name of a LogicVar, or #f if none / not a var
     */
    env.register_builtin("var-name", 1, false,
        [&heap, &intern_table](Args args) -> std::expected<LispVal, RuntimeError> {
            const LispVal v = args[0];
            if (!ops::is_boxed(v) || ops::tag(v) != Tag::HeapObject) return False;
            auto* lv = heap.try_get_as<ObjectKind::LogicVar, types::LogicVar>(ops::payload(v));
            if (!lv || lv->name.empty()) return False;
            return make_string(heap, intern_table, lv->name);
        });

    /**
     * Occurs-check policy
     *
     * (set-occurs-check! 'always)  ; run occurs-check, fail on cycle (default)
     * (set-occurs-check! 'never)   ; skip occurs-check (ISO-Prolog default; faster)
     * (set-occurs-check! 'error)   ; run occurs-check, raise error on cycle
     */
    env.register_builtin("set-occurs-check!", 1, false,
        [&intern_table, vm](Args args) -> std::expected<LispVal, RuntimeError> {
            if (!vm) return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError,
                "set-occurs-check!: requires a running VM"}});
            const LispVal v = args[0];
            if (!ops::is_boxed(v) || ops::tag(v) != Tag::Symbol)
                return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError,
                    "set-occurs-check!: expected a symbol ('always / 'never / 'error)"}});
            auto sname = get_symbol_name(v, intern_table);
            if (!sname) return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError,
                "set-occurs-check!: invalid symbol"}});
            if (*sname == "always") vm->set_occurs_check_mode(vm::VM::OccursCheckMode::Always);
            else if (*sname == "never") vm->set_occurs_check_mode(vm::VM::OccursCheckMode::Never);
            else if (*sname == "error") vm->set_occurs_check_mode(vm::VM::OccursCheckMode::Error);
            else return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::UserError,
                "set-occurs-check!: mode must be 'always, 'never, or 'error"}});
            return True;
        });

    env.register_builtin("occurs-check-mode", 0, false,
        [&intern_table, vm](Args) -> std::expected<LispVal, RuntimeError> {
            if (!vm) return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError,
                "occurs-check-mode: requires a running VM"}});
            switch (vm->occurs_check_mode()) {
                case vm::VM::OccursCheckMode::Always: return make_symbol(intern_table, "always");
                case vm::VM::OccursCheckMode::Never: return make_symbol(intern_table, "never");
                case vm::VM::OccursCheckMode::Error: return make_symbol(intern_table, "error");
            }
            return make_symbol(intern_table, "always");
        });

    /**
     * Ground check: ground?
     * Returns #t iff the term contains no unbound logic variables.
     * Recurses into Cons pairs and Vectors; treats all other heap objects
     */

    env.register_builtin("ground?", 1, false, [&heap](Args args) -> std::expected<LispVal, RuntimeError> {
        std::function<bool(LispVal)> is_ground = [&](LispVal v) -> bool {
            LispVal curr = v;
            for (;;) {
                if (!ops::is_boxed(curr) || ops::tag(curr) != Tag::HeapObject)
                    return true;
                auto id = ops::payload(curr);
                if (auto* lv = heap.try_get_as<ObjectKind::LogicVar, types::LogicVar>(id)) {
                    if (!lv->binding.has_value()) return false;  ///< unbound
                    curr = *lv->binding;                          ///< follow chain
                } else if (auto* cons = heap.try_get_as<ObjectKind::Cons, types::Cons>(id)) {
                    return is_ground(cons->car) && is_ground(cons->cdr);
                } else if (auto* vec = heap.try_get_as<ObjectKind::Vector, types::Vector>(id)) {
                    for (const auto& elem : vec->elements)
                        if (!is_ground(elem)) return false;
                    return true;
                } else if (auto* ct = heap.try_get_as<ObjectKind::CompoundTerm, types::CompoundTerm>(id)) {
                    for (const auto& a : ct->args)
                        if (!is_ground(a)) return false;
                    return true;
                } else {
                    return true;  ///< string, closure, port, etc.
                }
            }
        };
        return is_ground(args[0]) ? True : False;
    });

    /**
     * Compound terms: term / functor / arity / arg / compound?
     *
     * A `CompoundTerm` is a structured logic term with a symbol functor and
     * Unifies structurally with other compound terms of the same functor+arity.
     * See docs/logic.md and docs/logic-next-steps.md for the rationale.
     */

    env.register_builtin("compound?", 1, false,
        [&heap](Args args) -> std::expected<LispVal, RuntimeError> {
            const LispVal v = args[0];
            if (!ops::is_boxed(v) || ops::tag(v) != Tag::HeapObject) return False;
            return heap.try_get_as<ObjectKind::CompoundTerm, types::CompoundTerm>(ops::payload(v))
                ? True : False;
        });

    env.register_builtin("term", 1, true,
        [&heap](Args args) -> std::expected<LispVal, RuntimeError> {
            const LispVal fn = args[0];
            if (!ops::is_boxed(fn) || ops::tag(fn) != Tag::Symbol)
                return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError,
                    "term: functor must be a symbol"}});
            std::vector<LispVal> targs;
            targs.reserve(args.size() > 0 ? args.size() - 1 : 0);
            for (std::size_t i = 1; i < args.size(); ++i) targs.push_back(args[i]);
            return memory::factory::make_compound(heap, fn, std::move(targs));
        });

    env.register_builtin("functor", 1, false,
        [&heap](Args args) -> std::expected<LispVal, RuntimeError> {
            const LispVal v = args[0];
            if (!ops::is_boxed(v) || ops::tag(v) != Tag::HeapObject) return False;
            auto* ct = heap.try_get_as<ObjectKind::CompoundTerm, types::CompoundTerm>(ops::payload(v));
            if (!ct) return False;
            return ct->functor;
        });

    env.register_builtin("arity", 1, false,
        [&heap](Args args) -> std::expected<LispVal, RuntimeError> {
            const LispVal v = args[0];
            if (!ops::is_boxed(v) || ops::tag(v) != Tag::HeapObject)
                return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError,
                    "arity: argument must be a compound term"}});
            auto* ct = heap.try_get_as<ObjectKind::CompoundTerm, types::CompoundTerm>(ops::payload(v));
            if (!ct) return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError,
                "arity: argument must be a compound term"}});
            auto enc = ops::encode<int64_t>(static_cast<int64_t>(ct->args.size()));
            if (!enc) return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError,
                "arity: arity does not fit in a fixnum"}});
            return *enc;
        });

    env.register_builtin("arg", 2, false,
        [&heap](Args args) -> std::expected<LispVal, RuntimeError> {
            auto idx_opt = ops::decode<int64_t>(args[0]);
            if (!idx_opt || !ops::is_boxed(args[0]) || ops::tag(args[0]) != Tag::Fixnum)
                return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError,
                    "arg: first argument must be a fixnum index"}});
            const LispVal v = args[1];
            if (!ops::is_boxed(v) || ops::tag(v) != Tag::HeapObject)
                return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError,
                    "arg: second argument must be a compound term"}});
            auto* ct = heap.try_get_as<ObjectKind::CompoundTerm, types::CompoundTerm>(ops::payload(v));
            if (!ct) return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError,
                "arg: second argument must be a compound term"}});
            int64_t i = *idx_opt;
            if (i < 1 || static_cast<std::size_t>(i) > ct->args.size())
                return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::UserError,
                    "arg: index out of range"}});
            return ct->args[static_cast<std::size_t>(i - 1)];
        });

    /**
     *
     * These builtins were removed along with the Dual heap type.
     * They are retained as error stubs so that the global builtin slot indices
     * remain stable (existing compiled bytecode references slots by index).
     */

    env.register_builtin("dual?", 1, false, [](Args) -> std::expected<LispVal, RuntimeError> {
        return False;  ///< Nothing is a Dual any more
    });

    env.register_builtin("dual-primal", 1, false, [](Args args) -> std::expected<LispVal, RuntimeError> {
        return args[0];  ///< pass-through
    });

    env.register_builtin("dual-backprop", 1, false, [&heap](Args) -> std::expected<LispVal, RuntimeError> {
        /// Return a no-op backpropagator
        return make_primitive(heap,
            [](Args) -> std::expected<LispVal, RuntimeError> { return Nil; },
            1, false);
    });

    env.register_builtin("make-dual", 2, false, [](Args) -> std::expected<LispVal, RuntimeError> {
        return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError,
            "make-dual: Dual AD has been removed  -  use tape-based AD instead"}});
    });
}

void PrimReg::register_logic_prop_attr_bridge() {
    using Args = PrimReg::Args;
    auto& env = this->env;
    auto* vm = this->vm;

    using detail::core_primitives_logic::get_symbol_id;

    /**
     * (register-prop-attr! 'key)
     *   Marks attribute key 'key as carrying a list of re-propagator thunks.
     *   When `unify` later binds a logic var carrying this attribute, every
     *   thunk in the attribute's value (a list) is *enqueued* on the VM's
     *   FIFO propagation queue rather than invoked synchronously.
     */
    env.register_builtin("register-prop-attr!", 1, false,
        [vm](Args args) -> std::expected<LispVal, RuntimeError> {
            if (!vm) return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError,
                "register-prop-attr!: requires a running VM"}});
            auto key = get_symbol_id(args[0]);
            if (!key)
                return std::unexpected(RuntimeError{VMError{RuntimeErrorCode::TypeError,
                    "register-prop-attr!: arg must be a symbol (attribute key)"}});
            vm->async_thunk_attrs().insert(*key);
            return True;
        });
}

} // namespace eta::runtime
