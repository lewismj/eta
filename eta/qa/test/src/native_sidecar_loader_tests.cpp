/**
 * @file native_sidecar_loader_tests.cpp
 * @brief Unit tests for native sidecar loader skeleton and path resolution.
 */

#include <boost/test/unit_test.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "eta/native/extension_registry.h"
#include "eta/native/runtime_binding.h"
#include "eta/native/sidecar_loader.h"
#include "eta/runtime/memory/heap.h"

namespace fs = std::filesystem;

#ifndef ETA_TEST_NATIVE_SIDECAR_PATH
#error ETA_TEST_NATIVE_SIDECAR_PATH must be defined by CMake
#endif

namespace {

/**
 * @brief Temporary directory guard used by filesystem-based loader tests.
 */
struct TempDir {
    fs::path path;

    TempDir() {
        const auto suffix = std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count());
        path = fs::temp_directory_path() / ("eta_native_loader_test_" + suffix);
        fs::create_directories(path);
    }

    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }

    fs::path write_file(const std::string& rel, const std::string& content) const {
        const auto full = path / rel;
        fs::create_directories(full.parent_path());
        std::ofstream out(full, std::ios::out | std::ios::binary | std::ios::trunc);
        out << content;
        return full;
    }
};

[[nodiscard]] eta::native::NativeLoadContext make_context_for_test(const fs::path& root) {
    eta::native::NativeLoadContext context;
    context.context_kind = eta::package::ManifestContextKind::StandalonePackage;
    context.active_manifest_path = root / "eta.toml";
    context.lockfile_root = root;
    context.modules_root = root / ".eta" / "modules";
    return context;
}

[[nodiscard]] fs::path sidecar_fixture_path() {
    return fs::path(ETA_TEST_NATIVE_SIDECAR_PATH);
}

} // namespace

BOOST_AUTO_TEST_SUITE(native_sidecar_loader_tests)

BOOST_AUTO_TEST_CASE(build_native_load_context_resolves_standalone_package_roots) {
    TempDir temp;
    temp.write_file("app/eta.toml", R"toml(
[package]
name = "app"
version = "0.1.0"
license = "MIT"

[compatibility]
eta = ">=0.6, <0.8"

[dependencies]
lib = { path = "../lib" }
)toml");
    temp.write_file("lib/eta.toml", R"toml(
[package]
name = "lib"
version = "0.1.0"
license = "MIT"

[compatibility]
eta = ">=0.6, <0.8"

[dependencies]
)toml");

    const auto context = eta::native::build_native_load_context(temp.path / "app" / "src");
    BOOST_REQUIRE(context.has_value());
    BOOST_TEST(static_cast<int>(context->context_kind)
               == static_cast<int>(eta::package::ManifestContextKind::StandalonePackage));
    BOOST_TEST(context->lockfile_root == fs::weakly_canonical(temp.path / "app"));
    BOOST_TEST(context->modules_root == context->lockfile_root / ".eta" / "modules");
    BOOST_TEST(context->package_root_by_name.contains("app"));
    BOOST_TEST(context->package_root_by_name.contains("lib"));
}

BOOST_AUTO_TEST_CASE(build_native_load_context_uses_workspace_roots_for_member_start_dir) {
    TempDir temp;
    temp.write_file("ws/eta.toml", R"toml(
[workspace]
members = ["packages/*"]
)toml");
    temp.write_file("ws/packages/app/eta.toml", R"toml(
[package]
name = "app"
version = "0.1.0"
license = "MIT"

[compatibility]
eta = ">=0.6, <0.8"

[dependencies]
lib = { path = "../lib" }
)toml");
    temp.write_file("ws/packages/lib/eta.toml", R"toml(
[package]
name = "lib"
version = "0.1.0"
license = "MIT"

[compatibility]
eta = ">=0.6, <0.8"

[dependencies]
)toml");

    const auto context =
        eta::native::build_native_load_context(temp.path / "ws" / "packages" / "app" / "src");
    BOOST_REQUIRE(context.has_value());
    BOOST_TEST(static_cast<int>(context->context_kind)
               == static_cast<int>(eta::package::ManifestContextKind::WorkspaceMember));
    BOOST_TEST(context->lockfile_root == fs::weakly_canonical(temp.path / "ws"));
    BOOST_TEST(context->modules_root == context->lockfile_root / ".eta" / "modules");
    BOOST_TEST(context->package_root_by_name.contains("app"));
    BOOST_TEST(context->package_root_by_name.contains("lib"));
}

