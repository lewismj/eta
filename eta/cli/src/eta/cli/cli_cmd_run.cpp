/// Subcommands: run, prof.

#include <unordered_set>

#include "eta/cli/cli_internal.h"

namespace eta::cli {

namespace {

CliResult<RunOptions> parse_run_options(const std::vector<std::string>& args) {
    RunOptions options;
    bool parsing_program_args = false;

    for (std::size_t i = 0; i < args.size(); ++i) {
        const auto& arg = args[i];
        if (parsing_program_args) {
            options.program_args.push_back(arg);
            continue;
        }
        if (arg == "--") {
            parsing_program_args = true;
            continue;
        }
        if (arg == "--profile") {
            if (i + 1u >= args.size()) {
                return std::unexpected("eta run: --profile requires a value");
            }
            options.profile = args[++i];
            continue;
        }
        if (arg == "--prof") {
            options.prof_mode = "sample";
            if (i + 1u < args.size()) {
                const auto& next = args[i + 1u];
                if (next == "trace" || next == "sample") {
                    options.prof_mode = next;
                    ++i;
                }
            }
            continue;
        }
        if (arg.rfind("--prof=", 0) == 0) {
            options.prof_mode = arg.substr(7u);
            continue;
        }
        if (arg == "--prof-out") {
            if (i + 1u >= args.size()) {
                return std::unexpected("eta run: --prof-out requires a value");
            }
            options.prof_out = fs::path(args[++i]);
            continue;
        }
        if (arg == "--prof-format") {
            if (i + 1u >= args.size()) {
                return std::unexpected("eta run: --prof-format requires a value");
            }
            options.prof_format = args[++i];
            continue;
        }
        if (arg.rfind("--prof-format=", 0) == 0) {
            options.prof_format = arg.substr(14u);
            continue;
        }
        if (arg == "--prof-hz") {
            if (i + 1u >= args.size()) {
                return std::unexpected("eta run: --prof-hz requires a value");
            }
            auto parsed_hz = parse_positive_u32(args[++i], "--prof-hz");
            if (!parsed_hz) return std::unexpected("eta run: " + parsed_hz.error());
            options.prof_hz = *parsed_hz;
            continue;
        }
        if (arg.rfind("--prof-hz=", 0) == 0) {
            auto parsed_hz = parse_positive_u32(arg.substr(10u), "--prof-hz");
            if (!parsed_hz) return std::unexpected("eta run: " + parsed_hz.error());
            options.prof_hz = *parsed_hz;
            continue;
        }
        if (arg == "--strict-shadows") {
            options.strict_shadows = true;
            continue;
        }
        if (arg == "--bin") {
            if (i + 1u >= args.size()) {
                return std::unexpected("eta run: --bin requires a value");
            }
            options.bin_name = args[++i];
            continue;
        }
        if (arg == "--example") {
            if (i + 1u >= args.size()) {
                return std::unexpected("eta run: --example requires a value");
            }
            options.example_name = args[++i];
            continue;
        }
        if (!arg.empty() && arg.front() == '-') {
            return std::unexpected("eta run: unknown option " + arg);
        }
        if (options.input_path.has_value()) {
            return std::unexpected("eta run accepts at most one positional file path");
        }
        options.input_path = fs::path(arg);
    }

    if (options.profile != "release" && options.profile != "debug") {
        return std::unexpected("eta run: --profile must be 'release' or 'debug'");
    }
    if (options.prof_mode.has_value()
        && *options.prof_mode != "trace"
        && *options.prof_mode != "sample") {
        return std::unexpected("eta run: --prof mode must be 'trace' or 'sample'");
    }
    if (options.prof_format.has_value()) {
        static const std::unordered_set<std::string> kFormats = {
            "pretty",
            "json",
            "speedscope",
            "eta-prof",
            "chrome",
            "pprof",
        };
        if (!kFormats.contains(*options.prof_format)) {
            return std::unexpected(
                "eta run: --prof-format must be one of pretty|json|speedscope|eta-prof|chrome|pprof");
        }
    }
    if ((options.prof_format.has_value() || options.prof_hz.has_value())
        && !options.prof_mode.has_value()) {
        return std::unexpected("eta run: --prof-format/--prof-hz require --prof");
    }
    if (options.input_path.has_value()
        && (options.bin_name.has_value() || options.example_name.has_value())) {
        return std::unexpected("eta run: positional file path cannot be combined with --bin/--example");
    }
    if (options.bin_name.has_value() && options.example_name.has_value()) {
        return std::unexpected("eta run: --bin and --example are mutually exclusive");
    }
    return options;
}

} ///< namespace

CliResult<int> command_run(const char* program,
                           const char* argv0,
                           const std::vector<std::string>& args,
                           const fs::path& cwd) {
    auto parsed_args = parse_workspace_command_args("run", args);
    if (!parsed_args) return std::unexpected(parsed_args.error());

    for (const auto& arg : parsed_args->command_args) {
        if (arg == "--help" || arg == "-h") {
            print_run_usage(program);
            return 0;
        }
    }

    auto parsed = parse_run_options(parsed_args->command_args);
    if (!parsed) return std::unexpected(parsed.error());
    RunOptions options = std::move(*parsed);

    if (options.input_path.has_value()) {
        if (parsed_args->workspace.workspace
            || !parsed_args->workspace.packages.empty()
            || !parsed_args->workspace.exclude.empty()
            || parsed_args->workspace.manifest_path.has_value()) {
            return std::unexpected("eta run: script mode does not accept workspace selection flags");
        }

        const fs::path input = fs::absolute(*options.input_path);
        if (!fs::exists(input)) {
            return std::unexpected("eta run: file not found: " + input.string());
        }

        std::vector<std::string> etai_args;
        if (options.prof_mode.has_value()) {
            etai_args.push_back("--prof");
            etai_args.push_back(*options.prof_mode);
        }
        if (options.prof_out.has_value()) {
            etai_args.push_back("--prof-out");
            etai_args.push_back(options.prof_out->string());
        }
        if (options.prof_format.has_value()) {
            etai_args.push_back("--prof-format");
            etai_args.push_back(*options.prof_format);
        }
        if (options.prof_hz.has_value()) {
            etai_args.push_back("--prof-hz");
            etai_args.push_back(std::to_string(*options.prof_hz));
        }
        if (options.strict_shadows) etai_args.push_back("--strict-shadows");
        etai_args.push_back(input.string());
        if (!options.program_args.empty()) {
            etai_args.push_back("--");
            etai_args.insert(etai_args.end(), options.program_args.begin(), options.program_args.end());
        }
        return run_with_etai(argv0, etai_args, cwd);
    }

    auto targets = resolve_command_targets(
        "run",
        cwd,
        parsed_args->workspace,
        SelectionCommandMode::SingleTarget,
        "eta run: no eta.toml found (or pass a file path for script mode)");
    if (!targets) return std::unexpected(targets.error());

    auto state = resolve_project_state(targets->selected.front().manifest_path, false, true, true);
    if (!state) return std::unexpected("eta run: " + state.error());

    fs::path run_file;
    if (options.example_name.has_value()) {
        fs::path example_rel = *options.example_name;
        if (example_rel.extension().empty()) example_rel += ".eta";
        run_file = state->project_root / "cookbook" / example_rel;
    } else if (options.bin_name.has_value()) {
        run_file = state->project_root / "src" / (module_name_from_package_name(*options.bin_name) + ".eta");
    } else {
        run_file = state->project_root / "src" / (module_name_from_package_name(state->manifest.name) + ".eta");
    }

    if (!fs::is_regular_file(run_file)) {
        return std::unexpected("eta run: entry file not found: " + run_file.string());
    }

    auto module_entries = module_entries_from_lockfile(*state, argv0);
    const std::string module_path = join_module_path_entries(module_entries);

    std::vector<std::string> etai_args;
    if (options.prof_mode.has_value()) {
        etai_args.push_back("--prof");
        etai_args.push_back(*options.prof_mode);
    }
    if (options.prof_out.has_value()) {
        etai_args.push_back("--prof-out");
        etai_args.push_back(options.prof_out->string());
    }
    if (options.prof_format.has_value()) {
        etai_args.push_back("--prof-format");
        etai_args.push_back(*options.prof_format);
    }
    if (options.prof_hz.has_value()) {
        etai_args.push_back("--prof-hz");
        etai_args.push_back(std::to_string(*options.prof_hz));
    }
    if (options.strict_shadows) etai_args.push_back("--strict-shadows");
    if (!module_path.empty()) {
        etai_args.push_back("--path");
        etai_args.push_back(module_path);
    }
    etai_args.push_back(fs::absolute(run_file).string());

    if (!options.program_args.empty()) {
        etai_args.push_back("--");
        etai_args.insert(etai_args.end(), options.program_args.begin(), options.program_args.end());
    }
    return run_with_etai(argv0, etai_args, state->project_root);
}

CliResult<int> command_prof(const char* program,
                            const char* argv0,
                            const std::vector<std::string>& args,
                            const fs::path& cwd) {
    if (args.empty()) {
        print_prof_usage(program);
        return std::unexpected("eta prof: missing subcommand (expected run|report|merge|view)");
    }

    if (args.front() == "--help" || args.front() == "-h") {
        print_prof_usage(program);
        return 0;
    }

    const std::string subcommand = args.front();
    if (subcommand == "report" || subcommand == "merge" || subcommand == "view") {
        std::vector<std::string> prof_args;
        prof_args.reserve(args.size());
        prof_args.insert(prof_args.end(), args.begin(), args.end());
        return run_with_eta_prof(argv0, prof_args, cwd);
    }

    if (subcommand != "run") {
        return std::unexpected("eta prof: unknown subcommand '" + subcommand + "'");
    }

    std::string mode = "sample";
    std::optional<std::string> format;
    std::optional<std::uint32_t> hz;
    std::optional<std::string> out;
    std::vector<std::string> forwarded_run_args;
    forwarded_run_args.reserve(args.size() + 8u);

    for (std::size_t i = 1; i < args.size(); ++i) {
        const auto& arg = args[i];
        if (arg == "--help" || arg == "-h") {
            print_prof_usage(program);
            return 0;
        }
        if (arg == "--mode") {
            if (i + 1u >= args.size()) {
                return std::unexpected("eta prof run: --mode requires a value");
            }
            mode = args[++i];
            continue;
        }
        if (arg.rfind("--mode=", 0) == 0) {
            mode = arg.substr(7u);
            continue;
        }
        if (arg == "--format") {
            if (i + 1u >= args.size()) {
                return std::unexpected("eta prof run: --format requires a value");
            }
            format = args[++i];
            continue;
        }
        if (arg.rfind("--format=", 0) == 0) {
            format = arg.substr(9u);
            continue;
        }
        if (arg == "--hz") {
            if (i + 1u >= args.size()) {
                return std::unexpected("eta prof run: --hz requires a value");
            }
            auto parsed_hz = parse_positive_u32(args[++i], "--hz");
            if (!parsed_hz) return std::unexpected("eta prof run: " + parsed_hz.error());
            hz = *parsed_hz;
            continue;
        }
        if (arg.rfind("--hz=", 0) == 0) {
            auto parsed_hz = parse_positive_u32(arg.substr(5u), "--hz");
            if (!parsed_hz) return std::unexpected("eta prof run: " + parsed_hz.error());
            hz = *parsed_hz;
            continue;
        }
        if (arg == "--out") {
            if (i + 1u >= args.size()) {
                return std::unexpected("eta prof run: --out requires a value");
            }
            out = args[++i];
            continue;
        }
        if (arg.rfind("--out=", 0) == 0) {
            out = arg.substr(6u);
            continue;
        }
        forwarded_run_args.push_back(arg);
    }

    if (mode != "trace" && mode != "sample") {
        return std::unexpected("eta prof run: --mode must be 'trace' or 'sample'");
    }

    std::vector<std::string> run_args;
    run_args.reserve(forwarded_run_args.size() + 10u);
    run_args.push_back("--prof");
    run_args.push_back(mode);
    if (hz.has_value()) {
        run_args.push_back("--prof-hz");
        run_args.push_back(std::to_string(*hz));
    }
    if (format.has_value()) {
        run_args.push_back("--prof-format");
        run_args.push_back(*format);
    }
    if (out.has_value()) {
        run_args.push_back("--prof-out");
        run_args.push_back(*out);
    }
    run_args.insert(run_args.end(), forwarded_run_args.begin(), forwarded_run_args.end());
    return command_run(program, argv0, run_args, cwd);
}

} ///< namespace eta::cli
