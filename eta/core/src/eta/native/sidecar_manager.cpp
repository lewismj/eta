/**
 * @file sidecar_manager.cpp
 * @brief Native sidecar discovery/load orchestration for eta runtimes.
 */

#include "eta/native/sidecar_manager.h"

#include <algorithm>
#include <array>
#include <expected>
#include <functional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "eta/package/discovery.h"
#include "eta/package/lockfile.h"
#include "eta/package/manifest.h"
#include "eta/runtime/error.h"
#include "eta/runtime/nanbox.h"
#include "eta/util/path.h"
#include "eta/util/runtime_layout.h"

namespace eta::native {

namespace {

constexpr std::size_t kModulePathCollectionDepthLimit = 3u;
constexpr std::string_view kModulePathSidecarManifestKey = "__eta.module-path-sidecars__";

[[nodiscard]] bool is_all_zero_sha256(std::string_view digest) {
    if (digest.size() != 64u) return false;
    for (const char ch : digest) {
        if (ch != '0') return false;
    }
    return true;
}

[[nodiscard]] std::string host_target_triple() {
#if defined(_WIN32)
#if defined(_M_X64) || defined(__x86_64__)
    return "x86_64-pc-windows-msvc";
#elif defined(_M_ARM64) || defined(__aarch64__)
    return "aarch64-pc-windows-msvc";
#else
    return "unknown-pc-windows-msvc";
#endif
#elif defined(__APPLE__)
#if defined(__aarch64__)
    return "aarch64-apple-darwin";
#else
    return "x86_64-apple-darwin";
#endif
#elif defined(__linux__)
#if defined(__aarch64__)
    return "aarch64-unknown-linux-gnu";
#else
    return "x86_64-unknown-linux-gnu";
#endif
#else
    return "unknown-unknown-unknown";
#endif
}

[[nodiscard]] const package::ManifestNativeTarget* select_native_target_for_host(
    const package::ManifestNative& native) {
    const auto triple = host_target_triple();
    const auto it = std::find_if(native.targets.begin(),
                                 native.targets.end(),
                                 [&](const package::ManifestNativeTarget& target) {
                                     return target.triple == triple;
                                 });
    if (it == native.targets.end()) return nullptr;
    return &*it;
}

[[nodiscard]] bool has_complete_native_lockfile_metadata(
    const package::LockfilePackage& package) {
    return package.native_id.has_value()
        && package.native_abi.has_value()
        && package.native_entry.has_value()
        && package.native_target_triple.has_value()
        && package.native_artifact_relpath.has_value()
        && package.native_sha256.has_value();
}

[[nodiscard]] std::string lockfile_package_id(std::string_view name, std::string_view version) {
    std::string id;
    id.reserve(name.size() + version.size() + 1u);
    id.append(name);
    id.push_back('@');
    id.append(version);
    return id;
}

[[nodiscard]] fs::path package_root_from_lockfile_source(const package::LockfilePackage& package,
                                                         const fs::path& lockfile_root,
                                                         const fs::path& modules_root) {
    if (package.source == "root") {
        return util::canonicalize_path(lockfile_root);
    }

    constexpr std::string_view kWorkspacePrefix = "workspace+";
    if (package.source.starts_with(kWorkspacePrefix)) {
        const std::string rel = package.source.substr(kWorkspacePrefix.size());
        if (rel.empty() || rel == ".") {
            return util::canonicalize_path(lockfile_root);
        }
        return util::canonicalize_path(lockfile_root / fs::path(rel));
    }

    constexpr std::string_view kPathPrefix = "path+";
    if (package.source.starts_with(kPathPrefix)) {
        fs::path source_path = fs::path(package.source.substr(kPathPrefix.size()));
        if (!source_path.is_absolute()) {
            source_path = lockfile_root / source_path;
        }
        return util::canonicalize_path(source_path);
    }

    return util::canonicalize_path(modules_root / (package.name + "-" + package.version));
}

} // namespace

void NativeSidecarManager::set_runtime_context(void* runtime_context) noexcept {
    loader_.set_runtime_context(runtime_context);
}

bool NativeSidecarManager::ensure_package_sidecars_loaded(std::optional<fs::path> start_dir,
                                                          std::span<const fs::path> module_dirs,
                                                          std::string_view etai_path) {
    if (sidecar_manifest_key_.has_value()) {
        if (!registry_.extensions().empty()) return true;
        return ensure_bundled_sidecars_loaded(module_dirs, etai_path);
    }

    const auto load_non_package_sidecars = [&]() {
        if (!ensure_module_path_sidecars_loaded(module_dirs)) return false;
        if (module_path_sidecars_loaded_ && !sidecar_manifest_key_.has_value()) {
            sidecar_manifest_key_ = std::string(kModulePathSidecarManifestKey);
        }
        return ensure_bundled_sidecars_loaded(module_dirs, etai_path);
    };

    fs::path discovery_start;
    if (start_dir.has_value()) {
        discovery_start = *start_dir;
    } else {
        std::error_code ec;
        discovery_start = fs::current_path(ec);
        if (ec) return load_non_package_sidecars();
    }
    if (discovery_start.empty()) return load_non_package_sidecars();

    auto discovery = package::discover_manifest_context(discovery_start);
    if (!discovery) {
        host_.emit_sidecar_error(
            "failed to discover package context for native sidecars: " + discovery.error().message);
        return false;
    }
    if (!discovery->context.has_value()) return load_non_package_sidecars();
    if (*discovery->context == package::ManifestContextKind::WorkspaceNonMember) {
        return load_non_package_sidecars();
    }
    if (!discovery->package_manifest_path.has_value()) {
        /**
         * Workspace virtual roots have no selected package context.
         * Keep core-only behavior until a concrete package is selected.
         */
        return load_non_package_sidecars();
    }

    const auto package_manifest_path = util::canonicalize_path(*discovery->package_manifest_path);
    const auto package_manifest_key = util::canonical_path_key(package_manifest_path);

    const auto workspace_manifest_path = discovery->workspace_manifest_path.has_value()
        ? std::optional<fs::path>(util::canonicalize_path(*discovery->workspace_manifest_path))
        : std::nullopt;
    const auto lockfile_root = workspace_manifest_path.has_value()
        ? workspace_manifest_path->parent_path()
        : package_manifest_path.parent_path();
    const auto lockfile_path = lockfile_root / "eta.lock";

    std::error_code lockfile_ec;
    if (!fs::is_regular_file(lockfile_path, lockfile_ec) || lockfile_ec) {
        sidecar_manifest_key_ = package_manifest_key;
        return ensure_bundled_sidecars_loaded(module_dirs, etai_path);
    }

    auto lockfile = package::read_lockfile(lockfile_path);
    if (!lockfile) {
        host_.emit_sidecar_error(
            "failed to read lockfile for native sidecar loading: " + lockfile.error().message);
        return false;
    }

    auto package_manifest = package::read_manifest(package_manifest_path);
    if (!package_manifest) {
        host_.emit_sidecar_error(
            "failed to read package manifest for native sidecar loading: "
            + package_manifest.error().message);
        return false;
    }

    const auto modules_root = util::canonicalize_path(lockfile_root / ".eta" / "modules");
    const auto active_package_id =
        lockfile_package_id(package_manifest->name, package_manifest->version);
    std::unordered_map<std::string, const package::LockfilePackage*> package_by_id;
    package_by_id.reserve(lockfile->packages.size());
    const package::LockfilePackage* active_lock_package = nullptr;
    for (const auto& package : lockfile->packages) {
        const auto package_id = lockfile_package_id(package.name, package.version);
        package_by_id.emplace(package_id, &package);
        if (package_id == active_package_id && active_lock_package == nullptr) {
            active_lock_package = &package;
        }
    }

    if (active_lock_package == nullptr) {
        sidecar_manifest_key_ = package_manifest_key;
        return ensure_bundled_sidecars_loaded(module_dirs, etai_path);
    }

    std::unordered_set<std::string> closure_names;
    std::unordered_set<std::string> visited_package_ids;
    std::vector<const package::LockfilePackage*> pending;
    pending.push_back(active_lock_package);
    while (!pending.empty()) {
        const auto* package = pending.back();
        pending.pop_back();
        const auto package_id = lockfile_package_id(package->name, package->version);
        if (!visited_package_ids.insert(package_id).second) continue;
        closure_names.insert(package->name);
        for (const auto& dependency : package->dependencies) {
            const auto dep_id = lockfile_package_id(dependency.name, dependency.version);
            const auto dep_it = package_by_id.find(dep_id);
            if (dep_it != package_by_id.end()) {
                pending.push_back(dep_it->second);
            }
        }
    }

    NativeLoadContext load_context;
    load_context.context_kind = *discovery->context;
    load_context.active_manifest_path = package_manifest_path;
    load_context.workspace_manifest_path = workspace_manifest_path;
    load_context.lockfile_root = util::canonicalize_path(lockfile_root);
    load_context.modules_root = modules_root;
    for (const auto& package : lockfile->packages) {
        if (!closure_names.contains(package.name)) continue;
        if (load_context.package_root_by_name.contains(package.name)) continue;
        load_context.package_root_by_name[package.name] =
            package_root_from_lockfile_source(package, lockfile_root, modules_root);
    }

    std::vector<NativeSidecarSpec> sidecar_specs;
    sidecar_specs.reserve(lockfile->packages.size());
    for (const auto& package : lockfile->packages) {
        if (!closure_names.contains(package.name)) continue;
        if (!has_complete_native_lockfile_metadata(package)) continue;

        NativeSidecarSpec spec;
        spec.package_name = package.name;
        spec.artifact_relpath = fs::path(*package.native_artifact_relpath);
        spec.abi = *package.native_abi;
        spec.entrypoint = *package.native_entry;
        spec.expected_extension_id = *package.native_id;
        spec.expected_sha256 = *package.native_sha256;
        sidecar_specs.push_back(std::move(spec));
    }

    if (sidecar_specs.empty()) {
        sidecar_manifest_key_ = package_manifest_key;
        return ensure_bundled_sidecars_loaded(module_dirs, etai_path);
    }

    auto resolved_sidecars = resolve_native_sidecars(load_context, sidecar_specs);
    if (!resolved_sidecars) {
        host_.emit_sidecar_error(
            "failed to resolve native sidecar artifact paths: " + resolved_sidecars.error().message);
        return false;
    }

    for (const auto& sidecar : *resolved_sidecars) {
        auto loaded = loader_.load(sidecar);
        if (!loaded) {
            host_.emit_sidecar_error(
                "failed to load native sidecar for package '" + sidecar.spec.package_name
                + "': " + loaded.error().message);
            return false;
        }
    }

    if (!sync_sidecar_extensions_into_environment()) return false;
    sidecar_manifest_key_ = package_manifest_key;
    return true;
}

runtime::types::PrimitiveFunc NativeSidecarManager::make_sidecar_placeholder_primitive(
    std::string extension_id,
    std::string symbol_name) {
    return [extension_id = std::move(extension_id), symbol_name = std::move(symbol_name)](
               runtime::types::PrimitiveArgs)
               -> std::expected<runtime::nanbox::LispVal, runtime::error::RuntimeError> {
        runtime::error::VMError error;
        error.code = runtime::error::RuntimeErrorCode::NotImplemented;
        error.message = "native sidecar primitive '" + symbol_name
            + "' from extension '" + extension_id + "' is not callable in this runtime build";
        return std::unexpected(runtime::error::RuntimeError{std::move(error)});
    };
}

runtime::types::PrimitiveFunc NativeSidecarManager::make_registered_sidecar_primitive(
    std::string extension_id,
    std::string symbol_name,
    void* callable) {
    if (callable == nullptr) {
        return make_sidecar_placeholder_primitive(std::move(extension_id), std::move(symbol_name));
    }

    auto sidecar_callable = *static_cast<const runtime::types::PrimitiveFunc*>(callable);
    return [extension_id = std::move(extension_id),
            symbol_name = std::move(symbol_name),
            sidecar_callable = std::move(sidecar_callable)](runtime::types::PrimitiveArgs args) mutable
               -> std::expected<runtime::nanbox::LispVal, runtime::error::RuntimeError> {
        if (!sidecar_callable) {
            runtime::error::VMError error;
            error.code = runtime::error::RuntimeErrorCode::NotImplemented;
            error.message = "native sidecar primitive '" + symbol_name
                + "' from extension '" + extension_id + "' has no callable implementation";
            return std::unexpected(runtime::error::RuntimeError{std::move(error)});
        }
        return sidecar_callable(args);
    };
}

bool NativeSidecarManager::ensure_module_path_sidecars_loaded(
    std::span<const fs::path> module_dirs) {
    if (module_path_sidecars_attempted_) return true;
    module_path_sidecars_attempted_ = true;

    std::unordered_set<std::string> root_seen;
    std::vector<fs::path> package_roots;

    const auto add_package_root = [&](const fs::path& raw_root) {
        if (raw_root.empty()) return;
        std::error_code ec;
        if (!fs::is_directory(raw_root, ec) || ec) return;
        const auto canonical_root = util::canonicalize_path(raw_root);
        const auto root_key = util::canonical_path_key(canonical_root);
        if (!root_seen.insert(root_key).second) return;

        const auto manifest_path = canonical_root / "eta.toml";
        ec.clear();
        if (!fs::is_regular_file(manifest_path, ec) || ec) return;
        package_roots.push_back(canonical_root);
    };

    std::function<void(const fs::path&, std::size_t)> scan_collection_root;
    scan_collection_root = [&](const fs::path& raw_dir, const std::size_t depth) {
        std::error_code ec;
        if (!fs::is_directory(raw_dir, ec) || ec) return;

        const auto canonical_dir = util::canonicalize_path(raw_dir);
        const auto manifest_path = canonical_dir / "eta.toml";
        ec.clear();
        if (fs::is_regular_file(manifest_path, ec) && !ec) {
            add_package_root(canonical_dir);
            return;
        }

        if (depth >= kModulePathCollectionDepthLimit) return;

        std::vector<fs::path> children;
        for (const auto& child : fs::directory_iterator(
                 canonical_dir,
                 fs::directory_options::skip_permission_denied,
                 ec)) {
            if (ec) break;
            std::error_code child_ec;
            if (child.is_directory(child_ec) && !child_ec) {
                children.push_back(child.path());
            }
        }
        if (ec) return;

        std::sort(children.begin(),
                  children.end(),
                  [](const fs::path& lhs, const fs::path& rhs) {
                      return util::canonical_path_key(lhs)
                          < util::canonical_path_key(rhs);
                  });

        for (const auto& child : children) {
            scan_collection_root(child, depth + 1u);
        }
    };

    for (const auto& module_dir : module_dirs) {
        if (module_dir.empty()) continue;
        std::error_code ec;
        if (!fs::is_directory(module_dir, ec) || ec) continue;
        const auto canonical_dir = util::canonicalize_path(module_dir);

        fs::path ancestor = canonical_dir;
        for (std::size_t i = 0; i < 3u; ++i) {
            add_package_root(ancestor);
            const auto parent = ancestor.parent_path();
            if (parent.empty() || parent == ancestor) break;
            ancestor = parent;
        }

        scan_collection_root(canonical_dir, 0u);
    }

    if (package_roots.empty()) return true;

    std::unordered_set<std::string> package_seen;
    std::unordered_map<std::string, fs::path> package_root_by_name;
    std::vector<NativeSidecarSpec> sidecar_specs;
    sidecar_specs.reserve(package_roots.size());

    for (const auto& package_root : package_roots) {
        auto manifest = package::read_manifest(package_root / "eta.toml");
        if (!manifest) continue;
        if (!manifest->native.has_value()) continue;
        if (manifest->native->kind != "sidecar") continue;
        if (registry_.find_extension(manifest->native->id) != nullptr) continue;
        if (!package_seen.insert(manifest->name).second) continue;

        const auto* selected_target = select_native_target_for_host(*manifest->native);
        if (selected_target == nullptr) continue;

        std::error_code ec;
        const auto artifact_abs =
            util::canonicalize_path(package_root / selected_target->artifact);
        if (!is_path_within(package_root, artifact_abs)) continue;
        if (!fs::is_regular_file(artifact_abs, ec) || ec) continue;

        package_root_by_name.emplace(manifest->name, package_root);

        NativeSidecarSpec spec;
        spec.package_name = manifest->name;
        spec.artifact_relpath = selected_target->artifact;
        spec.abi = manifest->native->abi;
        spec.entrypoint = manifest->native->entry;
        spec.expected_extension_id = manifest->native->id;
        if (!selected_target->sha256.empty() && !is_all_zero_sha256(selected_target->sha256)) {
            spec.expected_sha256 = selected_target->sha256;
        }
        sidecar_specs.push_back(std::move(spec));
    }

    if (sidecar_specs.empty()) return true;

    NativeLoadContext context;
    context.context_kind = package::ManifestContextKind::StandalonePackage;
    std::error_code cwd_ec;
    context.active_manifest_path = util::canonicalize_path(fs::current_path(cwd_ec));
    if (cwd_ec) context.active_manifest_path = util::canonicalize_path(fs::path("."));
    context.lockfile_root = context.active_manifest_path.parent_path();
    context.modules_root = context.lockfile_root / ".eta" / "modules";
    context.package_root_by_name = std::move(package_root_by_name);

    auto resolved_sidecars = resolve_native_sidecars(context, sidecar_specs);
    if (!resolved_sidecars) {
        host_.emit_sidecar_error(
            "failed to resolve module-path native sidecar artifact paths: "
            + resolved_sidecars.error().message);
        return false;
    }

    for (const auto& sidecar : *resolved_sidecars) {
        auto loaded = loader_.load(sidecar);
        if (!loaded) {
            host_.emit_sidecar_error(
                "failed to load module-path native sidecar for package '"
                + sidecar.spec.package_name + "': " + loaded.error().message);
            return false;
        }
    }

    if (!sync_sidecar_extensions_into_environment()) return false;
    module_path_sidecars_loaded_ = true;
    return true;
}

bool NativeSidecarManager::ensure_bundled_sidecars_loaded(std::span<const fs::path> module_dirs,
                                                          std::string_view etai_path) {
    if (bundled_sidecars_attempted_) return true;
    bundled_sidecars_attempted_ = true;
    const auto& layout = util::kDefaultRuntimeLayoutConfig;

    std::unordered_set<std::string> root_seen;
    std::vector<fs::path> candidate_roots;
    const auto add_candidate_root = [&](const fs::path& root) {
        if (root.empty()) return;
        std::error_code ec;
        if (!fs::is_directory(root, ec) || ec) return;
        const auto canonical = util::canonicalize_path(root);
        const auto key = util::canonical_path_key(canonical);
        if (!root_seen.insert(key).second) return;
        candidate_roots.push_back(canonical);
    };

    for (const auto& module_dir : module_dirs) {
        add_candidate_root(util::bundled_stdlib_native_root(module_dir, layout));
        add_candidate_root(util::bundled_stdlib_native_root(module_dir.parent_path(), layout));
    }

    if (!etai_path.empty()) {
        const fs::path etai_abs_path = util::canonicalize_path(fs::path(etai_path));
        add_candidate_root(util::bundled_stdlib_native_root(etai_abs_path.parent_path(), layout));
        add_candidate_root(util::bundled_stdlib_native_root(
            etai_abs_path.parent_path().parent_path(), layout));
    }

    if (candidate_roots.empty()) return true;

    std::unordered_set<std::string> package_seen;
    std::unordered_map<std::string, fs::path> package_root_by_name;
    std::vector<NativeSidecarSpec> sidecar_specs;

    const std::array<std::string_view, 4> builtin_sidecar_dirs{
        "log",
        "stats",
        "torch",
        "nng",
    };

    for (const auto& native_root : candidate_roots) {
        for (const auto dir_name : builtin_sidecar_dirs) {
            const auto package_root = native_root / std::string(dir_name);
            const auto manifest_path = package_root / "eta.toml";
            std::error_code ec;
            if (!fs::is_regular_file(manifest_path, ec) || ec) continue;

            auto manifest = package::read_manifest(manifest_path);
            if (!manifest) continue;
            if (!manifest->native.has_value()) continue;

            const auto* selected_target = select_native_target_for_host(*manifest->native);
            if (selected_target == nullptr) continue;

            const auto artifact_abs = util::canonicalize_path(package_root / selected_target->artifact);
            if (!is_path_within(package_root, artifact_abs)) continue;
            if (!fs::is_regular_file(artifact_abs, ec) || ec) continue;

            if (!package_seen.insert(manifest->name).second) continue;
            package_root_by_name.emplace(manifest->name, util::canonicalize_path(package_root));

            NativeSidecarSpec spec;
            spec.package_name = manifest->name;
            spec.artifact_relpath = selected_target->artifact;
            spec.abi = manifest->native->abi;
            spec.entrypoint = manifest->native->entry;
            spec.expected_extension_id = manifest->native->id;
            if (!selected_target->sha256.empty() && !is_all_zero_sha256(selected_target->sha256)) {
                spec.expected_sha256 = selected_target->sha256;
            }
            sidecar_specs.push_back(std::move(spec));
        }
    }

    if (sidecar_specs.empty()) return true;

    NativeLoadContext context;
    context.context_kind = package::ManifestContextKind::StandalonePackage;
    std::error_code cwd_ec;
    context.active_manifest_path = util::canonicalize_path(fs::current_path(cwd_ec));
    if (cwd_ec) context.active_manifest_path = util::canonicalize_path(fs::path("."));
    context.lockfile_root = context.active_manifest_path.parent_path();
    context.modules_root = context.lockfile_root / ".eta" / "modules";
    context.package_root_by_name = std::move(package_root_by_name);

    auto resolved_sidecars = resolve_native_sidecars(context, sidecar_specs);
    if (!resolved_sidecars) {
        host_.emit_sidecar_error(
            "failed to resolve bundled native sidecar artifact paths: "
            + resolved_sidecars.error().message);
        return false;
    }

    for (const auto& sidecar : *resolved_sidecars) {
        auto loaded = loader_.load(sidecar);
        if (!loaded) {
            host_.emit_sidecar_error(
                "failed to load bundled native sidecar for package '" + sidecar.spec.package_name
                + "': " + loaded.error().message);
            return false;
        }
    }

    return sync_sidecar_extensions_into_environment();
}

bool NativeSidecarManager::sync_sidecar_extensions_into_environment() {
    const auto& loaded_extensions = registry_.extensions();
    if (sidecar_registered_extension_count_ >= loaded_extensions.size()) {
        return true;
    }

    if (!host_.can_register_extension_primitives()) {
        host_.emit_sidecar_error(
            "native sidecar registration was requested after the session started executing modules; "
            "sidecars must be discovered from startup module paths");
        return false;
    }

    for (std::size_t i = sidecar_registered_extension_count_; i < loaded_extensions.size(); ++i) {
        const auto& extension = loaded_extensions[i];
        std::vector<ExtensionSymbolDescriptor> symbols = extension.symbols;
        std::sort(symbols.begin(),
                  symbols.end(),
                  [](const ExtensionSymbolDescriptor& lhs, const ExtensionSymbolDescriptor& rhs) {
                      return lhs.name < rhs.name;
                  });

        for (const auto& symbol : symbols) {
            auto primitive = make_registered_sidecar_primitive(
                extension.id, symbol.name, symbol.callable);
            if (host_.has_builtin_primitive(symbol.name)) {
                host_.overwrite_builtin_primitive(symbol.name, std::move(primitive));
            } else {
                host_.register_extension_primitive(
                    symbol.name,
                    symbol.arity,
                    symbol.has_rest,
                    std::move(primitive));
            }
        }
    }

    sidecar_registered_extension_count_ = loaded_extensions.size();
    host_.invalidate_primitive_installer();
    return true;
}

} // namespace eta::native
