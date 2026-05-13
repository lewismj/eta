#include <boost/test/unit_test.hpp>

#include <algorithm>
#include <array>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_set>

#include <eta/runtime/memory/heap.h>
#include <eta/runtime/memory/intern_table.h>
#include <eta/runtime/builtin_catalog.h>
#include <eta/runtime/builtin_env.h>
#include <eta/runtime/builtin_metadata.h>
#include <eta/reader/special_form_docs.h>
#include <eta/interpreter/all_primitives.h>
#include <eta/runtime/os_primitives.h>
#include <eta/runtime/process_primitives.h>
#include <eta/runtime/time_primitives.h>
#include <eta/runtime/vm/vm.h>
#include <eta/torch/torch_primitives.h>
#include <eta/stats/stats_primitives.h>
#include <eta/log/log_primitives.h>

using namespace eta::runtime;
using namespace eta::runtime::memory::heap;
using namespace eta::runtime::memory::intern;

BOOST_AUTO_TEST_SUITE(builtin_sync_tests)

/**
 * Verify that register_builtin_specs() contains entries for
 * every builtin that the runtime modules register.
 *
 * We check os, process, time, torch, stats, and log individually (os/log require a live VM;
 * the others accept a null VM
 * pointer).
 * Port/IO/NNG require a live VM or driver-specific args, so full end-to-end
 * coverage is provided by the Driver constructor's verify_all_patched() call.
 */
BOOST_AUTO_TEST_CASE(names_ssot_contains_os_process_time_torch_stats_and_log) {
    /// 1. Analysis registration metadata from the builtin catalog
    BuiltinEnvironment names_env;
    register_builtin_specs(names_env);

    /// 2. OS primitives
    Heap heap(1ull << 22);
    InternTable intern;
    vm::VM vm(heap, intern);

    BuiltinEnvironment os_env;
    register_os_primitives(os_env, heap, intern, vm);

    for (size_t i = 0; i < os_env.size(); ++i) {
        auto idx = names_env.lookup(os_env.specs()[i].name);
        BOOST_TEST_CONTEXT("os builtin: " << os_env.specs()[i].name) {
            BOOST_REQUIRE(idx.has_value());
            BOOST_TEST(names_env.specs()[*idx].arity == os_env.specs()[i].arity);
            BOOST_TEST(names_env.specs()[*idx].has_rest == os_env.specs()[i].has_rest);
        }
    }

    /// 3. Process primitives
    BuiltinEnvironment process_env;
    register_process_primitives(process_env, heap, intern, vm);

    for (size_t i = 0; i < process_env.size(); ++i) {
        auto idx = names_env.lookup(process_env.specs()[i].name);
        BOOST_TEST_CONTEXT("process builtin: " << process_env.specs()[i].name) {
            BOOST_REQUIRE(idx.has_value());
            BOOST_TEST(names_env.specs()[*idx].arity == process_env.specs()[i].arity);
            BOOST_TEST(names_env.specs()[*idx].has_rest == process_env.specs()[i].has_rest);
        }
    }

    /// 4. Time primitives
    BuiltinEnvironment time_env;
    register_time_primitives(time_env, heap, intern, nullptr);

    for (size_t i = 0; i < time_env.size(); ++i) {
        auto idx = names_env.lookup(time_env.specs()[i].name);
        BOOST_TEST_CONTEXT("time builtin: " << time_env.specs()[i].name) {
            BOOST_REQUIRE(idx.has_value());
            BOOST_TEST(names_env.specs()[*idx].arity == time_env.specs()[i].arity);
            BOOST_TEST(names_env.specs()[*idx].has_rest == time_env.specs()[i].has_rest);
        }
    }

    /// 5. Torch primitives
    BuiltinEnvironment torch_env;
    eta::torch_bindings::register_torch_primitives(torch_env, heap, intern, nullptr);

    /// Every torch name must appear in the SSoT with matching metadata
    for (size_t i = 0; i < torch_env.size(); ++i) {
        auto idx = names_env.lookup(torch_env.specs()[i].name);
        BOOST_TEST_CONTEXT("torch builtin: " << torch_env.specs()[i].name) {
            BOOST_REQUIRE(idx.has_value());
            BOOST_TEST(names_env.specs()[*idx].arity == torch_env.specs()[i].arity);
            BOOST_TEST(names_env.specs()[*idx].has_rest == torch_env.specs()[i].has_rest);
        }
    }

    /// 6. Stats primitives
    BuiltinEnvironment stats_env;
    eta::stats_bindings::register_stats_primitives(stats_env, heap, intern, nullptr);

    for (size_t i = 0; i < stats_env.size(); ++i) {
        auto idx = names_env.lookup(stats_env.specs()[i].name);
        BOOST_TEST_CONTEXT("stats builtin: " << stats_env.specs()[i].name) {
            BOOST_REQUIRE(idx.has_value());
            BOOST_TEST(names_env.specs()[*idx].arity == stats_env.specs()[i].arity);
            BOOST_TEST(names_env.specs()[*idx].has_rest == stats_env.specs()[i].has_rest);
        }
    }

    /// 7. Log primitives
    BuiltinEnvironment log_env;
    eta::log::register_log_primitives(log_env, heap, intern, &vm);

    for (size_t i = 0; i < log_env.size(); ++i) {
        auto idx = names_env.lookup(log_env.specs()[i].name);
        BOOST_TEST_CONTEXT("log builtin: " << log_env.specs()[i].name) {
            BOOST_REQUIRE(idx.has_value());
            BOOST_TEST(names_env.specs()[*idx].arity == log_env.specs()[i].arity);
            BOOST_TEST(names_env.specs()[*idx].has_rest == log_env.specs()[i].has_rest);
        }
    }
}

