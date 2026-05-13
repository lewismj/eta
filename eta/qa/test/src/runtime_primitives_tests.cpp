/**
 * @file runtime_primitives_tests.cpp
 * @brief Unit tests for runtime primitive bootstrap helpers.
 */

#include <boost/test/unit_test.hpp>

#include <array>
#include <cmath>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "eta/runtime/aad_unary_helpers.h"
#include "eta/runtime/ad_error.h"
#include "eta/runtime/builtin_env.h"
#include "eta/runtime/core_primitives.h"
#include "eta/runtime/error.h"
#include "eta/runtime/extension_env.h"
#include "eta/runtime/memory/heap.h"
#include "eta/runtime/memory/intern_table.h"
#include "eta/runtime/nanbox.h"
#include "eta/runtime/numeric_value.h"
#include "eta/runtime/types/tape.h"
#include "eta/runtime/types/tape_ref.h"
#include "eta/runtime/types/primitive.h"
#include "eta/runtime/vm/vm.h"
#include "eta/session/runtime_primitives.h"

namespace {

eta::runtime::types::PrimitiveFunc make_const_primitive(std::int64_t value) {
    return [value](eta::runtime::types::PrimitiveArgs) {
        return eta::runtime::nanbox::ops::encode(value);
    };
}

std::expected<eta::runtime::nanbox::LispVal, eta::runtime::error::RuntimeError> invoke_builtin(
    const eta::runtime::BuiltinEnvironment& builtins,
    std::string_view name,
    std::span<const eta::runtime::nanbox::LispVal> args) {
    const auto index = builtins.lookup(name);
    if (!index.has_value()) {
        return std::unexpected(eta::runtime::error::RuntimeError{
            eta::runtime::error::VMError{
                eta::runtime::error::RuntimeErrorCode::InternalError,
                std::string("missing builtin: ") + std::string(name)}});
    }
    return builtins.specs()[*index].func(args);
}

const eta::runtime::error::VMErrorField* find_vm_error_field(
    const eta::runtime::error::VMError& error,
    std::string_view key) {
    for (const auto& field : error.fields) {
        if (field.key == key) {
            return &field;
        }
    }
    return nullptr;
}

} // namespace

BOOST_AUTO_TEST_SUITE(runtime_primitives_tests)

BOOST_AUTO_TEST_CASE(extension_environment_fingerprint_is_stable_for_equal_surfaces) {
    eta::runtime::ExtensionEnvironment first;
    eta::runtime::ExtensionEnvironment same_surface;
    eta::runtime::ExtensionEnvironment changed_arity;
    eta::runtime::ExtensionEnvironment reordered;

    first.register_extension("ext.alpha", 1u, false, make_const_primitive(1));
    first.register_extension("ext.beta", 2u, true, make_const_primitive(2));

    same_surface.register_extension("ext.alpha", 1u, false, make_const_primitive(42));
    same_surface.register_extension("ext.beta", 2u, true, make_const_primitive(99));

    changed_arity.register_extension("ext.alpha", 2u, false, make_const_primitive(1));
    changed_arity.register_extension("ext.beta", 2u, true, make_const_primitive(2));

    reordered.register_extension("ext.beta", 2u, true, make_const_primitive(2));
    reordered.register_extension("ext.alpha", 1u, false, make_const_primitive(1));

    BOOST_TEST(eta::runtime::ExtensionEnvironment{}.fingerprint() == 0u);
    BOOST_TEST(first.fingerprint() == same_surface.fingerprint());
    BOOST_TEST(first.fingerprint() != changed_arity.fingerprint());
    BOOST_TEST(first.fingerprint() != reordered.fingerprint());
}

