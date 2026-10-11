#include "format_source.h"

#include <algorithm>
#include <atomic>
#include <charconv>
#include <csignal>
#include <fstream>
#include <iostream>
#include <thread>

#include "eta/format/cli/args.h"
#include "eta/tooling/files.h"

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#include <windows.h>
#endif

namespace {
    namespace fs = std::filesystem;
    using namespace eta;
    std::atomic_bool cancelled{false};
    const auto cancellation = std::shared_ptr<const std::atomic_bool>(&cancelled, [](const auto *) {});

    int aggregate(int a, int b) {
        if (a == 70 || b == 70) return 70;
        if (a == 2 || b == 2) return 2;
        return std::max(a, b);
    }

    void diagnostics(const format::FormatResult &result, std::string_view display, std::string_view source) {
        std::optional<cst::Source> positions;
        if (!result.diagnostics.empty()) positions.emplace(1, std::string(display), std::string(source));
        for (const auto &diagnostic : result.diagnostics) {
            std::cerr << display;
            if (diagnostic.location.span) {
                const auto position = positions->position(diagnostic.location.span->begin, cst::PositionEncoding::UnicodeScalar);
                if (position) std::cerr << ':' << position->line + 1 << ':' << position->character + 1;
            }
            std::cerr << ": error[" << diagnostic.code << "]: " << diagnostic.message << '\n';
        }
        if (result.output)
            for (const auto &region : result.passthrough)
                std::cerr << display << ": note: passthrough [" << region.span.begin << ',' << region.span.end << "): " << region.reason << '\n';
    }

    std::expected<std::string, std::string> read_stdin(std::size_t limit) {
        std::string input;
        char buffer[65536];
        while (std::cin) {
            if (cancelled.load(std::memory_order_relaxed)) return std::unexpected("stdin operation cancelled");
            std::cin.read(buffer, sizeof(buffer));
            const auto count = static_cast<std::size_t>(std::cin.gcount());
            if (count > limit - input.size()) return std::unexpected("stdin exceeds the input byte limit");
            input.append(buffer, count);
        }
        if (!std::cin.eof()) return std::unexpected("cannot read stdin");
        return input;
    }