/**
 * Verify basic properties of analysis builtin registration:
 * non-empty, no duplicate names.
 */
BOOST_AUTO_TEST_CASE(names_ssot_no_duplicates) {
    BuiltinEnvironment env;
    register_builtin_specs(env);

    BOOST_TEST(env.size() > 0u);

    std::unordered_set<std::string> seen;
    std::vector<std::string> duplicates;
    for (size_t i = 0; i < env.size(); ++i) {
        if (!seen.insert(env.specs()[i].name).second) {
            duplicates.push_back(env.specs()[i].name);
        }
    }
    if (!duplicates.empty()) {
        std::string msg = "Duplicate builtin names:";
        for (const auto& d : duplicates) msg += " " + d;
        BOOST_FAIL(msg);
    }
}

BOOST_AUTO_TEST_CASE(catalog_has_no_duplicate_names) {
    const auto catalog = builtin_catalog();
    BOOST_TEST(catalog.size() > 0u);

    std::unordered_set<std::string> seen;
    std::vector<std::string> duplicates;
    for (const auto& entry : catalog) {
        if (!seen.insert(entry.name).second) {
            duplicates.push_back(entry.name);
        }
    }

    if (!duplicates.empty()) {
        std::string msg = "Duplicate builtin catalog names:";
        for (const auto& d : duplicates) msg += " " + d;
        BOOST_FAIL(msg);
    }
}

BOOST_AUTO_TEST_CASE(catalog_matches_analysis_registration_metadata) {
    BuiltinEnvironment specs_env;
    register_builtin_specs(specs_env);

    const auto catalog = builtin_catalog();
    BOOST_REQUIRE_EQUAL(catalog.size(), specs_env.size());

    constexpr std::array<std::string_view, 5> allowed_owners = {
        "core",
        "sidecar:eta-torch",
        "sidecar:eta-stats",
        "sidecar:eta-log",
        "sidecar:eta-nng"
    };

    for (std::size_t i = 0; i < catalog.size(); ++i) {
        const auto& entry = catalog[i];
        const auto& spec = specs_env.specs()[i];

        const bool owner_allowed = std::find(
            allowed_owners.begin(),
            allowed_owners.end(),
            std::string_view(entry.owner))
            != allowed_owners.end();

        BOOST_TEST_CONTEXT("slot " << i << " name=" << entry.name) {
            BOOST_TEST(!entry.name.empty());
            BOOST_TEST(owner_allowed);
            BOOST_TEST(entry.name == spec.name);
            BOOST_TEST(entry.arity == spec.arity);
            BOOST_TEST(entry.has_rest == spec.has_rest);
        }
    }
}

BOOST_AUTO_TEST_CASE(catalog_registration_adapter_matches_catalog_exactly) {
    BuiltinEnvironment env;
    register_builtin_specs(env);

    const auto catalog = builtin_catalog();
    BOOST_REQUIRE_EQUAL(env.size(), catalog.size());

    for (std::size_t i = 0; i < catalog.size(); ++i) {
        const auto& spec = env.specs()[i];
        const auto& entry = catalog[i];
        BOOST_TEST_CONTEXT("slot " << i << " name=" << entry.name) {
            BOOST_TEST(spec.name == entry.name);
            BOOST_TEST(spec.arity == entry.arity);
            BOOST_TEST(spec.has_rest == entry.has_rest);
            BOOST_TEST(!spec.func);
        }
    }
}

