#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

#include "eta/runtime/core_primitives_internal.h"
#include "eta/runtime/ad_error.h"
#include "eta/runtime/aad_unary_helpers.h"
#include "eta/runtime/types/tape.h"
#include "eta/runtime/types/tape_ref.h"
#include "eta/runtime/vm/vm.h"

namespace eta::runtime {

PrimReg::PrimReg(BuiltinEnvironment& env, Heap& heap, InternTable& intern_table, vm::VM* vm)
    : env(env), heap(heap), intern_table(intern_table), vm(vm) {}

bool PrimReg::has_tape_ref(Args args) {
    for (auto value : args) {
        if (ops::is_boxed(value) && ops::tag(value) == Tag::TapeRef) {
            return true;
        }
    }
    return false;
}

uint32_t PrimReg::allocate_tape_id() {
    static std::atomic<uint32_t> next_id{1};
    const uint32_t raw = next_id.fetch_add(1, std::memory_order_relaxed);
    return static_cast<uint32_t>(((raw - 1u) % types::tape_ref::MAX_TAPE_ID) + 1u);
}

RuntimeError PrimReg::make_ad_runtime_error(
    const char* tag,
    std::string message,
    std::vector<error::VMErrorField> fields) {
    return ad::make_error(tag, std::move(message), std::move(fields));
}

std::expected<uint32_t, RuntimeError> PrimReg::validate_ref_for_tape(
    types::Tape* tape,
    LispVal ref,
    const char* op_name,
    const char* role) {
    return detail::aad_unary::validate_tape_ref_for_op(tape, ref, op_name, role);
}

bool PrimReg::policy_is_strict(const vm::VM* vm) {
    return vm && vm->aad_nondiff_policy() == vm::VM::AadNondiffPolicy::Strict;
}

RuntimeError PrimReg::make_nondiff_error(std::string op, std::string detail) {
    const std::string message = op + ": non-differentiable point reached in strict mode";
    return make_ad_runtime_error(
        ad::kTagNondiffStrict,
        message,
        {
            ad::field("op", std::move(op)),
            ad::field("detail", std::move(detail))
        });
}

RuntimeError PrimReg::make_domain_error(
    std::string op,
    double base,
    double exponent,
    std::string detail) {
    const std::string message = op + ": domain violation";
    return make_ad_runtime_error(
        ad::kTagDomain,
        message,
        {
            ad::field("op", std::move(op)),
            ad::field("base", base),
            ad::field("exponent", exponent),
            ad::field("detail", std::move(detail))
        });
}

RuntimeError PrimReg::make_unary_domain_error(
    std::string op,
    double value,
    std::string detail) {
    const std::string message = op + ": domain violation";
    return make_ad_runtime_error(
        ad::kTagDomain,
        message,
        {
            ad::field("op", std::move(op)),
            ad::field("value", value),
            ad::field("detail", std::move(detail))
        });
}

std::expected<types::Tape*, RuntimeError> PrimReg::get_active_tape_for_op(
    Heap& heap,
    vm::VM* vm,
    const char* op_name) {
    return detail::aad_unary::active_tape_for_op(heap, vm, op_name);
}

void register_core_primitives(BuiltinEnvironment& env, Heap& heap, InternTable& intern_table, vm::VM* vm) {
    PrimReg reg{env, heap, intern_table, vm};
    reg.register_arithmetic();
    reg.register_math();
    reg.register_sequences();
    reg.register_strings();
    reg.register_sequences_collections_and_atoms_bridge();
    reg.register_misc();
    reg.register_logic();
    reg.register_clp();
    reg.register_aad();
    reg.register_stats();
    reg.register_logic_prop_attr_bridge();
    reg.register_clp_prop_queue_size_bridge();
    reg.register_misc_eval_bridge();
}

} ///< namespace eta::runtime
