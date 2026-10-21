/// Manifest discovery, workspace selection flags and target resolution.

#include <algorithm>
#include <system_error>
#include <unordered_set>

#include "eta/cli/cli_internal.h"

namespace eta::cli {

namespace {

CliResult<fs::path> find_manifest_path(const fs::path& start_dir, std::string_view missing_message) {
    auto discovered = discover_manifest_context(start_dir);
    if (!discovered) return std::unexpected(discovered.error());
    if (!discovered->active_manifest_path.has_value()) {
        return std::unexpected(std::string(missing_message));
    }
    return *discovered->active_manifest_path;
}

[[nodiscard]] bool paths_equal(const fs::path& lhs, const fs::path& rhs) {
    return path_key(lhs) == path_key(rhs);
}

[[nodiscard]] std::string normalize_member_selector(std::string_view selector) {
    auto normalized = fs::path(std::string(selector)).lexically_normal().generic_string();
    if (normalized.empty() || normalized == "./") return ".";
#ifdef _WIN32
    return lower_ascii(normalized);
#else
    return normalized;
#endif
}

[[nodiscard]] std::string workspace_member_selector(const eta::package::WorkspaceMember& member) {
    std::string selector = ".";
    if (member.source.rfind("workspace+", 0) == 0) {
        selector = member.source.substr(10u);
        if (selector.empty()) selector = ".";
    }
    return normalize_member_selector(selector);
}

CliResult<fs::path> resolve_manifest_path_argument(std::string_view command,
                                                   const fs::path& cwd,
                                                   const fs::path& raw_path) {
    fs::path candidate = raw_path;
    if (!candidate.is_absolute()) {
        candidate = cwd / candidate;
    }
    candidate = canonicalize_path(candidate);

    std::error_code ec;
    if (fs::is_directory(candidate, ec) && !ec) {
        candidate /= "eta.toml";
    }
    if (!fs::is_regular_file(candidate, ec) || ec) {
        return std::unexpected("eta " + std::string(command)
                               + ": --manifest-path does not point to an eta.toml file: "
                               + candidate.string());
    }
    return canonicalize_path(candidate);
}

} ///< namespace

CliResult<eta::package::ManifestDiscovery> discover_manifest_context(const fs::path& start_dir) {
    auto discovered = eta::package::discover_manifest_context(start_dir);
    if (!discovered) return std::unexpected(discovered.error().message);
    return *discovered;
}

CliResult<ParsedCommandArgs> parse_workspace_command_args(std::string_view command,
                                                          const std::vector<std::string>& args) {
    ParsedCommandArgs parsed;
    bool passthrough = false;

    for (std::size_t i = 0; i < args.size(); ++i) {
        const auto& arg = args[i];
        if (passthrough) {
            parsed.command_args.push_back(arg);
            continue;
        }
        if (arg == "--") {
            passthrough = true;
            parsed.command_args.push_back(arg);
            continue;
        }
        if (arg == "--workspace") {
            parsed.workspace.workspace = true;
            continue;
        }
        if (arg == "-p" || arg == "--package") {
            if (i + 1u >= args.size()) {
                return std::unexpected("eta " + std::string(command)
                                       + ": " + arg + " requires a package name");
            }
            parsed.workspace.packages.push_back(args[++i]);
            continue;
        }
        if (arg == "--exclude") {
            if (i + 1u >= args.size()) {
                return std::unexpected("eta " + std::string(command)
                                       + ": --exclude requires a package name");
            }
            parsed.workspace.exclude.push_back(args[++i]);
            continue;
        }
        if (arg == "--manifest-path") {
            if (i + 1u >= args.size()) {
                return std::unexpected("eta " + std::string(command)
                                       + ": --manifest-path requires a path");
            }
            if (parsed.workspace.manifest_path.has_value()) {
                return std::unexpected("eta " + std::string(command)
                                       + ": --manifest-path may be provided at most once");
            }
            parsed.workspace.manifest_path = fs::path(args[++i]);
            continue;
        }
        parsed.command_args.push_back(arg);
    }
    return parsed;
}

CliResult<ResolvedCommandTargets> resolve_command_targets(std::string_view command,
                                                          const fs::path& cwd,
                                                          const WorkspaceCliOptions& options,
                                                          SelectionCommandMode mode,
                                                          std::string_view missing_manifest_message) {
    std::string command_name(command);

    eta::package::ManifestDiscovery discovery;
    if (options.manifest_path.has_value()) {
        auto manifest_path = resolve_manifest_path_argument(command, cwd, *options.manifest_path);
        if (!manifest_path) return std::unexpected(manifest_path.error());

        auto document = eta::package::read_manifest_document(*manifest_path);
        if (!document) {
            return std::unexpected("eta " + command_name + ": " + document.error().message);
        }

        auto discovered = discover_manifest_context(manifest_path->parent_path());
        if (!discovered) return std::unexpected("eta " + command_name + ": " + discovered.error());
        discovery = std::move(*discovered);
        discovery.start_dir = manifest_path->parent_path();
        discovery.active_manifest_path = *manifest_path;

        if (document->package.has_value()) {
            discovery.package_manifest_path = *manifest_path;
        } else if (discovery.package_manifest_path.has_value()
                   && paths_equal(*discovery.package_manifest_path, *manifest_path)) {
            discovery.package_manifest_path.reset();
        }

        if (document->workspace.has_value()) {
            discovery.workspace_manifest_path = *manifest_path;
        } else if (discovery.workspace_manifest_path.has_value()
                   && paths_equal(*discovery.workspace_manifest_path, *manifest_path)) {
            discovery.workspace_manifest_path.reset();
        }
    } else {
        auto discovered = discover_manifest_context(cwd);
        if (!discovered) return std::unexpected("eta " + command_name + ": " + discovered.error());
        discovery = std::move(*discovered);
    }

    if (!discovery.package_manifest_path.has_value()
        && !discovery.workspace_manifest_path.has_value()
        && !discovery.active_manifest_path.has_value()) {
        return std::unexpected(std::string(missing_manifest_message));
    }

    if (!discovery.workspace_manifest_path.has_value()) {
        if (options.workspace || !options.packages.empty() || !options.exclude.empty()) {
            return std::unexpected("eta " + command_name
                                   + ": workspace selection flags require a workspace manifest");
        }
        const auto manifest_path = discovery.package_manifest_path.has_value()
            ? *discovery.package_manifest_path
            : *discovery.active_manifest_path;
        auto manifest = eta::package::read_manifest(manifest_path);
        if (!manifest) {
            return std::unexpected("eta " + command_name + ": " + manifest.error().message);
        }

        ResolvedCommandTargets targets;
        targets.context = InvocationContext::StandalonePackage;
        targets.selected.push_back(SelectedPackageTarget{
            manifest->name,
            canonicalize_path(manifest_path),
            canonicalize_path(manifest_path.parent_path()),
        });
        return targets;
    }

    auto workspace_members =
        eta::package::resolve_workspace_members(*discovery.workspace_manifest_path);
    if (!workspace_members) {
        return std::unexpected("eta " + command_name + ": " + workspace_members.error().message);
    }

    auto workspace_doc =
        eta::package::read_manifest_document(*discovery.workspace_manifest_path);
    if (!workspace_doc) {
        return std::unexpected("eta " + command_name + ": " + workspace_doc.error().message);
    }
    if (!workspace_doc->workspace.has_value()) {
        return std::unexpected("eta " + command_name
                               + ": manifest does not declare [workspace]: "
                               + discovery.workspace_manifest_path->string());
    }

    const auto& workspace = *workspace_members;
    const auto& workspace_section = *workspace_doc->workspace;

    std::unordered_map<std::string, const eta::package::WorkspaceMember*> by_name;
    by_name.reserve(workspace.members.size());
    const eta::package::WorkspaceMember* root_member = nullptr;
    for (const auto& member : workspace.members) {
        by_name.emplace(member.name, &member);
        if (paths_equal(member.package_root, workspace.workspace_root)) {
            root_member = &member;
        }
    }

    const eta::package::WorkspaceMember* current_member = nullptr;
    if (discovery.package_manifest_path.has_value()) {
        for (const auto& member : workspace.members) {
            if (paths_equal(member.manifest_path, *discovery.package_manifest_path)) {
                current_member = &member;
                break;
            }
        }
    }

    InvocationContext invocation = InvocationContext::WorkspaceNonMember;
    if (paths_equal(discovery.start_dir, workspace.workspace_root)) {
        invocation = InvocationContext::WorkspaceRoot;
    } else if (current_member != nullptr) {
        invocation = InvocationContext::WorkspaceMember;
    }

    auto resolve_default_members =
        [&]() -> CliResult<std::vector<const eta::package::WorkspaceMember*>> {
        std::vector<const eta::package::WorkspaceMember*> defaults;
        if (workspace_section.default_members.empty()) return defaults;

        std::unordered_set<std::string> seen;
        for (const auto& selector : workspace_section.default_members) {
            const eta::package::WorkspaceMember* matched = nullptr;

            if (auto by_name_it = by_name.find(selector); by_name_it != by_name.end()) {
                matched = by_name_it->second;
            } else {
                const auto normalized_selector = normalize_member_selector(selector);
                for (const auto& member : workspace.members) {
                    if (workspace_member_selector(member) == normalized_selector) {
                        matched = &member;
                        break;
                    }
                }
            }

            if (matched == nullptr) {
                return std::unexpected("eta " + command_name
                                       + ": workspace default-members entry does not match any member: "
                                       + selector);
            }
            if (seen.insert(matched->name).second) {
                defaults.push_back(matched);
            }
        }

        return defaults;
    };

    std::vector<const eta::package::WorkspaceMember*> selected_members;
    if (options.workspace) {
        if (!options.packages.empty()) {
            return std::unexpected("eta " + command_name
                                   + ": --workspace cannot be combined with -p/--package");
        }

        selected_members.reserve(workspace.members.size());
        for (const auto& member : workspace.members) {
            selected_members.push_back(&member);
        }

        std::unordered_set<std::string> excluded;
        for (const auto& package_name : options.exclude) {
            if (!by_name.contains(package_name)) {
                return std::unexpected("eta " + command_name
                                       + ": --exclude references unknown workspace package: "
                                       + package_name);
            }
            excluded.insert(package_name);
        }
        selected_members.erase(
            std::remove_if(selected_members.begin(),
                           selected_members.end(),
                           [&](const eta::package::WorkspaceMember* member) {
                               return excluded.contains(member->name);
                           }),
            selected_members.end());
    } else if (!options.packages.empty()) {
        if (!options.exclude.empty()) {
            return std::unexpected("eta " + command_name
                                   + ": --exclude requires --workspace");
        }

        std::unordered_set<std::string> seen;
        for (const auto& package_name : options.packages) {
            auto it = by_name.find(package_name);
            if (it == by_name.end()) {
                return std::unexpected("eta " + command_name
                                       + ": unknown workspace package: " + package_name);
            }
            if (seen.insert(package_name).second) {
                selected_members.push_back(it->second);
            }
        }
    } else {
        if (!options.exclude.empty()) {
            return std::unexpected("eta " + command_name
                                   + ": --exclude requires --workspace");
        }

        if (invocation == InvocationContext::WorkspaceMember && current_member != nullptr) {
            selected_members.push_back(current_member);
        } else {
            if (invocation == InvocationContext::WorkspaceNonMember
                && mode == SelectionCommandMode::SingleTarget) {
                return std::unexpected("eta " + command_name
                                       + ": workspace non-member directories require -p/--package");
            }
            if (mode == SelectionCommandMode::SingleTarget && root_member == nullptr) {
                return std::unexpected("eta " + command_name
                                       + ": virtual workspace commands require -p/--package");
            }

            auto defaults = resolve_default_members();
            if (!defaults) return std::unexpected(defaults.error());
            if (!defaults->empty()) {
                selected_members = std::move(*defaults);
            } else if (root_member != nullptr) {
                selected_members.push_back(root_member);
            } else if (mode == SelectionCommandMode::Aggregate) {
                selected_members.reserve(workspace.members.size());
                for (const auto& member : workspace.members) {
                    selected_members.push_back(&member);
                }
            } else {
                return std::unexpected("eta " + command_name
                                       + ": virtual workspace commands require -p/--package");
            }
        }
    }

    if (selected_members.empty()) {
        return std::unexpected("eta " + command_name + ": selection did not match any workspace members");
    }
    if (mode == SelectionCommandMode::SingleTarget && selected_members.size() != 1u) {
        return std::unexpected("eta " + command_name
                               + ": command requires a single selected package; use -p/--package");
    }

    ResolvedCommandTargets targets;
    targets.context = invocation;
    targets.workspace_manifest_path = workspace.workspace_manifest_path;
    targets.workspace_root = workspace.workspace_root;
    targets.selected.reserve(selected_members.size());
    for (const auto* member : selected_members) {
        targets.selected.push_back(SelectedPackageTarget{
            member->name,
            member->manifest_path,
            member->package_root,
        });
    }
    return targets;
}

} ///< namespace eta::cli