BOOST_AUTO_TEST_CASE(runtime_primitive_installer_populates_slots_and_names) {
    eta::runtime::memory::heap::Heap heap(1ull << 20);
    eta::runtime::BuiltinEnvironment builtins;
    eta::runtime::ExtensionEnvironment extensions;

    builtins.register_builtin("core.one", 0u, false, make_const_primitive(1));
    builtins.register_builtin("core.two", 1u, false, make_const_primitive(2));
    extensions.register_extension("ext.three", 0u, false, make_const_primitive(3));

    eta::session::RuntimePrimitiveInstaller installer(heap, builtins, extensions);
    BOOST_TEST(!installer.installed());
    BOOST_TEST(installer.builtin_count() == 2u);
    BOOST_TEST(installer.total_primitive_count() == 3u);

    std::vector<eta::runtime::nanbox::LispVal> globals;
    const auto install_result = installer.install_into(globals, 6u);
    BOOST_REQUIRE(install_result.has_value());
    BOOST_TEST(installer.installed());
    BOOST_TEST(globals.size() == 6u);

    const auto assert_primitive_debug_name =
        [&](std::size_t slot, const std::string& expected_name) {
            using eta::runtime::memory::heap::ObjectKind;
            using eta::runtime::nanbox::ops::payload;
            auto* primitive = heap.try_get_as<ObjectKind::Primitive, eta::runtime::types::Primitive>(
                payload(globals[slot]));
            BOOST_REQUIRE(primitive != nullptr);
            BOOST_TEST(primitive->debug_name == expected_name);
        };

    assert_primitive_debug_name(0u, "core.one");
    assert_primitive_debug_name(1u, "core.two");
    assert_primitive_debug_name(2u, "ext.three");

    std::unordered_map<uint32_t, std::string> global_names;
    installer.record_names(global_names);
    BOOST_TEST(global_names[0u] == "core.one");
    BOOST_TEST(global_names[1u] == "core.two");
    BOOST_TEST(global_names[2u] == "ext.three");

    installer.invalidate();
    BOOST_TEST(!installer.installed());
}

BOOST_AUTO_TEST_CASE(runtime_primitive_installer_does_not_reset_existing_non_primitive_globals) {
    eta::runtime::memory::heap::Heap heap(1ull << 20);
    eta::runtime::BuiltinEnvironment builtins;
    eta::runtime::ExtensionEnvironment extensions;

    builtins.register_builtin("core.only", 0u, false, make_const_primitive(1));
    eta::session::RuntimePrimitiveInstaller installer(heap, builtins, extensions);

    std::vector<eta::runtime::nanbox::LispVal> globals(8u, eta::runtime::nanbox::Nil);
    globals[6] = eta::runtime::nanbox::ops::encode(std::int64_t{77}).value();

    const auto install_result = installer.install_into(globals, 1u);
    BOOST_REQUIRE(install_result.has_value());
    BOOST_TEST(globals.size() == 8u);

    const auto preserved = eta::runtime::nanbox::ops::decode<std::int64_t>(globals[6]);
    BOOST_REQUIRE(preserved.has_value());
    BOOST_TEST(*preserved == 77);
}

BOOST_AUTO_TEST_CASE(aad_unary_helper_reports_no_active_tape) {
    eta::runtime::memory::heap::Heap heap(1ull << 20);
    eta::runtime::memory::intern::InternTable intern_table;
    eta::runtime::vm::VM vm(heap, intern_table);

    auto active_tape = eta::runtime::detail::aad_unary::active_tape_for_op(heap, &vm, "sin");
    BOOST_REQUIRE(!active_tape.has_value());

    auto* vm_error = std::get_if<eta::runtime::error::VMError>(&active_tape.error());
    BOOST_REQUIRE(vm_error != nullptr);
    BOOST_TEST(vm_error->tag_override == std::string(eta::runtime::ad::kTagNoActiveTape));
    BOOST_TEST(vm_error->message == "sin: no active tape");
}

BOOST_AUTO_TEST_CASE(aad_unary_helper_rejects_mismatched_and_stale_tape_refs) {
    eta::runtime::types::Tape tape;
    tape.tape_id = 9;
    tape.generation = 4;
    tape.entries.push_back({eta::runtime::types::TapeOp::Var, 0, 0, 1.0, 0.0});

    const auto mismatched_ref = eta::runtime::types::tape_ref::make(tape.tape_id + 1u, tape.generation, 0u);
    auto mismatched = eta::runtime::detail::aad_unary::validate_tape_ref_for_op(
        &tape, mismatched_ref, "sin", "arg");
    BOOST_REQUIRE(!mismatched.has_value());
    auto* mismatched_error = std::get_if<eta::runtime::error::VMError>(&mismatched.error());
    BOOST_REQUIRE(mismatched_error != nullptr);
    BOOST_TEST(mismatched_error->tag_override == std::string(eta::runtime::ad::kTagMixedTape));
    BOOST_TEST(mismatched_error->message == "sin: reference belongs to a different tape");

    const auto stale_ref = eta::runtime::types::tape_ref::make(
        tape.tape_id, eta::runtime::types::tape_ref::next_generation(tape.generation), 0u);
    auto stale = eta::runtime::detail::aad_unary::validate_tape_ref_for_op(
        &tape, stale_ref, "sin", "arg");
    BOOST_REQUIRE(!stale.has_value());
    auto* stale_error = std::get_if<eta::runtime::error::VMError>(&stale.error());
    BOOST_REQUIRE(stale_error != nullptr);
    BOOST_TEST(stale_error->tag_override == std::string(eta::runtime::ad::kTagStaleRef));
    BOOST_TEST(stale_error->message == "sin: stale TapeRef generation");
}

