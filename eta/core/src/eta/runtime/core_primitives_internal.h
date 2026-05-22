#pragma once

#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <vector>

#include "eta/runtime/core_primitives.h"

namespace eta::runtime {
namespace types {
struct Tape;
}

/**
 * @brief Internal registration dispatcher state for split core primitives.
 */
struct PrimReg {
    using Args = std::span<const LispVal>;

    BuiltinEnvironment& env;
    Heap& heap;
    InternTable& intern_table;
    vm::VM* vm;

    PrimReg(BuiltinEnvironment& env, Heap& heap, InternTable& intern_table, vm::VM* vm);

    void register_arithmetic();
    void register_pair_list_bridge();
    void register_math();
    void register_sequences();
    void register_sequences_higher_order_bridge();
    void register_sequences_collections_and_atoms_bridge();
    void register_strings();
    void register_misc();
    void register_misc_lifecycle_bridge();
    void register_actor_bridge();
    void register_misc_eval_bridge();
    void register_logic();
    void register_logic_prop_attr_bridge();
    void register_clp();
    void register_clp_prop_queue_size_bridge();
    void register_aad();
    void register_stats();

    static bool has_tape_ref(Args args);
    static uint32_t allocate_tape_id();
    static RuntimeError make_ad_runtime_error(
        const char* tag,
        std::string message,
        std::vector<error::VMErrorField> fields = {});
    static std::expected<uint32_t, RuntimeError> validate_ref_for_tape(
        types::Tape* tape,
        LispVal ref,
        const char* op_name,
        const char* role);
    static bool policy_is_strict(const vm::VM* vm);
    static RuntimeError make_nondiff_error(std::string op, std::string detail);
    static RuntimeError make_domain_error(
        std::string op,
        double base,
        double exponent,
        std::string detail);
    static RuntimeError make_unary_domain_error(
        std::string op,
        double value,
        std::string detail);
    static std::expected<types::Tape*, RuntimeError> get_active_tape_for_op(
        Heap& heap,
        vm::VM* vm,
        const char* op_name);

};

} ///< namespace eta::runtime