BOOST_AUTO_TEST_CASE(resolve_native_sidecars_rejects_artifact_escape) {
    TempDir temp;
    const auto package_root = temp.path / "pkg";
    fs::create_directories(package_root);

    auto context = make_context_for_test(temp.path);
    context.package_root_by_name.emplace("pkg", package_root);

    eta::native::NativeSidecarSpec sidecar;
    sidecar.package_name = "pkg";
    sidecar.artifact_relpath = fs::path("..") / "escape.dll";

    const auto resolved = eta::native::resolve_native_sidecars(context, {&sidecar, 1u});
    BOOST_REQUIRE(!resolved.has_value());
    BOOST_TEST(static_cast<int>(resolved.error().code)
               == static_cast<int>(
                   eta::native::SidecarLoaderError::Code::ArtifactPathEscapesPackageRoot));
}

BOOST_AUTO_TEST_CASE(resolve_native_sidecars_preserves_input_order) {
    TempDir temp;
    const auto alpha_root = temp.path / "alpha";
    const auto beta_root = temp.path / "beta";
    fs::create_directories(alpha_root);
    fs::create_directories(beta_root);
    std::ofstream(alpha_root / "alpha.dll").put('\n');
    std::ofstream(beta_root / "beta.dll").put('\n');

    auto context = make_context_for_test(temp.path);
    context.package_root_by_name.emplace("alpha", alpha_root);
    context.package_root_by_name.emplace("beta", beta_root);

    eta::native::NativeSidecarSpec first;
    first.package_name = "beta";
    first.artifact_relpath = "beta.dll";

    eta::native::NativeSidecarSpec second;
    second.package_name = "alpha";
    second.artifact_relpath = "alpha.dll";

    const std::vector<eta::native::NativeSidecarSpec> sidecars{first, second};
    const auto resolved = eta::native::resolve_native_sidecars(context, sidecars);
    BOOST_REQUIRE(resolved.has_value());
    BOOST_REQUIRE_EQUAL(resolved->size(), 2u);
    BOOST_TEST((*resolved)[0].spec.package_name == "beta");
    BOOST_TEST((*resolved)[1].spec.package_name == "alpha");

    const auto resolved_again = eta::native::resolve_native_sidecars(context, sidecars);
    BOOST_REQUIRE(resolved_again.has_value());
    BOOST_REQUIRE_EQUAL(resolved_again->size(), 2u);
    BOOST_TEST((*resolved_again)[0].artifact_path == (*resolved)[0].artifact_path);
    BOOST_TEST((*resolved_again)[1].artifact_path == (*resolved)[1].artifact_path);
}

BOOST_AUTO_TEST_CASE(sidecar_loader_reports_open_failure_for_missing_library) {
    TempDir temp;
    eta::native::ExtensionRegistry registry;
    eta::native::SidecarLoader loader(registry);

    eta::native::ResolvedNativeSidecar sidecar;
    sidecar.spec.package_name = "pkg";
    sidecar.spec.artifact_relpath = "missing.dll";
    sidecar.package_root = temp.path / "pkg";
    sidecar.artifact_path = sidecar.package_root / "missing.dll";

    const auto loaded = loader.load(sidecar);
    BOOST_REQUIRE(!loaded.has_value());
    BOOST_TEST(static_cast<int>(loaded.error().code)
               == static_cast<int>(eta::native::SidecarLoaderError::Code::LibraryOpenFailed));
}

BOOST_AUTO_TEST_CASE(sidecar_loader_reports_checksum_mismatch) {
    const auto fixture = sidecar_fixture_path();
    BOOST_REQUIRE_MESSAGE(fs::is_regular_file(fixture),
                          "missing sidecar fixture binary: " + fixture.string());

    eta::native::ExtensionRegistry registry;
    eta::native::SidecarLoader loader(registry);

    eta::native::ResolvedNativeSidecar sidecar;
    sidecar.spec.package_name = "pkg";
    sidecar.spec.artifact_relpath = fixture.filename();
    sidecar.spec.expected_sha256 =
        "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
    sidecar.package_root = fixture.parent_path();
    sidecar.artifact_path = fixture;

    const auto loaded = loader.load(sidecar);
    BOOST_REQUIRE(!loaded.has_value());
    BOOST_TEST(static_cast<int>(loaded.error().code)
               == static_cast<int>(eta::native::SidecarLoaderError::Code::ChecksumMismatch));
}