BOOST_AUTO_TEST_CASE(aad_unary_helper_checks_tape_ref_index_capacity) {
    eta::runtime::types::Tape tape;
    tape.tape_id = 5;
    tape.generation = 2;

    auto ref = eta::runtime::detail::aad_unary::make_tape_ref_result(
        &tape, eta::runtime::types::tape_ref::MAX_NODE_INDEX + 1u, "sin");
    BOOST_REQUIRE(!ref.has_value());

    auto* vm_error = std::get_if<eta::runtime::error::VMError>(&ref.error());
    BOOST_REQUIRE(vm_error != nullptr);
    BOOST_CHECK(vm_error->code == eta::runtime::error::RuntimeErrorCode::InternalError);
    BOOST_TEST(vm_error->message == "sin: tape node index exceeds TapeRef capacity");
}

BOOST_AUTO_TEST_CASE(aad_unary_sin_cos_exp_match_primal_op_type_and_backward_gradient) {
    using eta::runtime::nanbox::LispVal;
    using eta::runtime::nanbox::ops::payload;
    using eta::runtime::memory::heap::ObjectKind;
    using eta::runtime::types::TapeOp;

    eta::runtime::memory::heap::Heap heap(1ull << 20);
    eta::runtime::memory::intern::InternTable intern_table;
    eta::runtime::vm::VM vm(heap, intern_table);

    eta::runtime::BuiltinEnvironment builtins;
    eta::runtime::register_core_primitives(builtins, heap, intern_table, &vm);

    const std::array<LispVal, 0> no_args{};
    auto tape_res = invoke_builtin(builtins, "tape-new", no_args);
    BOOST_REQUIRE(tape_res.has_value());
    const LispVal tape_val = *tape_res;

    auto* tape = heap.try_get_as<ObjectKind::Tape, eta::runtime::types::Tape>(payload(tape_val));
    BOOST_REQUIRE(tape != nullptr);

    struct UnaryCase {
        const char* name;
        double input;
        TapeOp expected_op;
        double expected_primal;
        double expected_gradient;
    };

    const std::array<UnaryCase, 3> cases{{
        {"sin", 0.5, TapeOp::Sin, std::sin(0.5), std::cos(0.5)},
        {"cos", 0.5, TapeOp::Cos, std::cos(0.5), -std::sin(0.5)},
        {"exp", 0.5, TapeOp::Exp, std::exp(0.5), std::exp(0.5)},
    }};

    constexpr double kTol = 1e-12;
    for (const auto& test_case : cases) {
        auto clear_res = invoke_builtin(builtins, "tape-clear!", std::array<LispVal, 1>{tape_val});
        BOOST_REQUIRE(clear_res.has_value());

        auto start_res = invoke_builtin(builtins, "tape-start!", std::array<LispVal, 1>{tape_val});
        BOOST_REQUIRE(start_res.has_value());

        auto x_input = eta::runtime::make_flonum(test_case.input);
        BOOST_REQUIRE(x_input.has_value());
        auto x_res = invoke_builtin(builtins, "tape-var", std::array<LispVal, 2>{tape_val, *x_input});
        BOOST_REQUIRE(x_res.has_value());
        const LispVal x_ref = *x_res;
        BOOST_REQUIRE(eta::runtime::types::tape_ref::is_tape_ref(x_ref));

        auto z_res = invoke_builtin(builtins, test_case.name, std::array<LispVal, 1>{x_ref});
        BOOST_REQUIRE(z_res.has_value());
        const LispVal z_ref = *z_res;
        BOOST_REQUIRE(eta::runtime::types::tape_ref::is_tape_ref(z_ref));

        auto stop_res = invoke_builtin(builtins, "tape-stop!", no_args);
        BOOST_REQUIRE(stop_res.has_value());

        auto primal_res = invoke_builtin(builtins, "tape-primal", std::array<LispVal, 2>{tape_val, z_ref});
        BOOST_REQUIRE(primal_res.has_value());
        auto primal = eta::runtime::classify_numeric(*primal_res, heap);
        BOOST_REQUIRE(primal.is_valid());
        BOOST_TEST(primal.as_double() == test_case.expected_primal, boost::test_tools::tolerance(kTol));

        const auto parts = eta::runtime::types::tape_ref::decode(z_ref);
        BOOST_REQUIRE(parts.node_index < tape->entries.size());
        BOOST_CHECK(tape->entries[parts.node_index].op == test_case.expected_op);

        auto backward_res = invoke_builtin(builtins, "tape-backward!", std::array<LispVal, 2>{tape_val, z_ref});
        BOOST_REQUIRE(backward_res.has_value());

        auto grad_res = invoke_builtin(builtins, "tape-adjoint", std::array<LispVal, 2>{tape_val, x_ref});
        BOOST_REQUIRE(grad_res.has_value());
        auto gradient = eta::runtime::classify_numeric(*grad_res, heap);
        BOOST_REQUIRE(gradient.is_valid());
        BOOST_TEST(gradient.as_double() == test_case.expected_gradient, boost::test_tools::tolerance(kTol));
    }
}

