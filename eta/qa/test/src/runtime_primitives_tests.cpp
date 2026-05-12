/**
 * @file runtime_primitives_tests.cpp
 * @brief Unit tests for runtime primitive bootstrap helpers.
 */

#include <boost/test/unit_test.hpp>

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "eta/runtime/builtin_env.h"
#include "eta/runtime/extension_env.h"
#include "eta/runtime/memory/heap.h"
#include "eta/runtime/nanbox.h"
#include "eta/runtime/types/primitive.h"
#include "eta/session/runtime_primitives.h"

namespace {

eta::runtime::types::PrimitiveFunc make_const_primitive(std::int64_t value) {
    return [value](eta::runtime::types::PrimitiveArgs) {
        return eta::runtime::nanbox::ops::encode(value);
    };
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

BOOST_AUTO_TEST_SUITE_END()