BOOST_AUTO_TEST_CASE(sidecar_loader_reports_missing_entrypoint) {
    const auto fixture = sidecar_fixture_path();
    BOOST_REQUIRE_MESSAGE(fs::is_regular_file(fixture),
                          "missing sidecar fixture binary: " + fixture.string());

    eta::native::ExtensionRegistry registry;
    eta::native::SidecarLoader loader(registry);

    eta::native::ResolvedNativeSidecar sidecar;
    sidecar.spec.package_name = "pkg";
    sidecar.spec.artifact_relpath = fixture.filename();
    sidecar.spec.entrypoint = "eta_register_extension_missing";
    sidecar.package_root = fixture.parent_path();
    sidecar.artifact_path = fixture;

    const auto loaded = loader.load(sidecar);
    BOOST_REQUIRE(!loaded.has_value());
    BOOST_TEST(static_cast<int>(loaded.error().code)
               == static_cast<int>(eta::native::SidecarLoaderError::Code::SymbolLookupFailed));
}

BOOST_AUTO_TEST_CASE(sidecar_loader_reports_abi_mismatch) {
    const auto fixture = sidecar_fixture_path();
    BOOST_REQUIRE_MESSAGE(fs::is_regular_file(fixture),
                          "missing sidecar fixture binary: " + fixture.string());

    eta::native::ExtensionRegistry registry;
    eta::native::SidecarLoader loader(registry);

    eta::native::ResolvedNativeSidecar sidecar;
    sidecar.spec.package_name = "pkg";
    sidecar.spec.artifact_relpath = fixture.filename();
    sidecar.spec.entrypoint = "eta_register_extension_wrong_abi";
    sidecar.package_root = fixture.parent_path();
    sidecar.artifact_path = fixture;

    const auto loaded = loader.load(sidecar);
    BOOST_REQUIRE(!loaded.has_value());
    BOOST_TEST(static_cast<int>(loaded.error().code)
               == static_cast<int>(eta::native::SidecarLoaderError::Code::AbiMismatch));
}