BOOST_AUTO_TEST_CASE(aad_unary_domain_sensitive_ops_match_primal_op_type_and_backward_gradient) {
    using eta::runtime::nanbox::LispVal;
    using eta::runtime::nanbox::ops::payload;
    using eta::runtime::memory::heap::ObjectKind;
    using eta::runtime::types::TapeOp;

    eta::runtime::memory::heap::Heap heap(1ull << 20);
    eta::runtime::memory::intern::InternTable intern_table;
    eta::runtime::vm::VM vm(heap, intern_table);

    eta::runtime::BuiltinEnvironment builtins;
    eta::runtime::register_core_primitives(builtins, heap, intern_table, &vm);

    const std::array<LispVal, 0> no_args{};
    auto tape_res = invoke_builtin(builtins, "tape-new", no_args);
    BOOST_REQUIRE(tape_res.has_value());
    const LispVal tape_val = *tape_res;

    auto* tape = heap.try_get_as<ObjectKind::Tape, eta::runtime::types::Tape>(payload(tape_val));
    BOOST_REQUIRE(tape != nullptr);

    struct UnaryCase {
        const char* name;
        double input;
        TapeOp expected_op;
        double expected_primal;
        double expected_gradient;
    };

    const std::array<UnaryCase, 6> cases{{
        {"tan", 0.25, TapeOp::Tan, std::tan(0.25), 1.0 / (std::cos(0.25) * std::cos(0.25))},
        {"atan", 0.25, TapeOp::Atan, std::atan(0.25), 1.0 / (1.0 + 0.25 * 0.25)},
        {"log", 2.0, TapeOp::Log, std::log(2.0), 1.0 / 2.0},
        {"sqrt", 4.0, TapeOp::Sqrt, std::sqrt(4.0), 1.0 / (2.0 * std::sqrt(4.0))},
        {"asin", 0.5, TapeOp::Asin, std::asin(0.5), 1.0 / std::sqrt(1.0 - 0.5 * 0.5)},
        {"acos", 0.5, TapeOp::Acos, std::acos(0.5), -1.0 / std::sqrt(1.0 - 0.5 * 0.5)},
    }};

    constexpr double kTol = 1e-12;
    for (const auto& test_case : cases) {
        auto clear_res = invoke_builtin(builtins, "tape-clear!", std::array<LispVal, 1>{tape_val});
        BOOST_REQUIRE(clear_res.has_value());

        auto start_res = invoke_builtin(builtins, "tape-start!", std::array<LispVal, 1>{tape_val});
        BOOST_REQUIRE(start_res.has_value());

        auto x_input = eta::runtime::make_flonum(test_case.input);
        BOOST_REQUIRE(x_input.has_value());
        auto x_res = invoke_builtin(builtins, "tape-var", std::array<LispVal, 2>{tape_val, *x_input});
        BOOST_REQUIRE(x_res.has_value());
        const LispVal x_ref = *x_res;
        BOOST_REQUIRE(eta::runtime::types::tape_ref::is_tape_ref(x_ref));

        auto z_res = invoke_builtin(builtins, test_case.name, std::array<LispVal, 1>{x_ref});
        BOOST_REQUIRE(z_res.has_value());
        const LispVal z_ref = *z_res;
        BOOST_REQUIRE(eta::runtime::types::tape_ref::is_tape_ref(z_ref));

        auto stop_res = invoke_builtin(builtins, "tape-stop!", no_args);
        BOOST_REQUIRE(stop_res.has_value());

        auto primal_res = invoke_builtin(builtins, "tape-primal", std::array<LispVal, 2>{tape_val, z_ref});
        BOOST_REQUIRE(primal_res.has_value());
        auto primal = eta::runtime::classify_numeric(*primal_res, heap);
        BOOST_REQUIRE(primal.is_valid());
        BOOST_TEST(primal.as_double() == test_case.expected_primal, boost::test_tools::tolerance(kTol));

        const auto parts = eta::runtime::types::tape_ref::decode(z_ref);
        BOOST_REQUIRE(parts.node_index < tape->entries.size());
        BOOST_CHECK(tape->entries[parts.node_index].op == test_case.expected_op);

        auto backward_res = invoke_builtin(builtins, "tape-backward!", std::array<LispVal, 2>{tape_val, z_ref});
        BOOST_REQUIRE(backward_res.has_value());

        auto grad_res = invoke_builtin(builtins, "tape-adjoint", std::array<LispVal, 2>{tape_val, x_ref});
        BOOST_REQUIRE(grad_res.has_value());
        auto gradient = eta::runtime::classify_numeric(*grad_res, heap);
        BOOST_REQUIRE(gradient.is_valid());
        BOOST_TEST(gradient.as_double() == test_case.expected_gradient, boost::test_tools::tolerance(kTol));
    }
}