    int run(const format::cli::Arguments &args) {
        if (args.help) { std::cout << format::cli::help_text; return 0; }
        if (args.version) { std::cout << "eta_fmt " << ETA_FMT_VERSION << '\n'; return 0; }
        if (args.stdin_input) {
            auto input = read_stdin(cst::Limits{}.input_bytes);
            if (!input) { std::cerr << args.stdin_filename << ": " << input.error() << '\n'; return 2; }
            cst::Operation operation;
            operation.identity = args.stdin_filename;
            operation.cancelled = cancellation;
            const auto result = format::format_source(*input, args.options, operation);
            diagnostics(result, args.stdin_filename, *input);
            if (!result.output) return result.exit_class;
            if (args.mode == format::cli::Mode::Stdout) std::cout << *result.output;
            else if (args.mode == format::cli::Mode::Diff) std::cout << tooling::unified_diff(*input, *result.output, args.stdin_filename);
            return args.mode != format::cli::Mode::Stdout && *input != *result.output ? 1 : 0;
        }
        auto selected = tooling::discover(args.paths, args.excludes, fs::current_path(), args.mode == format::cli::Mode::InPlace);
        int status = selected.issues.empty() ? 0 : 2;
        struct Work {
            std::optional<tooling::Snapshot> snapshot;
            format::FormatResult result;
            std::string error;
        };
        std::vector<Work> work(selected.files.size());
        std::atomic_size_t cursor{0};
        auto worker = [&] {
            for (;;) {
                const auto index = cursor.fetch_add(1);
                if (index >= work.size()) return;
                auto &item = work[index];
                try {
                    auto snapshot = tooling::read_snapshot(selected.files[index].path, args.mode == format::cli::Mode::InPlace);
                    if (!snapshot) { item.error = snapshot.error(); continue; }
                    item.snapshot = std::move(*snapshot);
                    cst::Operation operation;
                    operation.identity = selected.files[index].display;
                    operation.cancelled = cancellation;
                    item.result = format::format_source(item.snapshot->bytes, args.options, operation);
                } catch (const std::exception &error) { item.error = error.what(); item.result.exit_class = 70; }
            }
        };
        {
            std::vector<std::jthread> workers;
            const auto count = std::min({args.jobs, work.size(), std::size_t{64}});
            for (std::size_t i = 0; i < count; ++i) workers.emplace_back(worker);
        }
        const bool internal = std::any_of(work.begin(), work.end(), [](const auto &item) { return item.result.exit_class == 70; });
        struct Publication { std::string display; std::optional<std::size_t> index; std::string error; };
        std::vector<Publication> publication;
        for (std::size_t i = 0; i < work.size(); ++i) publication.push_back({selected.files[i].display, i, {}});
        for (const auto &issue : selected.issues) publication.push_back({issue.display, {}, issue.message});
        std::sort(publication.begin(), publication.end(), [](const auto &a, const auto &b) { return a.display < b.display; });
        for (const auto &entry : publication) {
            if (!entry.index) { std::cerr << entry.display << ": error: " << entry.error << '\n'; continue; }
            const auto i = *entry.index;
            auto &item = work[i];
            const auto &display = selected.files[i].display;
            if (!item.error.empty()) {
                std::cerr << display << ": error: " << item.error << '\n';
                status = aggregate(status, item.result.exit_class == 70 ? 70 : 2);
                continue;
            }
            diagnostics(item.result, display, item.snapshot->bytes);
            if (!item.result.output) { status = aggregate(status, item.result.exit_class); continue; }
            if (cancelled.load(std::memory_order_relaxed)) {
                std::cerr << display << ": error: operation cancelled before publication/commit\n";
                status = aggregate(status, 2);
                continue;
            }
            const auto &output = *item.result.output;
            const bool changed = output != item.snapshot->bytes;
            if (args.mode == format::cli::Mode::Stdout) std::cout << output;
            else if (args.mode == format::cli::Mode::Diff) {
                std::cout << tooling::unified_diff(item.snapshot->bytes, output, display);
                if (changed) status = aggregate(status, 1);
            } else if (args.mode == format::cli::Mode::Check) {
                if (changed) { std::cerr << display << ": formatting differs\n"; status = aggregate(status, 1); }
            } else if (!internal && changed) {
                tooling::CommitHooks hooks;
                hooks.cancelled = cancellation;
                const auto committed = tooling::replace_atomically(*item.snapshot, output, hooks);
                if (!committed.success) { std::cerr << display << ": error: " << committed.message << '\n'; status = aggregate(status, 2); }
            } else if (internal && changed) {
                std::cerr << display << ": note: update not attempted after an internal failure\n";
            }
        }
        return status;
    }

    int run_cli(int argc, char **argv) {
#ifdef _WIN32
        _setmode(_fileno(stdin), _O_BINARY);
        _setmode(_fileno(stdout), _O_BINARY);
#endif
        try {
            std::signal(SIGINT, [](int) { cancelled.store(true, std::memory_order_relaxed); });
#ifndef _WIN32
            std::signal(SIGPIPE, SIG_IGN);
#endif
            const auto args = format::cli::arguments(argc, argv);
            if (!args) { std::cerr << "eta_fmt: " << args.error() << '\n'; return 64; }
            int status = run(*args);
            std::cout.flush();
            if (!std::cout) { std::cerr << "eta_fmt: output write failed\n"; status = aggregate(status, 2); }
            return status;
        } catch (const std::exception &error) {
            std::cerr << "eta_fmt: internal error: " << error.what() << '\n';
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
        if (!size) { std::cerr << "eta_fmt: invalid Unicode argument\n"; return 64; }
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