BOOST_AUTO_TEST_CASE(sidecar_loader_maps_registration_failure_to_loader_error) {
    const auto fixture = sidecar_fixture_path();
    BOOST_REQUIRE_MESSAGE(fs::is_regular_file(fixture),
                          "missing sidecar fixture binary: " + fixture.string());

    eta::native::ExtensionRegistry registry;
    eta::native::SidecarLoader loader(registry);

    eta::native::ResolvedNativeSidecar sidecar;
    sidecar.spec.package_name = "pkg";
    sidecar.spec.artifact_relpath = fixture.filename();
    sidecar.spec.entrypoint = "eta_register_extension_fail";
    sidecar.package_root = fixture.parent_path();
    sidecar.artifact_path = fixture;

    const auto loaded = loader.load(sidecar);
    BOOST_REQUIRE(!loaded.has_value());
    BOOST_TEST(static_cast<int>(loaded.error().code)
               == static_cast<int>(
                   eta::native::SidecarLoaderError::Code::ExtensionRegistrationFailed));
    BOOST_TEST(loaded.error().message.find("intentional sidecar failure") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(sidecar_loader_registers_extension_metadata_and_symbols) {
    const auto fixture = sidecar_fixture_path();
    BOOST_REQUIRE_MESSAGE(fs::is_regular_file(fixture),
                          "missing sidecar fixture binary: " + fixture.string());

    eta::native::ExtensionRegistry registry;
    eta::native::SidecarLoader loader(registry);

    eta::native::ResolvedNativeSidecar sidecar;
    sidecar.spec.package_name = "pkg";
    sidecar.spec.artifact_relpath = fixture.filename();
    sidecar.package_root = fixture.parent_path();
    sidecar.artifact_path = fixture;

    const auto loaded = loader.load(sidecar);
    BOOST_REQUIRE(loaded.has_value());
    BOOST_TEST(loader.loaded_library_count() == 1u);

    const auto* extension = registry.find_extension("eta.test.sidecar");
    BOOST_REQUIRE(extension != nullptr);
    BOOST_TEST(extension->abi == "eta-native-v1");
    BOOST_REQUIRE_EQUAL(extension->symbols.size(), 2u);
    BOOST_TEST(extension->symbols[0].name == "native.test.add");
    BOOST_TEST(extension->symbols[1].name == "native.test.rest");

    const auto owner = registry.symbol_owner("native.test.add");
    BOOST_REQUIRE(owner.has_value());
    BOOST_TEST(*owner == "eta.test.sidecar");
}

BOOST_AUTO_TEST_CASE(sidecar_loader_registers_builtin_sidecar_entrypoints) {
    const auto fixture = sidecar_fixture_path();
    BOOST_REQUIRE_MESSAGE(fs::is_regular_file(fixture),
                          "missing sidecar fixture binary: " + fixture.string());

    struct EntryProbe {
        const char* entrypoint;
        const char* extension_id;
        const char* symbol_name;
    };

    const std::vector<EntryProbe> probes{
        {"eta_register_log_extension_v1", "eta.log.sidecar", "%log-default-logger"},
        {"eta_register_stats_extension_v1", "eta.stats.sidecar", "%stats-mean-vec"},
        {"eta_register_torch_extension_v1", "eta.torch.sidecar", "torch/tensor"},
        {"eta_register_nng_extension_v1", "eta.nng.sidecar", "nng-socket"},
    };

    eta::native::ExtensionRegistry registry;
    eta::native::SidecarLoader loader(registry);

    for (const auto& probe : probes) {
        eta::native::ResolvedNativeSidecar sidecar;
        sidecar.spec.package_name = probe.extension_id;
        sidecar.spec.artifact_relpath = fixture.filename();
        sidecar.spec.entrypoint = probe.entrypoint;
        sidecar.package_root = fixture.parent_path();
        sidecar.artifact_path = fixture;

        const auto loaded = loader.load(sidecar);
        BOOST_REQUIRE_MESSAGE(
            loaded.has_value(),
            "failed to load sidecar entrypoint " << probe.entrypoint);

        const auto* extension = registry.find_extension(probe.extension_id);
        BOOST_REQUIRE(extension != nullptr);
        BOOST_TEST(extension->abi == "eta-native-v1");
        BOOST_TEST(!extension->symbols.empty());

        const auto owner = registry.symbol_owner(probe.symbol_name);
        BOOST_REQUIRE(owner.has_value());
        BOOST_TEST(*owner == probe.extension_id);
    }
}

BOOST_AUTO_TEST_CASE(sidecar_loader_native_object_api_gate_accepts_current_runtime) {
    const auto fixture = sidecar_fixture_path();
    BOOST_REQUIRE_MESSAGE(fs::is_regular_file(fixture),
                          "missing sidecar fixture binary: " + fixture.string());

    eta::native::ExtensionRegistry registry;
    eta::native::SidecarLoader loader(registry);
    eta::runtime::memory::heap::Heap heap(1ull << 20);
    eta::native::SidecarRuntimeBindingV1 binding{};
    binding.heap = &heap;
    loader.set_runtime_context(&binding);

    eta::native::ResolvedNativeSidecar sidecar;
    sidecar.spec.package_name = "pkg";
    sidecar.spec.artifact_relpath = fixture.filename();
    sidecar.spec.entrypoint = "eta_register_native_object_gate_extension_v1";
    sidecar.package_root = fixture.parent_path();
    sidecar.artifact_path = fixture;

    const auto loaded = loader.load(sidecar);
    BOOST_REQUIRE(loaded.has_value());

    const auto* extension = registry.find_extension("eta.native.gate.sidecar");
    BOOST_REQUIRE(extension != nullptr);
    BOOST_TEST(extension->abi == "eta-native-v1");
}

BOOST_AUTO_TEST_CASE(sidecar_loader_native_object_api_gate_rejects_legacy_runtime_gracefully) {
    const auto fixture = sidecar_fixture_path();
    BOOST_REQUIRE_MESSAGE(fs::is_regular_file(fixture),
                          "missing sidecar fixture binary: " + fixture.string());

    eta::native::ExtensionRegistry registry;
    eta::native::SidecarLoader loader(registry);
    eta::runtime::memory::heap::Heap heap(1ull << 20);
    eta::native::SidecarRuntimeBindingV1 binding{};
    binding.heap = &heap;
    loader.set_runtime_context(&binding);

    eta::native::ResolvedNativeSidecar sidecar;
    sidecar.spec.package_name = "pkg";
    sidecar.spec.artifact_relpath = fixture.filename();
    sidecar.spec.entrypoint = "eta_register_native_object_gate_extension_legacy_runtime_v1";
    sidecar.package_root = fixture.parent_path();
    sidecar.artifact_path = fixture;

    const auto loaded = loader.load(sidecar);
    BOOST_REQUIRE(!loaded.has_value());
    BOOST_TEST(static_cast<int>(loaded.error().code)
               == static_cast<int>(
                   eta::native::SidecarLoaderError::Code::ExtensionRegistrationFailed));
    BOOST_TEST(
        loaded.error().message.find("native-object-api-unavailable") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(sidecar_loader_native_object_alloc_get_roundtrip_and_vtable_guard) {
    const auto fixture = sidecar_fixture_path();
    BOOST_REQUIRE_MESSAGE(fs::is_regular_file(fixture),
                          "missing sidecar fixture binary: " + fixture.string());

    eta::native::ExtensionRegistry registry;
    eta::native::SidecarLoader loader(registry);
    eta::runtime::memory::heap::Heap heap(1ull << 20);
    eta::native::SidecarRuntimeBindingV1 binding{};
    binding.heap = &heap;
    loader.set_runtime_context(&binding);

    eta::native::ResolvedNativeSidecar sidecar;
    sidecar.spec.package_name = "pkg";
    sidecar.spec.artifact_relpath = fixture.filename();
    sidecar.spec.entrypoint = "eta_register_native_object_roundtrip_extension_v1";
    sidecar.package_root = fixture.parent_path();
    sidecar.artifact_path = fixture;

    const auto loaded = loader.load(sidecar);
    BOOST_REQUIRE(loaded.has_value());

    std::size_t native_object_count = 0;
    heap.for_each_entry(
        [&](eta::runtime::memory::heap::ObjectId, eta::runtime::memory::heap::HeapEntry& entry) {
            if (entry.header.kind == eta::runtime::memory::heap::ObjectKind::NativeObject) {
                ++native_object_count;
            }
        });
    BOOST_TEST(native_object_count == 1u);

    const auto* extension = registry.find_extension("eta.native.roundtrip.sidecar");
    BOOST_REQUIRE(extension != nullptr);
}

BOOST_AUTO_TEST_CASE(sidecar_loader_native_object_alloc_accepts_trace_vtable) {
    const auto fixture = sidecar_fixture_path();
    BOOST_REQUIRE_MESSAGE(fs::is_regular_file(fixture),
                          "missing sidecar fixture binary: " + fixture.string());

    eta::native::ExtensionRegistry registry;
    eta::native::SidecarLoader loader(registry);
    eta::runtime::memory::heap::Heap heap(1ull << 20);
    eta::native::SidecarRuntimeBindingV1 binding{};
    binding.heap = &heap;
    loader.set_runtime_context(&binding);

    eta::native::ResolvedNativeSidecar sidecar;
    sidecar.spec.package_name = "pkg";
    sidecar.spec.artifact_relpath = fixture.filename();
    sidecar.spec.entrypoint = "eta_register_native_object_trace_extension_v1";
    sidecar.package_root = fixture.parent_path();
    sidecar.artifact_path = fixture;

    const auto loaded = loader.load(sidecar);
    BOOST_REQUIRE(loaded.has_value());

    std::size_t native_object_count = 0;
    heap.for_each_entry(
        [&](eta::runtime::memory::heap::ObjectId, eta::runtime::memory::heap::HeapEntry& entry) {
            if (entry.header.kind == eta::runtime::memory::heap::ObjectKind::NativeObject) {
                ++native_object_count;
            }
        });
    BOOST_TEST(native_object_count == 1u);

    const auto* extension = registry.find_extension("eta.native.trace.sidecar");
    BOOST_REQUIRE(extension != nullptr);
}

BOOST_AUTO_TEST_SUITE_END()