BOOST_AUTO_TEST_CASE(aad_unary_domain_sensitive_ops_report_domain_error_tag_and_message) {
    using eta::runtime::nanbox::LispVal;

    eta::runtime::memory::heap::Heap heap(1ull << 20);
    eta::runtime::memory::intern::InternTable intern_table;
    eta::runtime::vm::VM vm(heap, intern_table);

    eta::runtime::BuiltinEnvironment builtins;
    eta::runtime::register_core_primitives(builtins, heap, intern_table, &vm);

    auto tape_res = invoke_builtin(builtins, "tape-new", std::array<LispVal, 0>{});
    BOOST_REQUIRE(tape_res.has_value());
    const LispVal tape_val = *tape_res;

    struct DomainCase {
        const char* name;
        double input;
        const char* detail;
    };

    const std::array<DomainCase, 4> cases{{
        {"log", 0.0, "requires x > 0"},
        {"sqrt", -1.0, "requires x >= 0"},
        {"asin", 1.2, "requires -1 <= x <= 1"},
        {"acos", -1.2, "requires -1 <= x <= 1"},
    }};

    for (const auto& test_case : cases) {
        auto clear_res = invoke_builtin(builtins, "tape-clear!", std::array<LispVal, 1>{tape_val});
        BOOST_REQUIRE(clear_res.has_value());

        auto start_res = invoke_builtin(builtins, "tape-start!", std::array<LispVal, 1>{tape_val});
        BOOST_REQUIRE(start_res.has_value());

        auto x_input = eta::runtime::make_flonum(test_case.input);
        BOOST_REQUIRE(x_input.has_value());
        auto x_res = invoke_builtin(builtins, "tape-var", std::array<LispVal, 2>{tape_val, *x_input});
        BOOST_REQUIRE(x_res.has_value());

        auto op_res = invoke_builtin(builtins, test_case.name, std::array<LispVal, 1>{*x_res});
        BOOST_REQUIRE(!op_res.has_value());

        auto* vm_error = std::get_if<eta::runtime::error::VMError>(&op_res.error());
        BOOST_REQUIRE(vm_error != nullptr);
        BOOST_TEST(vm_error->tag_override == std::string(eta::runtime::ad::kTagDomain));
        BOOST_TEST(vm_error->message == ": domain violation");

        auto* op_field = find_vm_error_field(*vm_error, "op");
        BOOST_REQUIRE(op_field != nullptr);
        auto* op_value = std::get_if<std::string>(&op_field->value);
        BOOST_REQUIRE(op_value != nullptr);
        BOOST_TEST(*op_value == std::string(test_case.name));

        auto* detail_field = find_vm_error_field(*vm_error, "detail");
        BOOST_REQUIRE(detail_field != nullptr);
        auto* detail_value = std::get_if<std::string>(&detail_field->value);
        BOOST_REQUIRE(detail_value != nullptr);
        BOOST_TEST(*detail_value == std::string(test_case.detail));

        auto stop_res = invoke_builtin(builtins, "tape-stop!", std::array<LispVal, 0>{});
        BOOST_REQUIRE(stop_res.has_value());
    }
}