BOOST_AUTO_TEST_CASE(catalog_order_matches_runtime_registration_order) {
    const auto catalog = builtin_catalog();

    Heap heap(1ull << 22);
    InternTable intern;
    vm::VM vm(heap, intern);

    BuiltinEnvironment runtime_env;
    eta::interpreter::register_all_primitives(runtime_env, heap, intern, vm);

    BOOST_REQUIRE_EQUAL(runtime_env.size(), catalog.size());
    for (std::size_t i = 0; i < catalog.size(); ++i) {
        const auto& spec = runtime_env.specs()[i];
        const auto& entry = catalog[i];
        BOOST_TEST_CONTEXT("slot " << i << " name=" << entry.name) {
            BOOST_TEST(spec.name == entry.name);
            BOOST_TEST(spec.arity == entry.arity);
            BOOST_TEST(spec.has_rest == entry.has_rest);
            BOOST_TEST(static_cast<bool>(spec.func));
        }
    }
}

BOOST_AUTO_TEST_CASE(runtime_builtin_registration_matches_metadata_exhaustive) {
    BuiltinEnvironment env;
    register_builtin_specs(env);

    const auto metadata = builtin_metadata();
    BOOST_REQUIRE_EQUAL(env.size(), metadata.size());

    Heap heap(1ull << 22);
    InternTable intern;
    vm::VM vm(heap, intern);

    env.begin_patching();
    eta::interpreter::register_all_primitives(env, heap, intern, vm);
    env.verify_all_patched();

    BOOST_REQUIRE_EQUAL(env.size(), metadata.size());
    for (std::size_t i = 0; i < metadata.size(); ++i) {
        const auto& spec = env.specs()[i];
        const auto& doc = metadata[i];
        BOOST_TEST_CONTEXT("slot " << i << " name=" << spec.name) {
            BOOST_TEST(spec.name == doc.name);
            BOOST_TEST(spec.arity == doc.arity);
            BOOST_TEST(spec.has_rest == doc.has_rest);
            BOOST_TEST(static_cast<bool>(spec.func));
        }
    }
}

BOOST_AUTO_TEST_CASE(register_all_primitives_installs_nng_sidecar_placeholders) {
    BuiltinEnvironment env;
    register_builtin_specs(env);

    Heap heap(1ull << 22);
    InternTable intern;
    vm::VM vm(heap, intern);

    env.begin_patching();
    eta::interpreter::register_all_primitives(env, heap, intern, vm);
    env.verify_all_patched();

    const auto send_idx = env.lookup("send!");
    BOOST_REQUIRE(send_idx.has_value());
    BOOST_REQUIRE(static_cast<bool>(env.specs()[*send_idx].func));

    const auto send_result = env.specs()[*send_idx].func({});
    BOOST_REQUIRE(!send_result.has_value());

    const auto* vm_error = std::get_if<error::VMError>(&send_result.error());
    BOOST_REQUIRE(vm_error != nullptr);
    BOOST_TEST(
        vm_error->message.find("requires package dependency 'eta-nng'")
        != std::string::npos);
}

BOOST_AUTO_TEST_CASE(builtin_metadata_is_consistent_across_consumers) {
    BuiltinEnvironment env;
    register_builtin_specs(env);

    const auto metadata = builtin_metadata();
    BOOST_REQUIRE_EQUAL(env.size(), metadata.size());

    for (std::size_t i = 0; i < metadata.size(); ++i) {
        const auto& spec = env.specs()[i];
        const auto& doc = metadata[i];
        BOOST_TEST_CONTEXT("slot " << i << " name=" << spec.name) {
            BOOST_TEST(spec.name == doc.name);
            BOOST_TEST(spec.arity == doc.arity);
            BOOST_TEST(spec.has_rest == doc.has_rest);
        }
    }
}

BOOST_AUTO_TEST_CASE(builtin_metadata_order_matches_catalog_order) {
    const auto catalog = builtin_catalog();
    const auto metadata = builtin_metadata();

    BOOST_REQUIRE_EQUAL(metadata.size(), catalog.size());
    for (std::size_t i = 0; i < metadata.size(); ++i) {
        const auto& entry = catalog[i];
        const auto& builtin = metadata[i];
        BOOST_TEST_CONTEXT("slot " << i << " name=" << entry.name) {
            BOOST_TEST(builtin.name == entry.name);
            BOOST_TEST(builtin.arity == entry.arity);
            BOOST_TEST(builtin.has_rest == entry.has_rest);
        }
    }
}

BOOST_AUTO_TEST_CASE(catalog_owner_matches_native_sidecar_lookup) {
    constexpr std::string_view sidecar_prefix = "sidecar:";

    for (const auto& entry : builtin_catalog()) {
        std::optional<std::string_view> expected_package;
        const std::string_view owner = entry.owner;
        if (owner.starts_with(sidecar_prefix)) {
            expected_package = owner.substr(sidecar_prefix.size());
        }

        const auto actual_package = builtin_native_sidecar_package(entry.name);

        BOOST_TEST_CONTEXT("builtin: " << entry.name << " owner=" << entry.owner) {
            BOOST_TEST(actual_package.has_value() == expected_package.has_value());
            if (expected_package.has_value()) {
                BOOST_REQUIRE(actual_package.has_value());
                BOOST_TEST(*actual_package == *expected_package);
            }
        }
    }
}

