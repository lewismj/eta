#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "eta/runtime/prof/archive.h"
#include "eta/runtime/prof/pprof.h"

namespace fs = std::filesystem;

namespace {

[[nodiscard]] std::optional<std::string> read_file(const fs::path& path) {
    std::ifstream in(path, std::ios::in | std::ios::binary);
    if (!in) return std::nullopt;
    std::string content;
    in.seekg(0, std::ios::end);
    content.resize(static_cast<std::size_t>(in.tellg()));
    in.seekg(0, std::ios::beg);
    in.read(content.data(), static_cast<std::streamsize>(content.size()));
    if (!in.good() && !in.eof()) return std::nullopt;
    return content;
}

[[nodiscard]] bool write_file(const fs::path& path, std::string_view content) {
    std::error_code ec;
    if (path.has_parent_path()) fs::create_directories(path.parent_path(), ec);
    if (ec) return false;

    std::ofstream out(path, std::ios::out | std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out.write(content.data(), static_cast<std::streamsize>(content.size()));
    return out.good();
}

void print_usage(const char* argv0) {
    std::cerr
        << "Usage: " << argv0 << " <subcommand> [options]\n"
        << "\n"
        << "Subcommands:\n"
        << "  report [--format pretty|json|speedscope|chrome|pprof] FILE.eta-prof\n"
        << "  merge  --out OUT.eta-prof IN1.eta-prof IN2.eta-prof ...\n"
        << "  view   FILE.speedscope.json|FILE.eta-prof\n";
}

[[nodiscard]] std::optional<std::string> render_report(
    const eta::runtime::prof::ArchiveSession& session,
    const std::string& format,
    std::string& error) {
    if (format == "pretty") return eta::runtime::prof::render_pretty_archive_report(session);
    if (format == "json") return eta::runtime::prof::render_json_archive_report(session);
    if (format == "speedscope") return eta::runtime::prof::render_speedscope_archive_report(session);
    if (format == "chrome") return eta::runtime::prof::render_chrome_archive_report(session);
    if (format == "pprof") {
        auto pprof = eta::runtime::prof::write_pprof_profile(session);
        if (!pprof) {
            error = pprof.error();
            return std::nullopt;
        }
        return std::move(*pprof);
    }
    error = "unknown format '" + format + "'";
    return std::nullopt;
}

int command_report(const std::vector<std::string>& args) {
    std::string format = "pretty";
    std::optional<fs::path> input;

    for (std::size_t i = 0; i < args.size(); ++i) {
        const auto& arg = args[i];
        if (arg == "--help" || arg == "-h") {
            std::cerr << "Usage: eta_prof report [--format pretty|json|speedscope|chrome|pprof] FILE.eta-prof\n";
            return 0;
        }
        if (arg == "--format") {
            if (i + 1u >= args.size()) {
                std::cerr << "eta_prof report: --format requires a value\n";
                return 1;
            }
            format = args[++i];
            continue;
        }
        if (arg.rfind("--format=", 0) == 0) {
            format = arg.substr(9u);
            continue;
        }
        if (!arg.empty() && arg.front() == '-') {
            std::cerr << "eta_prof report: unknown option " << arg << "\n";
            return 1;
        }
        if (input.has_value()) {
            std::cerr << "eta_prof report: accepts exactly one input file\n";
            return 1;
        }
        input = fs::path(arg);
    }

    if (!input.has_value()) {
        std::cerr << "eta_prof report: missing input file\n";
        return 1;
    }

    auto content = read_file(*input);
    if (!content) {
        std::cerr << "eta_prof report: cannot read file: " << input->string() << "\n";
        return 1;
    }

    auto parsed = eta::runtime::prof::parse_eta_prof_archive(*content);
    if (!parsed) {
        std::cerr << "eta_prof report: " << parsed.error() << "\n";
        return 1;
    }

    std::string error;
    auto rendered = render_report(*parsed, format, error);
    if (!rendered) {
        std::cerr << "eta_prof report: " << error << "\n";
        return 1;
    }
    std::cout << *rendered;
    return 0;
}

int command_merge(const std::vector<std::string>& args) {
    std::optional<fs::path> out_path;
    std::vector<fs::path> inputs;

    for (std::size_t i = 0; i < args.size(); ++i) {
        const auto& arg = args[i];
        if (arg == "--help" || arg == "-h") {
            std::cerr << "Usage: eta_prof merge --out OUT.eta-prof IN1.eta-prof IN2.eta-prof ...\n";
            return 0;
        }
        if (arg == "--out") {
            if (i + 1u >= args.size()) {
                std::cerr << "eta_prof merge: --out requires a value\n";
                return 1;
            }
            out_path = fs::path(args[++i]);
            continue;
        }
        if (arg.rfind("--out=", 0) == 0) {
            out_path = fs::path(arg.substr(6u));
            continue;
        }
        if (!arg.empty() && arg.front() == '-') {
            std::cerr << "eta_prof merge: unknown option " << arg << "\n";
            return 1;
        }
        inputs.push_back(fs::path(arg));
    }

    if (!out_path.has_value()) {
        std::cerr << "eta_prof merge: --out is required\n";
        return 1;
    }
    if (inputs.empty()) {
        std::cerr << "eta_prof merge: expected at least one input archive\n";
        return 1;
    }

    std::vector<eta::runtime::prof::ArchiveSession> sessions;
    sessions.reserve(inputs.size());
    for (const auto& input : inputs) {
        auto content = read_file(input);
        if (!content) {
            std::cerr << "eta_prof merge: cannot read file: " << input.string() << "\n";
            return 1;
        }
        auto parsed = eta::runtime::prof::parse_eta_prof_archive(*content);
        if (!parsed) {
            std::cerr << "eta_prof merge: " << input.string() << ": " << parsed.error() << "\n";
            return 1;
        }
        sessions.push_back(std::move(*parsed));
    }

    auto merged = eta::runtime::prof::merge_eta_prof_archives(
        std::span<const eta::runtime::prof::ArchiveSession>(sessions.data(), sessions.size()));
    if (!merged) {
        std::cerr << "eta_prof merge: " << merged.error() << "\n";
        return 1;
    }

    const auto output = eta::runtime::prof::write_eta_prof_archive(*merged);
    if (!write_file(*out_path, output)) {
        std::cerr << "eta_prof merge: failed to write " << out_path->string() << "\n";
        return 1;
    }
    std::cout << "merged " << sessions.size() << " archive(s) -> " << out_path->string() << "\n";
    return 0;
}

int command_view(const std::vector<std::string>& args) {
    if (args.size() == 1u && (args.front() == "--help" || args.front() == "-h")) {
        std::cerr << "Usage: eta_prof view FILE.speedscope.json|FILE.eta-prof\n";
        return 0;
    }
    if (args.size() != 1u) {
        std::cerr << "eta_prof view: expected exactly one input file\n";
        return 1;
    }

    fs::path input = fs::path(args.front());
    if (!fs::is_regular_file(input)) {
        std::cerr << "eta_prof view: file not found: " << input.string() << "\n";
        return 1;
    }

    fs::path speedscope_path = input;
    if (input.extension() == ".eta-prof") {
        auto content = read_file(input);
        if (!content) {
            std::cerr << "eta_prof view: cannot read file: " << input.string() << "\n";
            return 1;
        }
        auto parsed = eta::runtime::prof::parse_eta_prof_archive(*content);
        if (!parsed) {
            std::cerr << "eta_prof view: " << parsed.error() << "\n";
            return 1;
        }
        auto speedscope = eta::runtime::prof::render_speedscope_archive_report(*parsed);
        speedscope_path.replace_extension(".speedscope.json");
        if (!write_file(speedscope_path, speedscope)) {
            std::cerr << "eta_prof view: failed to write " << speedscope_path.string() << "\n";
            return 1;
        }
    }

    const auto absolute = fs::absolute(speedscope_path).generic_string();
    std::cout << "Open in speedscope:\n";
    std::cout << "  https://www.speedscope.app/#profileURL=file://" << absolute << "\n";
    return 0;
}

} // namespace

int main(const int argc, char* argv[]) {
    if (argc < 2) {
        print_usage(argv[0]);
        return 1;
    }

    const std::string command = argv[1];
    if (command == "--help" || command == "-h" || command == "help") {
        print_usage(argv[0]);
        return 0;
    }

    std::vector<std::string> args;
    args.reserve(static_cast<std::size_t>(argc - 2));
    for (int i = 2; i < argc; ++i) {
        args.emplace_back(argv[i]);
    }

    if (command == "report") return command_report(args);
    if (command == "merge") return command_merge(args);
    if (command == "view") return command_view(args);

    std::cerr << "eta_prof: unknown subcommand '" << command << "'\n";
    print_usage(argv[0]);
    return 1;
}
