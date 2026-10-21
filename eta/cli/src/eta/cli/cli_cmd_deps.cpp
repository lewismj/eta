/// Subcommands: vendor, install, add, remove, update, clean.

#include <algorithm>
#include <iostream>
#include <system_error>

#include "eta/cli/cli_internal.h"

namespace eta::cli {

namespace {

CliResult<VendorOptions> parse_vendor_options(const std::vector<std::string>& args) {
    VendorOptions options;
    for (std::size_t i = 0; i < args.size(); ++i) {
        const auto& arg = args[i];
        if (arg == "--target") {
            if (i + 1u >= args.size()) {
                return std::unexpected("eta vendor: --target requires a value");
            }
            options.target = fs::path(args[++i]);
            continue;
        }
        return std::unexpected("eta vendor: unknown option " + arg);
    }
    return options;
}

CliResult<InstallOptions> parse_install_options(const std::vector<std::string>& args) {
    InstallOptions options;
    for (const auto& arg : args) {
        if (arg == "--global") {
            options.global = true;
            continue;
        }
        if (!arg.empty() && arg.front() == '-') {
            return std::unexpected("eta install: unknown option " + arg);
        }
        if (options.package.has_value()) {
            return std::unexpected("eta install: accepts at most one package name");
        }
        options.package = arg;
    }
    return options;
}

CliResult<AddOptions> parse_add_options(const std::vector<std::string>& args) {
    AddOptions options;
    bool name_seen = false;
    for (std::size_t i = 0; i < args.size(); ++i) {
        const auto& arg = args[i];
        if (arg == "--dev") {
            options.dev = true;
            continue;
        }
        if (arg == "--path") {
            if (i + 1u >= args.size()) return std::unexpected("eta add: --path requires a value");
            options.path = fs::path(args[++i]);
            continue;
        }
        if (arg == "--git") {
            if (i + 1u >= args.size()) return std::unexpected("eta add: --git requires a value");
            options.git = args[++i];
            continue;
        }
        if (arg == "--rev") {
            if (i + 1u >= args.size()) return std::unexpected("eta add: --rev requires a value");
            options.rev = args[++i];
            continue;
        }
        if (arg == "--tarball") {
            if (i + 1u >= args.size()) return std::unexpected("eta add: --tarball requires a value");
            options.tarball = args[++i];
            continue;
        }
        if (arg == "--sha256") {
            if (i + 1u >= args.size()) return std::unexpected("eta add: --sha256 requires a value");
            options.sha256 = args[++i];
            continue;
        }
        if (!arg.empty() && arg.front() == '-') {
            return std::unexpected("eta add: unknown option " + arg);
        }
        if (name_seen) {
            return std::unexpected("eta add: accepts exactly one package argument");
        }
        name_seen = true;
        const auto at = arg.find('@');
        options.name = (at == std::string::npos) ? arg : arg.substr(0u, at);
    }

    if (options.name.empty()) return std::unexpected("eta add: missing package name");
    if (!is_valid_package_name(options.name)) {
        return std::unexpected("eta add: package name must match [a-z][a-z0-9_-]{0,63}");
    }

    const int kinds = (options.path.has_value() ? 1 : 0)
        + (options.git.has_value() ? 1 : 0)
        + (options.tarball.has_value() ? 1 : 0);
    if (kinds != 1) {
        return std::unexpected("eta add: choose exactly one source kind: --path, --git, or --tarball");
    }
    if (options.git.has_value()) {
        if (!options.rev.has_value()) {
            return std::unexpected("eta add: git dependencies require --rev <40-hex>");
        }
        if (!eta::package::is_safe_git_url(*options.git)) {
            return std::unexpected("eta add: --git must be https://, ssh:// or git@host:path");
        }
        if (!looks_like_hex(*options.rev, 40u)) {
            return std::unexpected("eta add: --rev must be a full 40-hex commit id");
        }
    }
    if (options.tarball.has_value()) {
        if (!options.sha256.has_value()) {
            return std::unexpected("eta add: tarball dependencies require --sha256 <64-hex>");
        }
        if (!looks_like_hex(*options.sha256, 64u)) {
            return std::unexpected("eta add: --sha256 must be 64 hex characters");
        }
    }
    return options;
}

void upsert_dependency(std::vector<eta::package::ManifestDependency>& deps,
                       eta::package::ManifestDependency dep) {
    auto it = std::find_if(deps.begin(),
                           deps.end(),
                           [&](const eta::package::ManifestDependency& existing) {
                               return existing.name == dep.name;
                           });
    if (it != deps.end()) {
        *it = std::move(dep);
    } else {
        deps.push_back(std::move(dep));
    }
    std::sort(deps.begin(),
              deps.end(),
              [](const eta::package::ManifestDependency& lhs,
                 const eta::package::ManifestDependency& rhs) {
                  return lhs.name < rhs.name;
              });
}

CliResult<CleanOptions> parse_clean_options(const std::vector<std::string>& args) {
    CleanOptions options;
    for (const auto& arg : args) {
        if (arg == "--all") {
            options.all = true;
            continue;
        }
        return std::unexpected("eta clean: unknown option " + arg);
    }
    return options;
}

} ///< namespace

CliResult<int> command_vendor(const char* program,
                              const std::vector<std::string>& args,
                              const fs::path& cwd) {
    auto parsed_args = parse_workspace_command_args("vendor", args);
    if (!parsed_args) return std::unexpected(parsed_args.error());

    for (const auto& arg : parsed_args->command_args) {
        if (arg == "--help" || arg == "-h") {
            print_vendor_usage(program);
            return 0;
        }
    }

    auto options = parse_vendor_options(parsed_args->command_args);
    if (!options) return std::unexpected(options.error());

    auto targets = resolve_command_targets(
        "vendor",
        cwd,
        parsed_args->workspace,
        SelectionCommandMode::Aggregate,
        "eta vendor: no eta.toml found");
    if (!targets) return std::unexpected(targets.error());

    auto state = resolve_project_state(targets->selected.front().manifest_path, false, true, true);
    if (!state) return std::unexpected("eta vendor: " + state.error());

    fs::path target = state->modules_root;
    if (options->target.has_value()) {
        const fs::path target_base = targets->workspace_manifest_path.has_value()
            ? targets->workspace_root
            : state->project_root;
        target = options->target->is_absolute()
            ? *options->target
            : canonicalize_path(target_base / *options->target);
    }

    if (auto materialize = materialize_modules_from_lockfile(state->lockfile_root, state->lockfile, target);
        !materialize) {
        return std::unexpected("eta vendor: " + materialize.error());
    }

    std::cout << "materialized dependencies to " << target.string() << "\n";
    return 0;
}

CliResult<int> command_install(const char* program,
                               const char* argv0,
                               const std::vector<std::string>& args,
                               const fs::path& cwd) {
    auto parsed_args = parse_workspace_command_args("install", args);
    if (!parsed_args) return std::unexpected(parsed_args.error());

    for (const auto& arg : parsed_args->command_args) {
        if (arg == "--help" || arg == "-h") {
            print_install_usage(program);
            return 0;
        }
    }

    auto options = parse_install_options(parsed_args->command_args);
    if (!options) return std::unexpected(options.error());

    auto targets = resolve_command_targets(
        "install",
        cwd,
        parsed_args->workspace,
        SelectionCommandMode::SingleTarget,
        "eta install: no eta.toml found");
    if (!targets) return std::unexpected(targets.error());

    auto state = resolve_project_state(targets->selected.front().manifest_path, false, true, true);
    if (!state) return std::unexpected("eta install: " + state.error());

    BuildOptions build_opts;
    build_opts.profile = "release";
    auto build_rc = compile_project_sources(argv0, *state, build_opts);
    if (!build_rc) return std::unexpected(build_rc.error());
    if (*build_rc != 0) return build_rc;

    fs::path install_root;
    if (options->global) {
        auto home = home_dir();
        if (!home.has_value()) {
            return std::unexpected("eta install: failed to resolve user home directory");
        }
        install_root = *home / ".eta" / "bin";
    } else {
        install_root = state->project_root / ".eta" / "bin";
    }
    if (auto mk = ensure_directory(install_root); !mk) return std::unexpected(mk.error());

    const bool install_root_package = !options->package.has_value()
        || *options->package == state->manifest.name;
    if (install_root_package) {
        const std::string root_module = module_name_from_package_name(state->manifest.name);
        const fs::path source =
            selected_package_target_profile_root(*state, "release") / (root_module + ".etac");
        if (!fs::is_regular_file(source)) {
            return std::unexpected("eta install: built artifact missing: " + source.string());
        }
        const fs::path destination = install_root / (root_module + ".etac");
        std::error_code ec;
        fs::copy_file(source, destination, fs::copy_options::overwrite_existing, ec);
        if (ec) {
            return std::unexpected("eta install: failed to copy artifact: " + ec.message());
        }
        std::cout << "installed " << destination.string() << "\n";
        return 0;
    }

    const auto it = std::find_if(state->lockfile.packages.begin(),
                                 state->lockfile.packages.end(),
                                 [&](const eta::package::LockfilePackage& pkg) {
                                     return pkg.name == *options->package;
                                 });
    if (it == state->lockfile.packages.end()) {
        return std::unexpected("eta install: package not found in lockfile: " + *options->package);
    }
    const fs::path dep_release = lockfile_package_release_root(*state, *it);
    std::error_code ec;
    if (!fs::is_directory(dep_release, ec) || ec) {
        return std::unexpected("eta install: dependency has no release artifacts: " + dep_release.string());
    }
    const fs::path dep_install_root = install_root / it->name;
    if (auto copy = copy_directory_tree(dep_release, dep_install_root); !copy) {
        return std::unexpected(copy.error());
    }
    std::cout << "installed dependency artifacts to " << dep_install_root.string() << "\n";
    return 0;
}

CliResult<int> command_add(const char* program,
                           const std::vector<std::string>& args,
                           const fs::path& cwd) {
    auto parsed_args = parse_workspace_command_args("add", args);
    if (!parsed_args) return std::unexpected(parsed_args.error());

    for (const auto& arg : parsed_args->command_args) {
        if (arg == "--help" || arg == "-h") {
            print_add_usage(program);
            return 0;
        }
    }

    auto options = parse_add_options(parsed_args->command_args);
    if (!options) return std::unexpected(options.error());

    auto targets = resolve_command_targets(
        "add",
        cwd,
        parsed_args->workspace,
        SelectionCommandMode::SingleTarget,
        "eta add: no eta.toml found");
    if (!targets) return std::unexpected(targets.error());
    const auto& selected_target = targets->selected.front();

    auto manifest = eta::package::read_manifest(selected_target.manifest_path);
    if (!manifest) return std::unexpected(manifest.error().message);

    eta::package::ManifestDependency dep;
    dep.name = options->name;
    if (options->path.has_value()) {
        dep.kind = eta::package::ManifestDependencyKind::Path;
        dep.path = *options->path;
    } else if (options->git.has_value()) {
        dep.kind = eta::package::ManifestDependencyKind::Git;
        dep.git = *options->git;
        dep.rev = *options->rev;
    } else {
        dep.kind = eta::package::ManifestDependencyKind::Tarball;
        dep.tarball = *options->tarball;
        dep.sha256 = lower_ascii(*options->sha256);
    }

    if (options->dev) {
        manifest->dependencies.erase(
            std::remove_if(manifest->dependencies.begin(),
                           manifest->dependencies.end(),
                           [&](const eta::package::ManifestDependency& existing) {
                               return existing.name == dep.name;
                           }),
            manifest->dependencies.end());
        upsert_dependency(manifest->dev_dependencies, std::move(dep));
    } else {
        manifest->dev_dependencies.erase(
            std::remove_if(manifest->dev_dependencies.begin(),
                           manifest->dev_dependencies.end(),
                           [&](const eta::package::ManifestDependency& existing) {
                               return existing.name == dep.name;
                           }),
            manifest->dev_dependencies.end());
        upsert_dependency(manifest->dependencies, std::move(dep));
    }

    auto write_res = eta::package::write_manifest_file(*manifest, selected_target.manifest_path);
    if (!write_res) return std::unexpected(write_res.error().message);

    auto state = resolve_project_state(selected_target.manifest_path, false, true, true);
    if (!state) return std::unexpected("eta add: " + state.error());

    std::cout << "added dependency " << options->name << "\n";
    return 0;
}

CliResult<int> command_remove(const char* program,
                              const std::vector<std::string>& args,
                              const fs::path& cwd) {
    auto parsed_args = parse_workspace_command_args("remove", args);
    if (!parsed_args) return std::unexpected(parsed_args.error());

    for (const auto& arg : parsed_args->command_args) {
        if (arg == "--help" || arg == "-h") {
            print_remove_usage(program);
            return 0;
        }
    }

    if (parsed_args->command_args.size() != 1u
        || parsed_args->command_args.front().empty()
        || parsed_args->command_args.front().front() == '-') {
        return std::unexpected("eta remove: requires exactly one package name");
    }
    const std::string package_name = parsed_args->command_args.front();

    auto targets = resolve_command_targets(
        "remove",
        cwd,
        parsed_args->workspace,
        SelectionCommandMode::SingleTarget,
        "eta remove: no eta.toml found");
    if (!targets) return std::unexpected(targets.error());
    const auto& selected_target = targets->selected.front();

    auto manifest = eta::package::read_manifest(selected_target.manifest_path);
    if (!manifest) return std::unexpected(manifest.error().message);

    auto erase_by_name = [&](std::vector<eta::package::ManifestDependency>& deps) {
        const auto before = deps.size();
        deps.erase(std::remove_if(deps.begin(),
                                  deps.end(),
                                  [&](const eta::package::ManifestDependency& dep) {
                                      return dep.name == package_name;
                                  }),
                   deps.end());
        return before != deps.size();
    };

    const bool removed = erase_by_name(manifest->dependencies) || erase_by_name(manifest->dev_dependencies);
    if (!removed) {
        return std::unexpected("eta remove: dependency not found: " + package_name);
    }

    auto write_res = eta::package::write_manifest_file(*manifest, selected_target.manifest_path);
    if (!write_res) return std::unexpected(write_res.error().message);

    auto state = resolve_project_state(selected_target.manifest_path, false, true, true);
    if (!state) return std::unexpected("eta remove: " + state.error());

    std::cout << "removed dependency " << package_name << "\n";
    return 0;
}

CliResult<int> command_update(const char* program,
                              const std::vector<std::string>& args,
                              const fs::path& cwd) {
    auto parsed_args = parse_workspace_command_args("update", args);
    if (!parsed_args) return std::unexpected(parsed_args.error());

    for (const auto& arg : parsed_args->command_args) {
        if (arg == "--help" || arg == "-h") {
            print_update_usage(program);
            return 0;
        }
        if (!arg.empty() && arg.front() == '-') {
            return std::unexpected("eta update: unknown option " + arg);
        }
    }

    auto targets = resolve_command_targets(
        "update",
        cwd,
        parsed_args->workspace,
        SelectionCommandMode::Aggregate,
        "eta update: no eta.toml found");
    if (!targets) return std::unexpected(targets.error());

    auto state = resolve_project_state(targets->selected.front().manifest_path, false, true, true);
    if (!state) return std::unexpected("eta update: " + state.error());

    if (!parsed_args->command_args.empty()) {
        std::cout << "updated lockfile (full graph refresh; package-scoped update is not narrowed yet)\n";
    } else {
        std::cout << "updated lockfile\n";
    }
    return 0;
}

CliResult<int> command_clean(const char* program,
                             const std::vector<std::string>& args,
                             const fs::path& cwd) {
    auto parsed_args = parse_workspace_command_args("clean", args);
    if (!parsed_args) return std::unexpected(parsed_args.error());

    for (const auto& arg : parsed_args->command_args) {
        if (arg == "--help" || arg == "-h") {
            print_clean_usage(program);
            return 0;
        }
    }

    auto options = parse_clean_options(parsed_args->command_args);
    if (!options) return std::unexpected(options.error());

    auto targets = resolve_command_targets(
        "clean",
        cwd,
        parsed_args->workspace,
        SelectionCommandMode::Aggregate,
        "eta clean: no eta.toml found");
    if (!targets) return std::unexpected(targets.error());

    if (targets->workspace_manifest_path.has_value()) {
        const fs::path workspace_target_root = targets->workspace_root / ".eta" / "target";
        std::error_code ec;
        if (fs::is_directory(workspace_target_root, ec) && !ec) {
            for (const auto& profile_dir : fs::directory_iterator(workspace_target_root, ec)) {
                if (ec) {
                    return std::unexpected("eta clean: failed to iterate path '"
                                           + workspace_target_root.string() + "': " + ec.message());
                }
                std::error_code entry_ec;
                if (!profile_dir.is_directory(entry_ec) || entry_ec) continue;
                for (const auto& target : targets->selected) {
                    if (auto rm = remove_path_if_exists(profile_dir.path() / target.name); !rm) {
                        return std::unexpected("eta clean: " + rm.error());
                    }
                }
            }
        } else if (ec) {
            return std::unexpected("eta clean: failed to stat path '"
                                   + workspace_target_root.string() + "': " + ec.message());
        }
    } else {
        for (const auto& target : targets->selected) {
            if (auto rm = remove_path_if_exists(target.package_root / ".eta" / "target"); !rm) {
                return std::unexpected("eta clean: " + rm.error());
            }
        }
    }

    if (options->all) {
        if (targets->workspace_manifest_path.has_value()) {
            if (auto rm = remove_path_if_exists(targets->workspace_root / ".eta" / "modules"); !rm) {
                return std::unexpected("eta clean: " + rm.error());
            }
        } else {
            for (const auto& target : targets->selected) {
                if (auto rm = remove_path_if_exists(target.package_root / ".eta" / "modules"); !rm) {
                    return std::unexpected("eta clean: " + rm.error());
                }
            }
        }
    }
    std::cout << "cleaned project artifacts\n";
    return 0;
}

} ///< namespace eta::cli
