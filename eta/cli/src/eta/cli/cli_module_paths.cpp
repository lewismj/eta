/// Module path assembly, tool discovery and compile/test drivers.

#include <algorithm>
#include <iostream>
#include <sstream>
#include <system_error>
#include <unordered_set>

#include "eta/cli/cli_internal.h"
#include "eta/util/path.h"
#include "eta/util/runtime_layout.h"

namespace eta::cli {

namespace {

void add_package_layout_dirs(const fs::path& package_root,
                             const std::optional<fs::path>& release_dir_override,
                             std::unordered_set<std::string>& seen,
                             std::vector<fs::path>& out) {
    auto add_unique = [&](const fs::path& candidate) {
        std::error_code ec;
        if (!fs::is_directory(candidate, ec) || ec) return;
        const auto key = path_key(candidate);
        if (seen.insert(key).second) out.push_back(canonicalize_path(candidate));
    };

    bool added = false;
    const fs::path release_dir =
        release_dir_override.has_value() ? *release_dir_override : (package_root / "target" / "release");
    add_unique(release_dir);
    if (seen.contains(path_key(release_dir))) added = true;

    const auto src_dir = package_root / "src";
    add_unique(src_dir);
    if (seen.contains(path_key(src_dir))) added = true;

    if (!added) add_unique(package_root);
}

[[nodiscard]] fs::path resolve_self_path(const char* argv0) {
    if (auto self = eta::util::current_executable_path(); self.has_value()) return *self;
    if (argv0 != nullptr && argv0[0] != '\0') {
        return canonicalize_path(fs::absolute(fs::path(argv0)));
    }
    return {};
}

void add_unique_existing_dir(const fs::path& candidate,
                             std::unordered_set<std::string>& seen,
                             std::vector<fs::path>& out) {
    std::error_code ec;
    if (!fs::is_directory(candidate, ec) || ec) return;
    const auto canonical = canonicalize_path(candidate);
    if (seen.insert(path_key(canonical)).second) out.push_back(canonical);
}

[[nodiscard]] bool is_stdlib_root(const fs::path& candidate) {
    std::error_code ec;
    if (!fs::is_directory(candidate, ec) || ec) return false;
    ec.clear();
    return fs::is_directory(candidate / "std", ec) && !ec;
}

void add_stdlib_from_ancestors(fs::path start,
                               std::unordered_set<std::string>& seen,
                               std::vector<fs::path>& out) {
    if (start.empty()) return;
    start = canonicalize_path(start);
    auto current = start;
    while (!current.empty()) {
        const auto candidate = current / "stdlib";
        if (is_stdlib_root(candidate)) {
            add_unique_existing_dir(candidate, seen, out);
            return;
        }
        const auto parent = current.parent_path();
        if (parent == current) return;
        current = parent;
    }
}

void add_stdlib_module_entry(const fs::path& project_root,
                             const char* argv0,
                             std::unordered_set<std::string>& seen,
                             std::vector<fs::path>& out) {
    add_stdlib_from_ancestors(project_root, seen, out);

    const auto self = resolve_self_path(argv0);
    if (!self.empty()) {
        const auto bundled = eta::util::bundled_stdlib_root_from_executable(self);
        if (is_stdlib_root(bundled)) {
            add_unique_existing_dir(bundled, seen, out);
        }
        add_stdlib_from_ancestors(self.parent_path(), seen, out);
    }
}

[[nodiscard]] std::optional<fs::path>
workspace_member_root_from_source(std::string_view source, const fs::path& workspace_root) {
    if (source.rfind("workspace+", 0) != 0) return std::nullopt;
    const std::string relative = std::string(source.substr(10u));
    if (relative.empty() || relative == ".") return canonicalize_path(workspace_root);

    const auto member_root = canonicalize_path(workspace_root / fs::path(relative));
    if (!is_path_within(workspace_root, member_root)) return std::nullopt;
    return member_root;
}

std::vector<fs::path> module_entries_from_graph(const ResolvedProjectState& state,
                                                const char* argv0) {
    std::vector<fs::path> entries;
    std::unordered_set<std::string> seen;

    const auto* root = state.graph.find(state.graph.root_name);
    if (root != nullptr) {
        const auto root_src = root->package_root / "src";
        std::error_code ec;
        if (fs::is_directory(root_src, ec) && !ec) {
            entries.push_back(canonicalize_path(root_src));
            seen.insert(path_key(root_src));
        } else {
            entries.push_back(canonicalize_path(root->package_root));
            seen.insert(path_key(root->package_root));
        }
    }

    for (const auto& package : state.graph.packages) {
        if (package.name == state.graph.root_name) continue;
        std::optional<fs::path> release_override;
        if (state.workspace_root.has_value() && package.source.rfind("workspace+", 0) == 0) {
            release_override = workspace_member_target_profile_root(
                *state.workspace_root, "release", package.name);
        }
        add_package_layout_dirs(package.package_root, release_override, seen, entries);
    }
    add_stdlib_module_entry(state.project_root, argv0, seen, entries);
    return entries;
}

[[nodiscard]] fs::path find_tool_path(const char* argv0,
                                      std::string_view tool_name,
                                      std::optional<fs::path> configured) {
    std::vector<fs::path> candidates;
    if (configured.has_value()) candidates.push_back(*configured);

    const auto self = resolve_self_path(argv0);
    if (!self.empty()) {
#ifdef _WIN32
        candidates.push_back(self.parent_path() / (std::string(tool_name) + ".exe"));
#else
        candidates.push_back(self.parent_path() / std::string(tool_name));
#endif
    }

    std::error_code ec;
    for (const auto& candidate : candidates) {
        if (candidate.empty()) continue;
        if (fs::is_regular_file(candidate, ec) && !ec) {
            return canonicalize_path(candidate);
        }
        ec.clear();
    }

#ifdef _WIN32
    return fs::path(std::string(tool_name) + ".exe");
#else
    return fs::path(std::string(tool_name));
#endif
}

[[nodiscard]] fs::path find_etai_path(const char* argv0) {
#ifdef ETA_ETAI_PATH
    return find_tool_path(argv0, "etai", fs::path(ETA_ETAI_PATH));
#else
    return find_tool_path(argv0, "etai", std::nullopt);
#endif
}

[[nodiscard]] fs::path find_etac_path(const char* argv0) {
#ifdef ETA_ETAC_PATH
    return find_tool_path(argv0, "etac", fs::path(ETA_ETAC_PATH));
#else
    return find_tool_path(argv0, "etac", std::nullopt);
#endif
}

[[nodiscard]] fs::path find_eta_test_path(const char* argv0) {
#ifdef ETA_ETA_TEST_PATH
    return find_tool_path(argv0, "eta_test", fs::path(ETA_ETA_TEST_PATH));
#else
    return find_tool_path(argv0, "eta_test", std::nullopt);
#endif
}

[[nodiscard]] fs::path find_eta_prof_path(const char* argv0) {
#ifdef ETA_ETA_PROF_PATH
    return find_tool_path(argv0, "eta_prof", fs::path(ETA_ETA_PROF_PATH));
#else
    return find_tool_path(argv0, "eta_prof", std::nullopt);
#endif
}

CliResult<int> run_with_etac(const char* argv0,
                             const std::vector<std::string>& args,
                             const std::optional<fs::path>& cwd = std::nullopt) {
    return run_external(find_etac_path(argv0), args, cwd);
}

CliResult<int> run_with_eta_test(const char* argv0,
                                 const std::vector<std::string>& args,
                                 const std::optional<fs::path>& cwd = std::nullopt) {
    return run_external(find_eta_test_path(argv0), args, cwd);
}

std::vector<fs::path> collect_eta_sources(const fs::path& root_dir) {
    std::vector<fs::path> files;
    std::error_code ec;
    if (!fs::is_directory(root_dir, ec) || ec) return files;
    for (const auto& entry : fs::recursive_directory_iterator(root_dir, ec)) {
        if (ec) break;
        std::error_code entry_ec;
        if (!entry.is_regular_file(entry_ec) || entry_ec) continue;
        if (entry.path().extension() == ".eta") files.push_back(entry.path());
    }
    std::sort(files.begin(), files.end());
    return files;
}

} ///< namespace

void print_tree_children(std::ostream& out,
                         const eta::package::ResolvedPackage& node,
                         const fs::path& root_dir,
                         const std::unordered_map<std::string, const eta::package::ResolvedPackage*>& by_name,
                         std::string prefix,
                         std::size_t depth,
                         std::optional<std::size_t> max_depth) {
    if (max_depth.has_value() && depth >= *max_depth) return;

    std::vector<const eta::package::ResolvedPackage*> children;
    children.reserve(node.dependency_names.size());
    for (const auto& dep_name : node.dependency_names) {
        if (auto it = by_name.find(dep_name); it != by_name.end()) {
            children.push_back(it->second);
        }
    }

    for (std::size_t i = 0; i < children.size(); ++i) {
        const bool is_last = i + 1u == children.size();
        const auto* child = children[i];
        out << prefix
            << (is_last ? "`-- " : "|-- ")
            << child->name << " v" << child->version
            << " (" << display_source(*child, root_dir) << ")\n";

        print_tree_children(out,
                            *child,
                            root_dir,
                            by_name,
                            prefix + (is_last ? "    " : "|   "),
                            depth + 1u,
                            max_depth);
    }
}

[[nodiscard]] std::string join_module_path_entries(const std::vector<fs::path>& entries) {
#ifdef _WIN32
    constexpr char separator = ';';
#else
    constexpr char separator = ':';
#endif

    std::ostringstream out;
    for (std::size_t i = 0; i < entries.size(); ++i) {
        if (i != 0u) out << separator;
        out << entries[i].string();
    }
    return out.str();
}

std::vector<fs::path> module_entries_from_lockfile(const ResolvedProjectState& state,
                                                   const char* argv0) {
    std::vector<fs::path> entries;
    std::unordered_set<std::string> seen;

    const auto root_src = state.project_root / "src";
    std::error_code ec;
    if (fs::is_directory(root_src, ec) && !ec) {
        entries.push_back(canonicalize_path(root_src));
        seen.insert(path_key(root_src));
    } else {
        entries.push_back(canonicalize_path(state.project_root));
        seen.insert(path_key(state.project_root));
    }

    const fs::path modules_root = state.modules_root;
    for (const auto& package : state.lockfile.packages) {
        if (package.source == "root") continue;
        if (auto workspace_member_root =
                workspace_member_root_from_source(package.source, state.lockfile_root);
            workspace_member_root.has_value()) {
            std::optional<fs::path> release_override;
            if (state.workspace_root.has_value()) {
                release_override = workspace_member_target_profile_root(
                    *state.workspace_root, "release", package.name);
            }
            add_package_layout_dirs(*workspace_member_root, release_override, seen, entries);
            continue;
        }
        add_package_layout_dirs(modules_root / (package.name + "-" + package.version),
                                std::nullopt,
                                 seen,
                                 entries);
    }
    add_stdlib_module_entry(state.project_root, argv0, seen, entries);
    return entries;
}

CliResult<int> run_with_etai(const char* argv0,
                             const std::vector<std::string>& args,
                             const std::optional<fs::path>& cwd) {
    return run_external(find_etai_path(argv0), args, cwd);
}

CliResult<int> run_with_eta_prof(const char* argv0,
                                 const std::vector<std::string>& args,
                                 const std::optional<fs::path>& cwd) {
    return run_external(find_eta_prof_path(argv0), args, cwd);
}

CliResult<int> compile_project_sources(const char* argv0,
                                       const ResolvedProjectState& state,
                                       const BuildOptions& options) {
    const fs::path src_root = state.project_root / "src";
    std::vector<fs::path> source_files;
    if (options.bin_name.has_value()) {
        const fs::path candidate =
            src_root / (module_name_from_package_name(*options.bin_name) + ".eta");
        if (!fs::is_regular_file(candidate)) {
            return std::unexpected("eta build: entry source not found: " + candidate.string());
        }
        source_files.push_back(candidate);
    } else {
        source_files = collect_eta_sources(src_root);
    }
    if (source_files.empty()) {
        return std::unexpected("eta build: no source files found under " + src_root.string());
    }

    const fs::path out_root = selected_package_target_profile_root(state, options.profile);
    if (auto mk = ensure_directory(out_root); !mk) return std::unexpected(mk.error());

    auto module_entries = module_entries_from_lockfile(state, argv0);
    const std::string module_path = join_module_path_entries(module_entries);

    for (const auto& source : source_files) {
        std::error_code ec;
        auto rel = fs::relative(source, src_root, ec);
        if (ec || rel.empty()) rel = source.filename();
        fs::path output = out_root / rel;
        output.replace_extension(".etac");
        if (auto mk = ensure_directory(output.parent_path()); !mk) {
            return std::unexpected(mk.error());
        }

        std::vector<std::string> etac_args;
        if (options.profile == "release") {
            etac_args.push_back("-O");
            etac_args.push_back("--no-debug");
        } else {
            etac_args.push_back("-O0");
        }
        if (!module_path.empty()) {
            etac_args.push_back("--path");
            etac_args.push_back(module_path);
        }
        etac_args.push_back(source.string());
        etac_args.push_back("-o");
        etac_args.push_back(output.string());

        auto rc = run_with_etac(argv0, etac_args, state.project_root);
        if (!rc) return std::unexpected(rc.error());
        if (*rc != 0) return rc;
    }

    std::cout << "compiled " << source_files.size() << " source file(s) to "
              << out_root.string() << "\n";
    return 0;
}

CliResult<int> run_project_suite(const char* argv0,
                                 const ResolvedProjectState& state,
                                 const fs::path& suite_root,
                                 const PathCommandOptions& options) {
    std::error_code ec;
    if (!fs::is_directory(suite_root, ec) || ec) {
        std::cout << "no suite directory: " << suite_root.string() << "\n";
        return 0;
    }

    auto module_entries = module_entries_from_graph(state, argv0);
    const std::string module_path = join_module_path_entries(module_entries);

    std::vector<std::string> test_args;
    if (!module_path.empty()) {
        test_args.push_back("--path");
        test_args.push_back(module_path);
    }

    if (options.filter.has_value()) {
        test_args.push_back(*options.filter);
    } else {
        test_args.push_back(suite_root.string());
    }

    auto rc = run_with_eta_test(argv0, test_args, state.project_root);
    if (!rc) return std::unexpected(rc.error());
    return *rc;
}

} ///< namespace eta::cli
