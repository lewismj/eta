#include <boost/test/unit_test.hpp>

#include <span>
#include <unordered_set>

#include <eta/runtime/memory/heap.h>
#include <eta/runtime/memory/intern_table.h>
#include <eta/runtime/builtin_env.h>
#include <eta/runtime/builtin_names.h>
#include <eta/runtime/os_primitives.h>
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
 * Verify that register_builtin_names() (the SSoT) contains entries for
 * every builtin that the runtime modules register.
 *
 * We check os, time, torch, stats, and log individually (os/log require a live VM;
 * the others accept a null VM
 * pointer).
 * Port/IO/NNG require a live VM or driver-specific args, so full end-to-end
 * coverage is provided by the Driver constructor's verify_all_patched() call.
 */
BOOST_AUTO_TEST_CASE(names_ssot_contains_os_time_torch_stats_and_log) {
    /// 1. Names-only environment via the SSoT
    BuiltinEnvironment names_env;
    register_builtin_names(names_env);

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

    /// 3. Time primitives
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

    /// 4. Torch primitives
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

    /// 5. Stats primitives
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

    /// 6. Log primitives
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
 * Verify basic properties of the SSoT: non-empty, no duplicate names.
 */
BOOST_AUTO_TEST_CASE(names_ssot_no_duplicates) {
    BuiltinEnvironment env;
    register_builtin_names(env);

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
