/// Subcommands: new, init, tree, build, test, bench.

#include <iostream>

#include "eta/cli/cli_internal.h"

namespace eta::cli {

namespace {

CliResult<BuildOptions> parse_build_options(const std::vector<std::string>& args) {
    BuildOptions options;
    for (std::size_t i = 0; i < args.size(); ++i) {
        const auto& arg = args[i];
        if (arg == "--profile") {
            if (i + 1u >= args.size()) {
                return std::unexpected("eta build: --profile requires a value");
            }
            options.profile = args[++i];
            continue;
        }
        if (arg == "--bin") {
            if (i + 1u >= args.size()) {
                return std::unexpected("eta build: --bin requires a value");
            }
            options.bin_name = args[++i];
            continue;
        }
        return std::unexpected("eta build: unknown option " + arg);
    }
    if (options.profile != "release" && options.profile != "debug") {
        return std::unexpected("eta build: --profile must be 'release' or 'debug'");
    }
    return options;
}

CliResult<PathCommandOptions> parse_path_command_options(const std::string& command,
                                                         const std::vector<std::string>& args) {
    PathCommandOptions options;
    for (const auto& arg : args) {
        if (!arg.empty() && arg.front() == '-') {
            return std::unexpected("eta " + command + ": unknown option " + arg);
        }
        if (options.filter.has_value()) {
            return std::unexpected("eta " + command + ": accepts at most one filter path");
        }
        options.filter = arg;
    }
    return options;
}

} ///< namespace

CliResult<int> command_new(const char* program,
                           const std::vector<std::string>& args,
                           const fs::path& cwd) {
    ProjectKind kind = ProjectKind::Bin;
    std::optional<std::string> package_name;

    for (const auto& arg : args) {
        if (arg == "--help" || arg == "-h") {
            print_new_usage(program);
            return 0;
        }
        if (arg == "--bin") {
            kind = ProjectKind::Bin;
            continue;
        }
        if (arg == "--lib") {
            kind = ProjectKind::Lib;
            continue;
        }
        if (!arg.empty() && arg.front() == '-') {
            return std::unexpected("unknown option for eta new: " + arg);
        }
        if (package_name.has_value()) {
            return std::unexpected("eta new accepts exactly one <name> argument");
        }
        package_name = arg;
    }

    if (!package_name.has_value()) {
        return std::unexpected("eta new requires a package name");
    }
    if (!is_valid_package_name(*package_name)) {
        return std::unexpected(
            "package name must match [a-z][a-z0-9_-]{0,63}: " + *package_name);
    }

    return scaffold_project(cwd / *package_name, *package_name, kind, false);
}

CliResult<int> command_init(const char* program,
                            const std::vector<std::string>& args,
                            const fs::path& cwd) {
    ProjectKind kind = ProjectKind::Bin;

    for (const auto& arg : args) {
        if (arg == "--help" || arg == "-h") {
            print_init_usage(program);
            return 0;
        }
        if (arg == "--bin") {
            kind = ProjectKind::Bin;
            continue;
        }
        if (arg == "--lib") {
            kind = ProjectKind::Lib;
            continue;
        }
        return std::unexpected("eta init does not accept positional arguments: " + arg);
    }

    auto package_name = cwd.filename().string();
    if (package_name.empty()) {
        return std::unexpected("cannot infer package name from current directory");
    }
    if (!is_valid_package_name(package_name)) {
        return std::unexpected("current directory name is not a valid package name: " + package_name);
    }

    return scaffold_project(cwd, std::move(package_name), kind, true);
}

CliResult<int> command_tree(const char* program,
                            const std::vector<std::string>& args,
                            const fs::path& cwd) {
    auto parsed_args = parse_workspace_command_args("tree", args);
    if (!parsed_args) return std::unexpected(parsed_args.error());

    std::optional<std::size_t> max_depth;
    for (std::size_t i = 0; i < parsed_args->command_args.size(); ++i) {
        const auto& arg = parsed_args->command_args[i];
        if (arg == "--help" || arg == "-h") {
            print_tree_usage(program);
            return 0;
        }
        if (arg == "--depth") {
            if (i + 1u >= parsed_args->command_args.size()) {
                return std::unexpected("eta tree: --depth requires a value");
            }
            auto depth_res = parse_depth(parsed_args->command_args[++i]);
            if (!depth_res) return std::unexpected("eta tree: " + depth_res.error());
            max_depth = *depth_res;
            continue;
        }
        return std::unexpected("eta tree: unknown option " + arg);
    }

    auto targets = resolve_command_targets(
        "tree",
        cwd,
        parsed_args->workspace,
        SelectionCommandMode::Aggregate,
        "eta tree: could not find eta.toml (searched from " + cwd.string() + ")");
    if (!targets) return std::unexpected(targets.error());

    for (std::size_t i = 0; i < targets->selected.size(); ++i) {
        const auto& target = targets->selected[i];
        auto state = resolve_project_state(target.manifest_path, false, false, false);
        if (!state) return std::unexpected("eta tree: " + state.error());

        const auto* root = state->graph.find(state->graph.root_name);
        if (root == nullptr) {
            return std::unexpected("eta tree: resolved graph is missing root package");
        }

        std::unordered_map<std::string, const eta::package::ResolvedPackage*> by_name;
        by_name.reserve(state->graph.packages.size());
        for (const auto& package : state->graph.packages) {
            by_name.emplace(package.name, &package);
        }

        const auto root_dir = root->package_root;
        std::cout << root->name << " v" << root->version
                  << " (" << display_source(*root, root_dir) << ")\n";
        print_tree_children(std::cout, *root, root_dir, by_name, "", 0u, max_depth);
        if (i + 1u < targets->selected.size()) {
            std::cout << "\n";
        }
    }
    return 0;
}

CliResult<int> command_build(const char* program,
                             const char* argv0,
                             const std::vector<std::string>& args,
                             const fs::path& cwd) {
    auto parsed_args = parse_workspace_command_args("build", args);
    if (!parsed_args) return std::unexpected(parsed_args.error());

    for (const auto& arg : parsed_args->command_args) {
        if (arg == "--help" || arg == "-h") {
            print_build_usage(program);
            return 0;
        }
    }

    auto options = parse_build_options(parsed_args->command_args);
    if (!options) return std::unexpected(options.error());

    auto targets = resolve_command_targets(
        "build",
        cwd,
        parsed_args->workspace,
        SelectionCommandMode::Aggregate,
        "eta build: no eta.toml found");
    if (!targets) return std::unexpected(targets.error());

    for (const auto& target : targets->selected) {
        auto state = resolve_project_state(target.manifest_path, false, true, true);
        if (!state) return std::unexpected("eta build: " + state.error());

        auto rc = compile_project_sources(argv0, *state, *options);
        if (!rc) return std::unexpected(rc.error());
        if (*rc != 0) return rc;
    }
    return 0;
}

CliResult<int> command_test(const char* program,
                            const char* argv0,
                            const std::vector<std::string>& args,
                            const fs::path& cwd) {
    auto parsed_args = parse_workspace_command_args("test", args);
    if (!parsed_args) return std::unexpected(parsed_args.error());

    for (const auto& arg : parsed_args->command_args) {
        if (arg == "--help" || arg == "-h") {
            print_path_command_usage(program, "test", "tests");
            return 0;
        }
    }

    auto options = parse_path_command_options("test", parsed_args->command_args);
    if (!options) return std::unexpected(options.error());

    auto targets = resolve_command_targets(
        "test",
        cwd,
        parsed_args->workspace,
        SelectionCommandMode::Aggregate,
        "eta test: no eta.toml found");
    if (!targets) return std::unexpected(targets.error());

    for (const auto& target : targets->selected) {
        auto state = resolve_project_state(target.manifest_path, true, false, true);
        if (!state) return std::unexpected("eta test: " + state.error());

        const fs::path suite_root = state->project_root / "tests";
        auto rc = run_project_suite(argv0, *state, suite_root, *options);
        if (!rc) return std::unexpected(rc.error());
        if (*rc != 0) return rc;
    }
    return 0;
}

CliResult<int> command_bench(const char* program,
                             const char* argv0,
                             const std::vector<std::string>& args,
                             const fs::path& cwd) {
    auto parsed_args = parse_workspace_command_args("bench", args);
    if (!parsed_args) return std::unexpected(parsed_args.error());

    for (const auto& arg : parsed_args->command_args) {
        if (arg == "--help" || arg == "-h") {
            print_path_command_usage(program, "bench", "bench");
            return 0;
        }
    }

    auto options = parse_path_command_options("bench", parsed_args->command_args);
    if (!options) return std::unexpected(options.error());

    auto targets = resolve_command_targets(
        "bench",
        cwd,
        parsed_args->workspace,
        SelectionCommandMode::Aggregate,
        "eta bench: no eta.toml found");
    if (!targets) return std::unexpected(targets.error());

    for (const auto& target : targets->selected) {
        auto state = resolve_project_state(target.manifest_path, true, false, true);
        if (!state) return std::unexpected("eta bench: " + state.error());

        const fs::path suite_root = state->project_root / "bench";
        auto rc = run_project_suite(argv0, *state, suite_root, *options);
        if (!rc) return std::unexpected(rc.error());
        if (*rc != 0) return rc;
    }
    return 0;
}

} ///< namespace eta::cli
