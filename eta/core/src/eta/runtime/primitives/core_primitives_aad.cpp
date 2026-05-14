#include <expected>

#include "eta/runtime/core_primitives_internal.h"
#include "eta/runtime/primitives/core_primitives_aad_helpers.h"
#include "eta/runtime/factory.h"
#include "eta/runtime/numeric_value.h"
#include "eta/runtime/types/tape.h"
#include "eta/runtime/types/tape_ref.h"
#include "eta/runtime/vm/vm.h"

namespace eta::runtime {

void PrimReg::register_aad() {
    using Args = PrimReg::Args;
    auto& env = this->env;
    auto& heap = this->heap;
    auto* vm = this->vm;

    using detail::core_primitives_aad::ensure_tape_identity;
    using detail::core_primitives_aad::expect_tape_arg;

    /**
     * AD tape primitives expose reverse-mode tape lifecycle and value access.
     */

    env.register_builtin("tape-new", 0, false, [&heap](Args /*args*/) -> std::expected<LispVal, RuntimeError> {
        auto tv = make_tape(heap);
        if (!tv) return std::unexpected(tv.error());
        auto* tape = heap.try_get_as<ObjectKind::Tape, types::Tape>(ops::payload(*tv));
        if (!tape) {
            return std::unexpected(RuntimeError{VMError{
                RuntimeErrorCode::InternalError, "tape-new: failed to allocate tape object"}});
        }
        ensure_tape_identity(*tape);
        return *tv;
    });

    env.register_builtin("tape-start!", 1, false, [&heap, vm](Args args) -> std::expected<LispVal, RuntimeError> {
        if (!vm) {
            return std::unexpected(RuntimeError{VMError{
                RuntimeErrorCode::TypeError, "tape-start!: requires a running VM"}});
        }
        auto tape = expect_tape_arg(heap, args[0], "tape-start!", "argument");
        if (!tape) return std::unexpected(tape.error());
        ensure_tape_identity(**tape);
        vm->push_active_tape(args[0]);
        return True;
    });

    env.register_builtin("tape-stop!", 0, false, [vm](Args /*args*/) -> std::expected<LispVal, RuntimeError> {
        if (!vm) {
            return std::unexpected(RuntimeError{VMError{
                RuntimeErrorCode::TypeError, "tape-stop!: requires a running VM"}});
        }
        vm->pop_active_tape();
        return True;
    });

    env.register_builtin("tape-clear!", 1, false, [&heap](Args args) -> std::expected<LispVal, RuntimeError> {
        auto tape = expect_tape_arg(heap, args[0], "tape-clear!", "argument");
        if (!tape) return std::unexpected(tape.error());
        (*tape)->clear_and_bump_generation();
        return True;
    });

    env.register_builtin("tape-var", 2, false,
        [&heap](Args args) -> std::expected<LispVal, RuntimeError> {
            auto tape = expect_tape_arg(heap, args[0], "tape-var", "first argument");
            if (!tape) return std::unexpected(tape.error());
            ensure_tape_identity(**tape);

            auto n = classify_numeric(args[1], heap);
            if (!n.is_valid()) {
                return std::unexpected(RuntimeError{VMError{
                    RuntimeErrorCode::TypeError, "tape-var: second argument must be a number"}});
            }

            uint32_t idx = (*tape)->push_var(n.as_double());
            if (idx > types::tape_ref::MAX_NODE_INDEX) {
                return std::unexpected(RuntimeError{VMError{
                    RuntimeErrorCode::InternalError,
                    "tape-var: tape node index exceeds TapeRef capacity"}});
            }
            return types::tape_ref::make((*tape)->tape_id, (*tape)->generation, idx);
        });

    env.register_builtin("tape-backward!", 2, false,
        [&heap](Args args) -> std::expected<LispVal, RuntimeError> {
            auto tape = expect_tape_arg(heap, args[0], "tape-backward!", "first argument");
            if (!tape) return std::unexpected(tape.error());
            if (!types::tape_ref::is_tape_ref(args[1])) {
                return std::unexpected(RuntimeError{VMError{
                    RuntimeErrorCode::TypeError, "tape-backward!: second argument must be a tape-ref"}});
            }
            auto output_idx = PrimReg::validate_ref_for_tape(*tape, args[1], "tape-backward!", "output-ref");
            if (!output_idx) return std::unexpected(output_idx.error());
            (*tape)->backward(*output_idx);
            return True;
        });

    env.register_builtin("tape-adjoint", 2, false,
        [&heap](Args args) -> std::expected<LispVal, RuntimeError> {
            auto tape = expect_tape_arg(heap, args[0], "tape-adjoint", "first argument");
            if (!tape) return std::unexpected(tape.error());
            if (!types::tape_ref::is_tape_ref(args[1])) {
                return std::unexpected(RuntimeError{VMError{
                    RuntimeErrorCode::TypeError, "tape-adjoint: second argument must be a tape-ref"}});
            }
            auto idx = PrimReg::validate_ref_for_tape(*tape, args[1], "tape-adjoint", "ref");
            if (!idx) return std::unexpected(idx.error());
            return make_flonum((*tape)->entries[*idx].adjoint);
        });

    env.register_builtin("tape-primal", 2, false,
        [&heap](Args args) -> std::expected<LispVal, RuntimeError> {
            auto tape = expect_tape_arg(heap, args[0], "tape-primal", "first argument");
            if (!tape) return std::unexpected(tape.error());
            if (!types::tape_ref::is_tape_ref(args[1])) {
                return std::unexpected(RuntimeError{VMError{
                    RuntimeErrorCode::TypeError, "tape-primal: second argument must be a tape-ref"}});
            }
            auto idx = PrimReg::validate_ref_for_tape(*tape, args[1], "tape-primal", "ref");
            if (!idx) return std::unexpected(idx.error());
            return make_flonum((*tape)->entries[*idx].primal);
        });

    env.register_builtin("tape-ref?", 1, false, [](Args args) -> std::expected<LispVal, RuntimeError> {
        return types::tape_ref::is_tape_ref(args[0]) ? True : False;
    });

    env.register_builtin("tape-ref-index", 1, false, [](Args args) -> std::expected<LispVal, RuntimeError> {
        if (!types::tape_ref::is_tape_ref(args[0])) {
            return std::unexpected(RuntimeError{VMError{
                RuntimeErrorCode::TypeError, "tape-ref-index: argument must be a tape-ref"}});
        }
        const auto parts = types::tape_ref::decode(args[0]);
        return ops::encode(static_cast<int64_t>(parts.node_index));
    });

    env.register_builtin("tape-size", 1, false, [&heap](Args args) -> std::expected<LispVal, RuntimeError> {
        auto tape = expect_tape_arg(heap, args[0], "tape-size", "argument");
        if (!tape) return std::unexpected(tape.error());
        return ops::encode(static_cast<int64_t>((*tape)->entries.size()));
    });

    env.register_builtin("tape-ref-value-of", 2, false,
        [&heap](Args args) -> std::expected<LispVal, RuntimeError> {
            auto tape = expect_tape_arg(heap, args[0], "tape-ref-value-of", "first argument");
            if (!tape) return std::unexpected(tape.error());
            if (!types::tape_ref::is_tape_ref(args[1])) {
                return std::unexpected(RuntimeError{VMError{
                    RuntimeErrorCode::TypeError, "tape-ref-value-of: second argument must be a tape-ref"}});
            }
            auto idx = PrimReg::validate_ref_for_tape(*tape, args[1], "tape-ref-value-of", "ref");
            if (!idx) return std::unexpected(idx.error());
            return make_flonum((*tape)->entries[*idx].primal);
        });

    /**
     * tape-ref-value: extract the primal value of a TapeRef from the
     * current active tape. Non-TapeRef inputs remain pass-through.
     */
    env.register_builtin("tape-ref-value", 1, false,
        [&heap, vm](Args args) -> std::expected<LispVal, RuntimeError> {
            if (!types::tape_ref::is_tape_ref(args[0])) return args[0];

            auto tape = PrimReg::get_active_tape_for_op(heap, vm, "tape-ref-value");
            if (!tape) return std::unexpected(tape.error());

            auto idx = PrimReg::validate_ref_for_tape(*tape, args[0], "tape-ref-value", "ref");
            if (!idx) return std::unexpected(idx.error());
            return make_flonum((*tape)->entries[*idx].primal);
        });
}

} // namespace eta::runtime