BOOST_AUTO_TEST_CASE(aad_unary_domain_sensitive_ops_preserve_singular_gradient_behavior_in_strict_mode) {
    using eta::runtime::nanbox::LispVal;

    eta::runtime::memory::heap::Heap heap(1ull << 20);
    eta::runtime::memory::intern::InternTable intern_table;
    eta::runtime::vm::VM vm(heap, intern_table);

    eta::runtime::BuiltinEnvironment builtins;
    eta::runtime::register_core_primitives(builtins, heap, intern_table, &vm);

    auto tape_res = invoke_builtin(builtins, "tape-new", std::array<LispVal, 0>{});
    BOOST_REQUIRE(tape_res.has_value());
    const LispVal tape_val = *tape_res;

    auto strict_sym = eta::runtime::make_symbol(intern_table, "strict");
    BOOST_REQUIRE(strict_sym.has_value());
    auto zero_subgrad_sym = eta::runtime::make_symbol(intern_table, "zero-subgrad");
    BOOST_REQUIRE(zero_subgrad_sym.has_value());

    struct SingularCase {
        const char* name;
        double input;
        bool positive_infinity;
    };

    const std::array<SingularCase, 3> singular_cases{{
        {"sqrt", 0.0, true},
        {"asin", 1.0, true},
        {"acos", 1.0, false},
    }};

    const std::array<LispVal, 2> policies{*strict_sym, *zero_subgrad_sym};
    for (const LispVal policy : policies) {
        auto set_policy = invoke_builtin(
            builtins, "set-aad-nondiff-policy!", std::array<LispVal, 1>{policy});
        BOOST_REQUIRE(set_policy.has_value());

        for (const auto& test_case : singular_cases) {
            auto clear_res = invoke_builtin(builtins, "tape-clear!", std::array<LispVal, 1>{tape_val});
            BOOST_REQUIRE(clear_res.has_value());

            auto start_res = invoke_builtin(builtins, "tape-start!", std::array<LispVal, 1>{tape_val});
            BOOST_REQUIRE(start_res.has_value());

            auto x_input = eta::runtime::make_flonum(test_case.input);
            BOOST_REQUIRE(x_input.has_value());
            auto x_res = invoke_builtin(builtins, "tape-var", std::array<LispVal, 2>{tape_val, *x_input});
            BOOST_REQUIRE(x_res.has_value());

            auto z_res = invoke_builtin(builtins, test_case.name, std::array<LispVal, 1>{*x_res});
            BOOST_REQUIRE(z_res.has_value());

            auto stop_res = invoke_builtin(builtins, "tape-stop!", std::array<LispVal, 0>{});
            BOOST_REQUIRE(stop_res.has_value());

            auto backward_res = invoke_builtin(
                builtins, "tape-backward!", std::array<LispVal, 2>{tape_val, *z_res});
            BOOST_REQUIRE(backward_res.has_value());

            auto grad_res = invoke_builtin(
                builtins, "tape-adjoint", std::array<LispVal, 2>{tape_val, *x_res});
            BOOST_REQUIRE(grad_res.has_value());
            auto gradient = eta::runtime::classify_numeric(*grad_res, heap);
            BOOST_REQUIRE(gradient.is_valid());
            BOOST_TEST(std::isinf(gradient.as_double()));
            BOOST_TEST((gradient.as_double() > 0.0) == test_case.positive_infinity);
        }
    }
}

BOOST_AUTO_TEST_SUITE_END()
