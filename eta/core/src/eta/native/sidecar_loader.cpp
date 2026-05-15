#include "eta/native/sidecar_loader.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <string_view>
#include <unordered_set>
#include <utility>

#include "eta/native/sdk.h"
#include "eta/native/runtime_binding.h"
#include "eta/package/lockfile.h"
#include "eta/package/resolver.h"
#include "eta/runtime/nanbox.h"
#include "eta/util/path.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace eta::native {

namespace {

[[nodiscard]] std::string lower_ascii(std::string value) {
    std::transform(value.begin(),
                   value.end(),
                   value.begin(),
                   [](const char c) {
                       return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                   });
    return value;
}

[[nodiscard]] std::expected<void, SidecarLoaderError> unexpected_with(
    const SidecarLoaderError::Code code,
    std::string message) {
    return std::unexpected(SidecarLoaderError{code, std::move(message)});
}

#if defined(_WIN32)
[[nodiscard]] std::string win32_error_message(const DWORD error_code) {
    LPSTR buffer = nullptr;
    const DWORD size = FormatMessageA(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr,
        error_code,
        MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        reinterpret_cast<LPSTR>(&buffer),
        0,
        nullptr);
    std::string message;
    if (size > 0 && buffer != nullptr) {
        message.assign(buffer, buffer + size);
        while (!message.empty() && (message.back() == '\n' || message.back() == '\r')) {
            message.pop_back();
        }
    } else {
        message = "Win32 error "
            + std::to_string(static_cast<unsigned long long>(error_code));
    }
    if (buffer != nullptr) LocalFree(buffer);
    return message;
}

struct Win32LibraryLoadResult {
    HMODULE handle{nullptr};
    DWORD error_code{ERROR_SUCCESS};
};

[[nodiscard]] Win32LibraryLoadResult load_library_with_dependencies(
    const fs::path& artifact_path) {
#if defined(LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR) && defined(LOAD_LIBRARY_SEARCH_DEFAULT_DIRS)
    constexpr DWORD kLoadFlags = LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS;
    if (HMODULE handle = LoadLibraryExW(artifact_path.c_str(), nullptr, kLoadFlags);
        handle != nullptr) {
        return {handle, ERROR_SUCCESS};
    }

    const DWORD first_error = GetLastError();
    if (first_error != ERROR_INVALID_PARAMETER && first_error != ERROR_CALL_NOT_IMPLEMENTED) {
        return {nullptr, first_error};
    }
#endif

    if (HMODULE handle =
            LoadLibraryExW(artifact_path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        handle != nullptr) {
        return {handle, ERROR_SUCCESS};
    }
    return {nullptr, GetLastError()};
}
#endif

struct Sha256Context {
    std::array<std::uint32_t, 8u> state{
        0x6a09e667u,
        0xbb67ae85u,
        0x3c6ef372u,
        0xa54ff53au,
        0x510e527fu,
        0x9b05688cu,
        0x1f83d9abu,
        0x5be0cd19u,
    };
    std::uint64_t total_bytes{0};
    std::array<std::uint8_t, 64u> buffer{};
    std::size_t buffer_size{0};
};

constexpr std::array<std::uint32_t, 64u> kSha256RoundConstants{
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u,
    0xab1c5ed5u, 0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu,
    0x9bdc06a7u, 0xc19bf174u, 0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu,
    0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau, 0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u,
    0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u, 0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu,
    0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u, 0xa2bfe8a1u, 0xa81a664bu,
    0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u, 0x19a4c116u,
    0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u,
    0xc67178f2u,
};

[[nodiscard]] constexpr std::uint32_t rotr32(const std::uint32_t value, const std::uint32_t shift) {
    return (value >> shift) | (value << (32u - shift));
}

[[nodiscard]] constexpr std::uint32_t sha256_choice(const std::uint32_t x,
                                                    const std::uint32_t y,
                                                    const std::uint32_t z) {
    return (x & y) ^ ((~x) & z);
}

[[nodiscard]] constexpr std::uint32_t sha256_majority(const std::uint32_t x,
                                                      const std::uint32_t y,
                                                      const std::uint32_t z) {
    return (x & y) ^ (x & z) ^ (y & z);
}

[[nodiscard]] constexpr std::uint32_t sha256_big_sigma0(const std::uint32_t value) {
    return rotr32(value, 2u) ^ rotr32(value, 13u) ^ rotr32(value, 22u);
}

[[nodiscard]] constexpr std::uint32_t sha256_big_sigma1(const std::uint32_t value) {
    return rotr32(value, 6u) ^ rotr32(value, 11u) ^ rotr32(value, 25u);
}

[[nodiscard]] constexpr std::uint32_t sha256_small_sigma0(const std::uint32_t value) {
    return rotr32(value, 7u) ^ rotr32(value, 18u) ^ (value >> 3u);
}

[[nodiscard]] constexpr std::uint32_t sha256_small_sigma1(const std::uint32_t value) {
    return rotr32(value, 17u) ^ rotr32(value, 19u) ^ (value >> 10u);
}

void sha256_transform(Sha256Context& context, const std::array<std::uint8_t, 64u>& chunk) {
    std::array<std::uint32_t, 64u> schedule{};
    for (std::size_t i = 0; i < 16u; ++i) {
        const std::size_t offset = i * 4u;
        schedule[i] = (static_cast<std::uint32_t>(chunk[offset]) << 24u)
            | (static_cast<std::uint32_t>(chunk[offset + 1u]) << 16u)
            | (static_cast<std::uint32_t>(chunk[offset + 2u]) << 8u)
            | static_cast<std::uint32_t>(chunk[offset + 3u]);
    }
    for (std::size_t i = 16u; i < schedule.size(); ++i) {
        schedule[i] =
            sha256_small_sigma1(schedule[i - 2u]) + schedule[i - 7u]
            + sha256_small_sigma0(schedule[i - 15u]) + schedule[i - 16u];
    }

    std::uint32_t a = context.state[0];
    std::uint32_t b = context.state[1];
    std::uint32_t c = context.state[2];
    std::uint32_t d = context.state[3];
    std::uint32_t e = context.state[4];
    std::uint32_t f = context.state[5];
    std::uint32_t g = context.state[6];
    std::uint32_t h = context.state[7];

    for (std::size_t i = 0; i < schedule.size(); ++i) {
        const std::uint32_t temp1 =
            h + sha256_big_sigma1(e) + sha256_choice(e, f, g) + kSha256RoundConstants[i]
            + schedule[i];
        const std::uint32_t temp2 = sha256_big_sigma0(a) + sha256_majority(a, b, c);
        h = g;
        g = f;
        f = e;
        e = d + temp1;
        d = c;
        c = b;
        b = a;
        a = temp1 + temp2;
    }

    context.state[0] += a;
    context.state[1] += b;
    context.state[2] += c;
    context.state[3] += d;
    context.state[4] += e;
    context.state[5] += f;
    context.state[6] += g;
    context.state[7] += h;
}

void sha256_update(Sha256Context& context,
                   const std::uint8_t* data,
                   const std::size_t size) {
    context.total_bytes += static_cast<std::uint64_t>(size);
    std::size_t offset = 0;
    while (offset < size) {
        const std::size_t remaining = size - offset;
        const std::size_t copy_count =
            (std::min)(remaining, context.buffer.size() - context.buffer_size);
        std::copy_n(data + offset, copy_count, context.buffer.data() + context.buffer_size);
        context.buffer_size += copy_count;
        offset += copy_count;

        if (context.buffer_size == context.buffer.size()) {
            sha256_transform(context, context.buffer);
            context.buffer_size = 0;
        }
    }
}

[[nodiscard]] std::array<std::uint8_t, 32u> sha256_finalize(Sha256Context& context) {
    const std::uint64_t total_bits = context.total_bytes * 8u;

    context.buffer[context.buffer_size++] = 0x80u;
    if (context.buffer_size > 56u) {
        while (context.buffer_size < context.buffer.size()) {
            context.buffer[context.buffer_size++] = 0u;
        }
        sha256_transform(context, context.buffer);
        context.buffer_size = 0;
    }

    while (context.buffer_size < 56u) {
        context.buffer[context.buffer_size++] = 0u;
    }

    for (std::size_t i = 0; i < 8u; ++i) {
        const auto shift = static_cast<std::uint32_t>((7u - i) * 8u);
        context.buffer[56u + i] = static_cast<std::uint8_t>((total_bits >> shift) & 0xffu);
    }
    sha256_transform(context, context.buffer);

    std::array<std::uint8_t, 32u> digest{};
    for (std::size_t i = 0; i < context.state.size(); ++i) {
        const std::uint32_t word = context.state[i];
        digest[i * 4u] = static_cast<std::uint8_t>((word >> 24u) & 0xffu);
        digest[i * 4u + 1u] = static_cast<std::uint8_t>((word >> 16u) & 0xffu);
        digest[i * 4u + 2u] = static_cast<std::uint8_t>((word >> 8u) & 0xffu);
        digest[i * 4u + 3u] = static_cast<std::uint8_t>(word & 0xffu);
    }
    return digest;
}

[[nodiscard]] std::string to_lower_hex(const std::array<std::uint8_t, 32u>& digest) {
    constexpr std::array<char, 16u> kHex{
        '0', '1', '2', '3', '4', '5', '6', '7',
        '8', '9', 'a', 'b', 'c', 'd', 'e', 'f',
    };

    std::string text;
    text.reserve(digest.size() * 2u);
    for (const auto byte : digest) {
        text.push_back(kHex[(byte >> 4u) & 0x0fu]);
        text.push_back(kHex[byte & 0x0fu]);
    }
    return text;
}

[[nodiscard]] std::expected<std::string, SidecarLoaderError> compute_sha256_file_impl(
    const fs::path& file_path) {
    std::ifstream in(file_path, std::ios::binary);
    if (!in.is_open()) {
        return std::unexpected(SidecarLoaderError{
            SidecarLoaderError::Code::ChecksumMismatch,
            "failed to open sidecar for checksum verification: " + file_path.string(),
        });
    }

    Sha256Context context;
    std::array<char, 4096u> chunk{};
    while (in.good()) {
        in.read(chunk.data(), static_cast<std::streamsize>(chunk.size()));
        const auto bytes_read = in.gcount();
        if (bytes_read <= 0) break;
        sha256_update(context,
                      reinterpret_cast<const std::uint8_t*>(chunk.data()),
                      static_cast<std::size_t>(bytes_read));
    }
    if (in.bad()) {
        return std::unexpected(SidecarLoaderError{
            SidecarLoaderError::Code::ChecksumMismatch,
            "failed while reading sidecar for checksum verification: " + file_path.string(),
        });
    }

    return to_lower_hex(sha256_finalize(context));
}

struct PendingRegistration {
    std::vector<ExtensionSymbolDescriptor> symbols;
    std::string reported_error;
};

int register_primitive_bridge(void* user_data,
                              const char* name,
                              std::uint32_t arity,
                              std::uint8_t has_rest,
                              void* callable) {
    auto* pending = static_cast<PendingRegistration*>(user_data);
    if (pending == nullptr || name == nullptr || name[0] == '\0') {
        return ETA_NATIVE_STATUS_ERROR;
    }

    ExtensionSymbolDescriptor descriptor;
    descriptor.name = name;
    descriptor.arity = arity;
    descriptor.has_rest = has_rest != 0;
    descriptor.callable = callable;
    pending->symbols.push_back(std::move(descriptor));
    return ETA_NATIVE_STATUS_OK;
}

void report_error_bridge(void* user_data, const char* message) {
    auto* pending = static_cast<PendingRegistration*>(user_data);
    if (pending == nullptr || message == nullptr) return;
    pending->reported_error = message;
}

[[nodiscard]] eta::native::SidecarRuntimeBindingV1* runtime_binding_from_context(
    void* runtime_context) {
    if (runtime_context == nullptr) return nullptr;
    auto* binding = static_cast<eta::native::SidecarRuntimeBindingV1*>(runtime_context);
    if (binding->heap == nullptr) return nullptr;
    return binding;
}

int alloc_native_object_bridge(void* runtime_context,
                               const EtaNativeObjectVTable* vtable,
                               void* payload,
                               std::uint64_t* out_val) {
    auto* binding = runtime_binding_from_context(runtime_context);
    if (binding == nullptr || vtable == nullptr || out_val == nullptr) {
        return ETA_NATIVE_STATUS_ERROR;
    }

    auto allocated = binding->heap->allocate<
        eta::runtime::memory::heap::NativeObjectHeader,
        eta::runtime::memory::heap::ObjectKind::NativeObject>(
        eta::runtime::memory::heap::NativeObjectHeader{vtable, payload});
    if (!allocated.has_value()) return ETA_NATIVE_STATUS_ERROR;

    *out_val = eta::runtime::nanbox::ops::box(
        eta::runtime::nanbox::Tag::HeapObject,
        static_cast<eta::runtime::nanbox::LispVal>(*allocated));
    return ETA_NATIVE_STATUS_OK;
}

void* get_native_object_bridge(void* runtime_context,
                               const std::uint64_t raw_val,
                               const EtaNativeObjectVTable* vtable) {
    auto* binding = runtime_binding_from_context(runtime_context);
    if (binding == nullptr || vtable == nullptr) return nullptr;

    const eta::runtime::nanbox::LispVal value = raw_val;
    if (!eta::runtime::nanbox::ops::is_boxed(value)
        || eta::runtime::nanbox::ops::tag(value) != eta::runtime::nanbox::Tag::HeapObject) {
        return nullptr;
    }

    auto* native_header = binding->heap->try_get_as<
        eta::runtime::memory::heap::ObjectKind::NativeObject,
        eta::runtime::memory::heap::NativeObjectHeader>(eta::runtime::nanbox::ops::payload(value));
    if (native_header == nullptr || native_header->vtable != vtable) return nullptr;
    return native_header->user_data;
}

[[nodiscard]] std::expected<void, SidecarLoaderError> validate_registration_conflicts(
    const ExtensionRegistry& registry,
    std::string_view extension_id,
    std::span<const ExtensionSymbolDescriptor> symbols) {
    if (registry.find_extension(extension_id) != nullptr) {
        return unexpected_with(
            SidecarLoaderError::Code::RegistryConflict,
            "duplicate extension id '" + std::string(extension_id) + "'");
    }

    std::unordered_set<std::string> seen_symbols;
    seen_symbols.reserve(symbols.size());
    for (const auto& symbol : symbols) {
        if (!seen_symbols.insert(symbol.name).second) {
            return unexpected_with(
                SidecarLoaderError::Code::RegistryConflict,
                "duplicate symbol '" + symbol.name + "' in extension '"
                    + std::string(extension_id) + "'");
        }
        if (auto owner = registry.symbol_owner(symbol.name); owner.has_value()) {
            return unexpected_with(
                SidecarLoaderError::Code::RegistryConflict,
                "duplicate symbol '" + symbol.name + "' already owned by '"
                    + std::string(*owner) + "'");
        }
    }
    return {};
}

} // namespace

std::expected<std::string, SidecarLoaderError> compute_sidecar_sha256(
    const fs::path& file_path) {
    return compute_sha256_file_impl(file_path);
}

bool is_path_within(const fs::path& root, const fs::path& candidate) {
    const auto root_key = util::canonical_path_key(root);
    const auto candidate_key = util::canonical_path_key(candidate);
    if (candidate_key == root_key) return true;
    if (candidate_key.size() <= root_key.size()) return false;
    if (candidate_key.compare(0u, root_key.size(), root_key) != 0) return false;
    if (!root_key.empty() && (root_key.back() == '/' || root_key.back() == '\\')) return true;
    const char boundary = candidate_key[root_key.size()];
    return boundary == '/' || boundary == '\\';
}

NativeLoadContextResult build_native_load_context(const fs::path& start_dir) {
    auto discovery = package::discover_manifest_context(start_dir);
    if (!discovery) {
        return std::unexpected(SidecarLoaderError{
            SidecarLoaderError::Code::ManifestDiscoveryFailed,
            discovery.error().message,
        });
    }
    if (!discovery->context.has_value() || !discovery->active_manifest_path.has_value()) {
        return std::unexpected(SidecarLoaderError{
            SidecarLoaderError::Code::MissingPackageContext,
            "no package/workspace manifest context available for sidecar loading",
        });
    }

    NativeLoadContext context;
    context.context_kind = *discovery->context;
    context.active_manifest_path = util::canonicalize_path(*discovery->active_manifest_path);
    context.workspace_manifest_path = discovery->workspace_manifest_path;
    if (context.workspace_manifest_path.has_value()) {
        *context.workspace_manifest_path = util::canonicalize_path(*context.workspace_manifest_path);
        context.lockfile_root = context.workspace_manifest_path->parent_path();
    } else {
        context.lockfile_root = context.active_manifest_path.parent_path();
    }
    context.lockfile_root = util::canonicalize_path(context.lockfile_root);
    context.modules_root = context.lockfile_root / ".eta" / "modules";

    std::optional<package::Lockfile> lockfile;
    const auto lockfile_path = context.lockfile_root / "eta.lock";
    std::error_code lock_ec;
    if (fs::is_regular_file(lockfile_path, lock_ec) && !lock_ec) {
        auto lock = package::read_lockfile(lockfile_path);
        if (!lock) {
            return std::unexpected(SidecarLoaderError{
                SidecarLoaderError::Code::LockfileReadFailed,
                lock.error().message,
            });
        }
        lockfile = std::move(*lock);
    }

    package::ResolveOptions options;
    options.modules_root = context.modules_root;
    if (lockfile.has_value()) options.lockfile = &*lockfile;

    auto add_packages = [&](const package::ResolvedGraph& graph) {
        for (const auto& pkg : graph.packages) {
            context.package_root_by_name[pkg.name] = util::canonicalize_path(pkg.package_root);
        }
    };

    switch (context.context_kind) {
        case package::ManifestContextKind::StandalonePackage: {
            auto graph = package::resolve_dependencies(context.active_manifest_path, options);
            if (!graph) {
                return std::unexpected(SidecarLoaderError{
                    SidecarLoaderError::Code::DependencyResolutionFailed,
                    graph.error().message,
                });
            }
            add_packages(*graph);
            break;
        }
        case package::ManifestContextKind::WorkspaceRoot:
        case package::ManifestContextKind::WorkspaceMember:
        case package::ManifestContextKind::WorkspaceNonMember: {
            if (!context.workspace_manifest_path.has_value()) {
                return std::unexpected(SidecarLoaderError{
                    SidecarLoaderError::Code::WorkspaceResolutionFailed,
                    "workspace context missing workspace manifest path",
                });
            }

            auto workspace = package::resolve_workspace_members(*context.workspace_manifest_path);
            if (!workspace) {
                return std::unexpected(SidecarLoaderError{
                    SidecarLoaderError::Code::WorkspaceResolutionFailed,
                    workspace.error().message,
                });
            }
            auto graph = package::resolve_workspace_dependencies(*workspace, options);
            if (!graph) {
                return std::unexpected(SidecarLoaderError{
                    SidecarLoaderError::Code::DependencyResolutionFailed,
                    graph.error().message,
                });
            }
            add_packages(*graph);
            break;
        }
    }

    return context;
}

NativeSidecarResolutionResult resolve_native_sidecars(
    const NativeLoadContext& context,
    const std::span<const NativeSidecarSpec> sidecars) {
    std::vector<ResolvedNativeSidecar> resolved;
    resolved.reserve(sidecars.size());

    for (const auto& sidecar : sidecars) {
        const auto package_root_it = context.package_root_by_name.find(sidecar.package_name);
        if (package_root_it == context.package_root_by_name.end()) {
            return std::unexpected(SidecarLoaderError{
                SidecarLoaderError::Code::UnknownPackageRoot,
                "package root not found for sidecar package '" + sidecar.package_name + "'",
            });
        }

        if (sidecar.artifact_relpath.empty() || sidecar.artifact_relpath.is_absolute()) {
            return std::unexpected(SidecarLoaderError{
                SidecarLoaderError::Code::ArtifactPathNotRelative,
                "sidecar artifact path must be relative for package '" + sidecar.package_name + "'",
            });
        }

        const auto package_root = util::canonicalize_path(package_root_it->second);
        const auto artifact_path = util::canonicalize_path(package_root / sidecar.artifact_relpath);
        if (!is_path_within(package_root, artifact_path)) {
            return std::unexpected(SidecarLoaderError{
                SidecarLoaderError::Code::ArtifactPathEscapesPackageRoot,
                "sidecar artifact path escapes package root for package '" + sidecar.package_name
                    + "': " + sidecar.artifact_relpath.generic_string(),
            });
        }

        ResolvedNativeSidecar entry;
        entry.spec = sidecar;
        if (entry.spec.abi.empty()) entry.spec.abi = ETA_NATIVE_ABI_ID_V1;
        if (entry.spec.entrypoint.empty()) entry.spec.entrypoint = "eta_register_extension_v1";
        entry.package_root = package_root;
        entry.artifact_path = artifact_path;
        resolved.push_back(std::move(entry));
    }

    return resolved;
}

struct SidecarLoader::LoadedLibrary {
#if defined(_WIN32)
    HMODULE handle{nullptr};
#else
    void* handle{nullptr};
#endif
    fs::path path;

