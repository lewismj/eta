#pragma once

/**
 * @file aad_unary_helpers.h
 * @brief Shared helper utilities for unary tape-aware math primitives.
 */

#include <expected>
#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "eta/runtime/ad_error.h"
#include "eta/runtime/error.h"
#include "eta/runtime/factory.h"
#include "eta/runtime/memory/heap.h"
#include "eta/runtime/nanbox.h"
#include "eta/runtime/numeric_value.h"
#include "eta/runtime/types/tape.h"
#include "eta/runtime/types/tape_ref.h"
#include "eta/runtime/vm/vm.h"

namespace eta::runtime::detail::aad_unary {

using namespace eta::runtime::error;
using namespace eta::runtime::memory::heap;
using namespace eta::runtime::nanbox;

/**
 * Resolve the currently active AD tape for a unary operation.
 */
inline std::expected<types::Tape*, RuntimeError>
active_tape_for_op(Heap& heap, vm::VM* vm, const char* op_name) {
    if (!vm) {
        return std::unexpected(RuntimeError{
            VMError{RuntimeErrorCode::TypeError, std::string(op_name) + ": requires a running VM"}});
    }
    const LispVal active = vm->active_tape();
    auto* tape = (ops::is_boxed(active) && ops::tag(active) == Tag::HeapObject)
        ? heap.try_get_as<ObjectKind::Tape, types::Tape>(ops::payload(active))
        : nullptr;
    if (!tape) {
        return std::unexpected(ad::make_error(
            ad::kTagNoActiveTape,
            std::string(op_name) + ": no active tape",
            {ad::field("op", std::string(op_name))}));
    }
    return tape;
}

/**
 * Validate that a TapeRef belongs to the active tape and still points at a
 * live node.
 */
inline std::expected<uint32_t, RuntimeError>
validate_tape_ref_for_op(types::Tape* tape, LispVal ref, const char* op_name, const char* role) {
    const auto parts = types::tape_ref::decode(ref);
    if (parts.tape_id != tape->tape_id) {
        return std::unexpected(ad::make_error(
            ad::kTagMixedTape,
            std::string(op_name) + ": reference belongs to a different tape",
            {
                ad::field("op", std::string(op_name)),
                ad::field("role", std::string(role)),
                ad::field("expected-tape-id", tape->tape_id),
                ad::field("actual-tape-id", parts.tape_id),
                ad::field("generation", parts.generation),
                ad::field("node-index", parts.node_index)
            }));
    }
    if (parts.generation != tape->generation) {
        return std::unexpected(ad::make_error(
            ad::kTagStaleRef,
            std::string(op_name) + ": stale TapeRef generation",
            {
                ad::field("op", std::string(op_name)),
                ad::field("role", std::string(role)),
                ad::field("tape-id", tape->tape_id),
                ad::field("expected-gen", tape->generation),
                ad::field("actual-gen", parts.generation),
                ad::field("node-index", parts.node_index)
            }));
    }
    if (parts.node_index >= tape->entries.size()) {
        return std::unexpected(ad::make_error(
            ad::kTagStaleRef,
            std::string(op_name) + ": TapeRef index out of range",
            {
                ad::field("op", std::string(op_name)),
                ad::field("role", std::string(role)),
                ad::field("tape-id", tape->tape_id),
                ad::field("expected-gen", tape->generation),
                ad::field("actual-gen", parts.generation),
                ad::field("node-index", parts.node_index)
            }));
    }
    return parts.node_index;
}

/**
 * Guard a newly-created tape node index against TapeRef payload limits.
 */
inline std::expected<uint32_t, RuntimeError>
checked_node_index_for_op(uint32_t idx, const char* op_name) {
    if (idx > types::tape_ref::MAX_NODE_INDEX) {
        return std::unexpected(RuntimeError{VMError{
            RuntimeErrorCode::InternalError,
            std::string(op_name) + ": tape node index exceeds TapeRef capacity"}});
    }
    return idx;
}

/**
 * Construct a TapeRef result with index-capacity validation.
 */
inline std::expected<LispVal, RuntimeError>
make_tape_ref_result(types::Tape* tape, uint32_t idx, const char* op_name) {
    auto checked = checked_node_index_for_op(idx, op_name);
    if (!checked) {
        return std::unexpected(checked.error());
    }
    return types::tape_ref::make(tape->tape_id, tape->generation, *checked);
}

/**
 * Append a unary tape node and return the encoded TapeRef result.
 */
inline std::expected<LispVal, RuntimeError>
push_unary_tape_entry(types::Tape* tape, types::TapeOp op, uint32_t input_index, double primal, const char* op_name) {
    const uint32_t out = tape->push({op, input_index, 0, primal, 0.0});
    return make_tape_ref_result(tape, out, op_name);
}

/**
 * Execute numeric fallback handling for unary primitives.
 *
 * The callback receives a classified NumericValue and returns the primitive
 * result for the non-tape path.
 */
template<typename NumericFn>
inline std::expected<LispVal, RuntimeError>
dispatch_numeric_fallback(Heap& heap, LispVal arg, const char* op_name, NumericFn&& numeric_fn) {
    const auto n = classify_numeric(arg, heap);
    if (!n.is_valid()) {
        return std::unexpected(RuntimeError{VMError{
            RuntimeErrorCode::TypeError, std::string(op_name) + ": argument is not a number"}});
    }
    return std::invoke(std::forward<NumericFn>(numeric_fn), n);
}

}  // namespace eta::runtime::detail::aad_unary