BOOST_AUTO_TEST_CASE(all_configured_builtins_have_docs) {
    constexpr std::array<std::string_view, 0> allowed_missing{};
    const auto missing = missing_builtin_docs(allowed_missing);

    if (!missing.empty()) {
        std::string message = "Builtins with missing docs:";
        for (const auto& name : missing) message += " " + name;
        BOOST_FAIL(message);
    }

    for (const auto& builtin : builtin_metadata()) {
        BOOST_TEST_CONTEXT("builtin: " << builtin.name) {
            BOOST_TEST(!builtin.category.empty());
            BOOST_TEST(!builtin.signature.empty());
            BOOST_TEST(!builtin.summary.empty());
        }
    }
}

BOOST_AUTO_TEST_CASE(doc_metadata_has_no_duplicate_special_forms) {
    std::unordered_set<std::string> seen;
    std::vector<std::string> duplicates;

    for (const auto& entry : eta::reader::special_form_docs()) {
        if (!seen.insert(std::string(entry.name)).second) {
            duplicates.push_back(std::string(entry.name));
        }
    }
    if (!duplicates.empty()) {
        std::string message = "Duplicate special-form docs:";
        for (const auto& name : duplicates) message += " " + name;
        BOOST_FAIL(message);
    }
}

BOOST_AUTO_TEST_CASE(doc_metadata_has_no_duplicate_builtins) {
    std::unordered_set<std::string> seen;
    std::vector<std::string> duplicates;

    for (const auto& builtin : builtin_metadata()) {
        if (!seen.insert(builtin.name).second) {
            duplicates.push_back(builtin.name);
        }
    }
    if (!duplicates.empty()) {
        std::string message = "Duplicate builtin docs:";
        for (const auto& name : duplicates) message += " " + name;
        BOOST_FAIL(message);
    }
}

BOOST_AUTO_TEST_CASE(doc_metadata_has_no_special_form_builtin_name_collisions) {
    std::unordered_set<std::string> builtin_names;
    for (const auto& builtin : builtin_metadata()) {
        builtin_names.insert(builtin.name);
    }

    std::vector<std::string> unexpected;
    for (const auto& entry : eta::reader::special_form_docs()) {
        if (!builtin_names.contains(std::string(entry.name))) continue;
        if (eta::reader::is_allowed_special_form_builtin_collision(entry.name)) continue;
        unexpected.push_back(std::string(entry.name));
    }

    if (!unexpected.empty()) {
        std::string message = "Unexpected special-form/builtin collisions:";
        for (const auto& name : unexpected) message += " " + name;
        BOOST_FAIL(message);
    }
}

/**
 * Verify patch mode mechanics: begin_patching + register_builtin validates
 * metadata and installs funcs without aborting when metadata matches.
 */
BOOST_AUTO_TEST_CASE(patch_mode_basic_mechanics) {
    BuiltinEnvironment env;

    /// Pre-register two names with null funcs
    env.register_builtin("foo", 1, false, PrimitiveFunc{});
    env.register_builtin("bar", 2, true,  PrimitiveFunc{});

    BOOST_TEST(env.size() == 2u);
    BOOST_TEST(!env.specs()[0].func);  ///< null
    BOOST_TEST(!env.specs()[1].func);  ///< null

    /// Switch to patch mode
    env.begin_patching();

    /// Patch with real funcs (matching metadata)
    PrimitiveFunc foo_fn = [](std::span<const nanbox::LispVal>)
        -> std::expected<nanbox::LispVal, error::RuntimeError> {
        return nanbox::Nil;
    };
    PrimitiveFunc bar_fn = [](std::span<const nanbox::LispVal>)
        -> std::expected<nanbox::LispVal, error::RuntimeError> {
        return nanbox::True;
    };

    env.register_builtin("foo", 1, false, foo_fn);
    env.register_builtin("bar", 2, true,  bar_fn);

    /// Both should now have non-null funcs
    BOOST_TEST(static_cast<bool>(env.specs()[0].func));
    BOOST_TEST(static_cast<bool>(env.specs()[1].func));

    /// verify_all_patched should succeed (no abort)
    env.verify_all_patched();

    /// Size unchanged
    BOOST_TEST(env.size() == 2u);
}

BOOST_AUTO_TEST_SUITE_END()