    LoadedLibrary() = default;
    LoadedLibrary(const LoadedLibrary&) = delete;
    LoadedLibrary& operator=(const LoadedLibrary&) = delete;
    LoadedLibrary(LoadedLibrary&&) = delete;
    LoadedLibrary& operator=(LoadedLibrary&&) = delete;

    ~LoadedLibrary() {
        /**
         * Keep sidecar libraries loaded for process lifetime.
         *
         * Core runtime structures (heap objects, primitive callables, and
         * captured closures) may retain code paths originating from sidecar
         * modules beyond one Driver instance. Explicit unload during Driver
         * teardown can invalidate those code pointers and crash finalization.
         *
         * OS loader cleanup at process exit is sufficient here.
         */
        handle = nullptr;
    }
};

SidecarLoader::SidecarLoader(ExtensionRegistry& registry)
    : registry_(registry) {}

SidecarLoader::~SidecarLoader() = default;

std::expected<void, SidecarLoaderError> SidecarLoader::load(
    const ResolvedNativeSidecar& sidecar) {
    auto library = std::make_unique<LoadedLibrary>();
    library->path = sidecar.artifact_path;

    if (sidecar.spec.expected_sha256.has_value()) {
        auto digest = compute_sidecar_sha256(sidecar.artifact_path);
        if (!digest) return std::unexpected(digest.error());

        const auto expected = lower_ascii(*sidecar.spec.expected_sha256);
        if (*digest != expected) {
            return std::unexpected(SidecarLoaderError{
                SidecarLoaderError::Code::ChecksumMismatch,
                "sidecar checksum mismatch for '" + sidecar.artifact_path.string() + "'",
            });
        }
    }

#if defined(_WIN32)
    const auto load_result = load_library_with_dependencies(sidecar.artifact_path);
    library->handle = load_result.handle;
    if (library->handle == nullptr) {
        return std::unexpected(SidecarLoaderError{
            SidecarLoaderError::Code::LibraryOpenFailed,
            "failed to load sidecar '" + sidecar.artifact_path.string() + "': "
                + win32_error_message(load_result.error_code),
        });
    }
#else
    library->handle = dlopen(sidecar.artifact_path.string().c_str(), RTLD_NOW | RTLD_LOCAL);
    if (library->handle == nullptr) {
        const char* dl_error = dlerror();
        return std::unexpected(SidecarLoaderError{
            SidecarLoaderError::Code::LibraryOpenFailed,
            "failed to load sidecar '" + sidecar.artifact_path.string() + "': "
                + (dl_error != nullptr ? std::string(dl_error) : std::string("dlopen failed")),
        });
    }
#endif

    const auto entrypoint = sidecar.spec.entrypoint.empty()
        ? std::string("eta_register_extension_v1")
        : sidecar.spec.entrypoint;

    void* symbol = nullptr;
#if defined(_WIN32)
    symbol = reinterpret_cast<void*>(GetProcAddress(library->handle, entrypoint.c_str()));
    if (symbol == nullptr) {
        return std::unexpected(SidecarLoaderError{
            SidecarLoaderError::Code::SymbolLookupFailed,
            "failed to resolve sidecar entrypoint '" + entrypoint + "' in '"
                + sidecar.artifact_path.string() + "': "
                + win32_error_message(GetLastError()),
        });
    }
#else
    dlerror();
    symbol = dlsym(library->handle, entrypoint.c_str());
    const char* dlsym_error = dlerror();
    if (symbol == nullptr || dlsym_error != nullptr) {
        return std::unexpected(SidecarLoaderError{
            SidecarLoaderError::Code::SymbolLookupFailed,
            "failed to resolve sidecar entrypoint '" + entrypoint + "' in '"
                + sidecar.artifact_path.string() + "': "
                + (dlsym_error != nullptr ? std::string(dlsym_error)
                                          : std::string("dlsym failed")),
        });
    }
#endif

    auto entry = reinterpret_cast<EtaRegisterExtensionFnV1>(symbol);
    PendingRegistration pending;
    EtaNativeApiV1 api{};
    api.struct_size = sizeof(EtaNativeApiV1);
    api.abi_id = ETA_NATIVE_ABI_ID_V1;
    api.user_data = &pending;
    api.runtime_context = runtime_context_;
    api.register_primitive = &register_primitive_bridge;
    api.report_error = &report_error_bridge;
    api.alloc_native_object = &alloc_native_object_bridge;
    api.get_native_object = &get_native_object_bridge;

    EtaExtensionInfoV1 info{};
    info.struct_size = sizeof(EtaExtensionInfoV1);

    const int status = entry(&api, &info);
    if (status != ETA_NATIVE_STATUS_OK) {
        std::string message = "sidecar registration failed for '"
            + sidecar.artifact_path.string() + "'";
        if (!pending.reported_error.empty()) {
            message += ": ";
            message += pending.reported_error;
        }
        return std::unexpected(SidecarLoaderError{
            SidecarLoaderError::Code::ExtensionRegistrationFailed,
            std::move(message),
        });
    }

    if (info.abi_id == nullptr || info.extension_id == nullptr
        || info.extension_id[0] == '\0') {
        return std::unexpected(SidecarLoaderError{
            SidecarLoaderError::Code::InvalidExtensionMetadata,
            "sidecar '" + sidecar.artifact_path.string()
                + "' returned incomplete extension metadata",
        });
    }

    const std::string reported_abi = info.abi_id;
    if (reported_abi != sidecar.spec.abi) {
        return std::unexpected(SidecarLoaderError{
            SidecarLoaderError::Code::AbiMismatch,
            "sidecar '" + sidecar.artifact_path.string() + "' reported ABI '"
                + reported_abi + "' but expected '" + sidecar.spec.abi + "'",
        });
    }

    const std::string extension_id = info.extension_id;
    if (sidecar.spec.expected_extension_id.has_value()
        && *sidecar.spec.expected_extension_id != extension_id) {
        return std::unexpected(SidecarLoaderError{
            SidecarLoaderError::Code::InvalidExtensionMetadata,
            "sidecar '" + sidecar.artifact_path.string()
                + "' reported extension id '" + extension_id + "' but expected '"
                + *sidecar.spec.expected_extension_id + "'",
        });
    }

    auto conflict_check = validate_registration_conflicts(
        registry_, extension_id, std::span<const ExtensionSymbolDescriptor>(pending.symbols));
    if (!conflict_check) return std::unexpected(conflict_check.error());

    auto register_extension = registry_.register_extension(
        extension_id,
        info.extension_version != nullptr ? std::string(info.extension_version) : std::string{},
        reported_abi);
    if (!register_extension) {
        return std::unexpected(SidecarLoaderError{
            SidecarLoaderError::Code::RegistryConflict,
            register_extension.error().message,
        });
    }
    for (auto& symbol_descriptor : pending.symbols) {
        auto register_symbol = registry_.register_symbol(extension_id, std::move(symbol_descriptor));
        if (!register_symbol) {
            return std::unexpected(SidecarLoaderError{
                SidecarLoaderError::Code::RegistryConflict,
                register_symbol.error().message,
            });
        }
    }

    loaded_libraries_.push_back(std::move(library));
    return {};
}

std::size_t SidecarLoader::loaded_library_count() const noexcept {
    return loaded_libraries_.size();
}

void SidecarLoader::set_runtime_context(void* runtime_context) noexcept {
    runtime_context_ = runtime_context;
}

} // namespace eta::native
