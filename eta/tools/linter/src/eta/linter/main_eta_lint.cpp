#include "eta/linter/lint.h"

#include <algorithm>
#include <atomic>
#include <iostream>
#include <sstream>
#include <vector>

#include "eta/linter/cli/args.h"
#include "eta/linter/cli/reporter.h"
#include "eta/linter/core/config.h"
#include "eta/linter/core/engine.h"
#include "eta/linter/core/registry.h"
#include "eta/tooling/files.h"

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#include <windows.h>
#endif

namespace {
    namespace fs = std::filesystem;
    using namespace eta;

    int aggregate(int a, int b) {
        if (a == 70 || b == 70) return 70;
        if (a == 2 || b == 2) return 2;
        return std::max(a, b);
    }

    struct LoadedConfig {
        std::filesystem::path path;
        linter::LintConfig config;
    };

    void emit(const std::vector<linter::FileReport> &reports, const linter::LintOptions &options) {
        if (options.format == "json") std::cout << linter::json_report(reports) << '\n';
        else if (options.format == "sarif") std::cout << linter::sarif_report(reports) << '\n';
        else if (options.format == "github") std::cout << linter::github_report(reports);
        else std::cout << linter::cli::pretty_report(reports, options.quiet);
    }

    void apply_config(const linter::LintConfig &config, linter::LintOptions &options) {
        if (options.preset.empty() && config.preset) options.preset = *config.preset;
        if (!options.max_warnings && config.max_warnings) options.max_warnings = config.max_warnings;
        options.includes.insert(options.includes.end(), config.includes.begin(), config.includes.end());
        options.excludes.insert(options.excludes.end(), config.excludes.begin(), config.excludes.end());
        options.no_rules.insert(options.no_rules.end(), config.disabled_rules.begin(), config.disabled_rules.end());
    }

    std::expected<std::optional<LoadedConfig>, std::string> load_config(const linter::cli::Arguments &args) {
        std::optional<std::filesystem::path> config_path = args.config;
        if (!config_path) config_path = linter::find_lint_config(fs::current_path());
        if (!config_path) return std::optional<LoadedConfig>{};
        auto loaded = linter::load_lint_config(*config_path);
        if (!loaded) return std::unexpected(loaded.error());
        return LoadedConfig{*config_path, *loaded};
    }

    std::string read_stdin(std::istream &in) {
        std::ostringstream buffer;
        buffer << in.rdbuf();
        return buffer.str();
    }

    int run(const linter::cli::Arguments &args) {
        if (args.help) { std::cout << linter::cli::help_text; return 0; }
        if (args.version) { std::cout << "eta_lint " << ETA_LINT_VERSION << '\n'; return 0; }
        if (args.list_rules) { std::cout << linter::rule_list_text(); return 0; }
        if (args.explain_rule) {
            const auto explanation = linter::rule_explanation(*args.explain_rule);
            if (!explanation) { std::cerr << "eta_lint: " << explanation.error() << '\n'; return 64; }
            std::cout << *explanation;
            return 0;
        }

        auto options = args.options;
        auto config = load_config(args);
        if (!config) { std::cerr << "eta_lint: " << config.error() << '\n'; return 64; }
        std::filesystem::path discovery_root = fs::current_path();
        if (*config) {
            apply_config((*config)->config, options);
            discovery_root = fs::absolute((*config)->path).parent_path();
        }

        const auto rules = linter::resolve_rules(options);
        if (!rules) { std::cerr << "eta_lint: " << rules.error() << '\n'; return 64; }

        if (options.stdin_input) {
            if (options.fix) { std::cerr << "eta_lint: --fix is not supported with --stdin\n"; return 64; }
            if (!args.paths.empty()) { std::cerr << "eta_lint: --stdin is exclusive with paths\n"; return 64; }
            auto report = linter::lint_source(read_stdin(std::cin), options.stdin_filename, *rules, options);
            const int status = report.exit_class;
            emit({report}, options);
            return status;
        }

        auto selected = tooling::discover(args.paths, options.excludes, discovery_root, options.fix);
        if (!options.includes.empty()) {
            std::vector<tooling::Selection> filtered;
            for (const auto &file : selected.files) {
                if (std::any_of(options.includes.begin(), options.includes.end(), [&](const auto &pattern) {
                        return tooling::glob_matches(pattern, file.display);
                    })) {
                    filtered.push_back(file);
                }
            }
            selected.files = std::move(filtered);
        }
        int status = selected.issues.empty() ? 0 : 2;
        std::vector<linter::FileReport> reports;
        reports.reserve(selected.files.size());
        for (const auto &issue : selected.issues)
            std::cerr << issue.display << ": error: " << issue.message << '\n';
        bool internal = false;
        for (const auto &file : selected.files) {
            auto snapshot = tooling::read_snapshot(file.path, options.fix);
            if (!snapshot) {
                std::cerr << file.display << ": error: " << snapshot.error() << '\n';
                status = aggregate(status, 2);
                continue;
            }
            auto report = linter::lint_source(std::move(snapshot->bytes), file.display, *rules, options);
            status = aggregate(status, report.exit_class);
            if (options.fix && report.fixed_source && !internal && report.exit_class < 70) {
                tooling::CommitHooks hooks;
                const auto committed = tooling::replace_atomically(*snapshot, *report.fixed_source, hooks);
                if (!committed.success) {
                    std::cerr << file.display << ": error: " << committed.message << '\n';
                    status = aggregate(status, 2);
                    report.exit_class = std::max(report.exit_class, 2);
                }
            }
            if (report.exit_class == 70) internal = true;
            if (options.fix && report.fixed_source && internal && report.exit_class < 70) {
                std::cerr << file.display << ": note: update not attempted after an internal failure\n";
            }
            reports.push_back(std::move(report));
        }
        emit(reports, options);
        return status;
    }

    int run_cli(int argc, char **argv) {
#ifdef _WIN32
        _setmode(_fileno(stdin), _O_BINARY);
        _setmode(_fileno(stdout), _O_BINARY);
#endif
        try {
            const auto args = linter::cli::arguments(argc, argv);
            if (!args) { std::cerr << "eta_lint: " << args.error() << '\n'; return 64; }
            int status = run(*args);
            std::cout.flush();
            if (!std::cout) { std::cerr << "eta_lint: output write failed\n"; status = aggregate(status, 2); }
            return status;
        } catch (const std::exception &error) {
            std::cerr << "eta_lint: internal error: " << error.what() << '\n';
            return 70;
        }
    }
}

#ifdef _WIN32
int wmain(int argc, wchar_t **argv) {
    std::vector<std::string> storage;
    storage.reserve(argc);
    for (int i = 0; i < argc; ++i) {
        const auto size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, argv[i], -1, nullptr, 0, nullptr, nullptr);
        if (!size) { std::cerr << "eta_lint: invalid Unicode argument\n"; return 64; }
        std::string text(size, '\0');
        if (!WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, argv[i], -1, text.data(), size, nullptr, nullptr)) return 64;
        text.pop_back();
        storage.push_back(std::move(text));
    }
    std::vector<char *> arguments;
    for (auto &text : storage) arguments.push_back(text.data());
    return run_cli(argc, arguments.data());
}
#else
int main(int argc, char **argv) { return run_cli(argc, argv); }
#endif

