#include <cstdlib>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <exception>
#include <optional>
#include <string>
#include <vector>

#include "eta/session/driver.h"
#include "eta/interpreter/module_path.h"
#include "eta/runtime/prof/archive.h"
#include "eta/runtime/prof/pprof.h"
#include "eta/runtime/prof/profiler.h"
#include "eta/runtime/vm/bytecode_serializer.h"
#include "eta/runtime/vm/disassembler.h"

namespace fs = std::filesystem;

static void print_usage(const char* prog) {
    std::cerr << "Usage: " << prog << " [options] <file.eta|file.etac> [--] [arg ...]\n"
              << "\n"
              << "Options:\n"
              << "  --path <dirs>       Search path for .eta modules (";
#ifdef _WIN32
    std::cerr << "semicolon";
#else
    std::cerr << "colon";
#endif
    std::cerr << "-separated).\n"
              << "                      Falls back to ETA_MODULE_PATH environment variable.\n"
              << "  --strict-shadows    Fail when a module resolves to multiple files.\n"
              << "  --mailbox <url>     nng endpoint to dial on startup (spawned child mode).\n"
              << "                      The child dials this endpoint to connect to the parent.\n"
              << "  --prof [sample|trace] Enable runtime profiling (default: sample).\n"
              << "  --prof-hz <int>     Sampling frequency when --prof sample is active.\n"
              << "  --prof-format <fmt> Report format: pretty|json|speedscope|eta-prof|chrome|pprof.\n"
              << "  --prof-out <file>   Write profiler report to a file.\n"
              << "  --disasm            Disassemble bytecode instead of executing.\n"
              << "  --help              Show this help message.\n";
}

int main(int argc, char* argv[]) {
    std::string cli_path;
    std::string input_file;
    std::vector<std::string> program_args;
    std::string mailbox_endpoint;  ///< --mailbox <url>  (spawned child mode)
    bool profiling_enabled = false;
    std::string profiler_mode{"sample"};
    std::optional<std::string> profiler_format;
    std::uint32_t profiler_hz = 1000;
    std::optional<fs::path> profiler_out_path;
    bool disasm_mode = false;
    bool strict_shadows = false;
    bool parsing_program_args = false;

    /// Parse CLI arguments
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];

        if (parsing_program_args) {
            program_args.push_back(arg);
            continue;
        }

        if (arg == "--") {
            parsing_program_args = true;
            continue;
        }

        if (arg == "--help" || arg == "-h") {
            print_usage(argv[0]);
            return 0;
        }

        if (arg == "--disasm") {
            disasm_mode = true;
            continue;
        }

        if (arg == "--strict-shadows") {
            strict_shadows = true;
            continue;
        }

        if (arg == "--prof") {
            profiling_enabled = true;
            if (i + 1 < argc) {
                const std::string next = argv[i + 1];
                if (next == "trace" || next == "sample") {
                    profiler_mode = next;
                    ++i;
                }
            }
            continue;
        }

        if (arg.rfind("--prof=", 0) == 0) {
            profiling_enabled = true;
            profiler_mode = arg.substr(7);
            continue;
        }

        if (arg == "--prof-out") {
            if (i + 1 >= argc) {
                std::cerr << "error: --prof-out requires a value\n";
                return 1;
            }
            profiler_out_path = fs::path(argv[++i]);
            continue;
        }

        if (arg == "--prof-format") {
            if (i + 1 >= argc) {
                std::cerr << "error: --prof-format requires a value\n";
                return 1;
            }
            profiler_format = argv[++i];
            continue;
        }

        if (arg.rfind("--prof-format=", 0) == 0) {
            profiler_format = arg.substr(14);
            continue;
        }

        if (arg == "--prof-hz") {
            if (i + 1 >= argc) {
                std::cerr << "error: --prof-hz requires a value\n";
                return 1;
            }
            arg = argv[++i];
            try {
                const auto parsed = std::stoul(arg);
                if (parsed == 0) {
                    std::cerr << "error: --prof-hz must be a positive integer\n";
                    return 1;
                }
                profiler_hz = static_cast<std::uint32_t>(parsed);
            } catch (const std::exception&) {
                std::cerr << "error: --prof-hz must be a positive integer\n";
                return 1;
            }
            continue;
        }

        if (arg.rfind("--prof-hz=", 0) == 0) {
            const auto value = arg.substr(10);
            try {
                const auto parsed = std::stoul(value);
                if (parsed == 0) {
                    std::cerr << "error: --prof-hz must be a positive integer\n";
                    return 1;
                }
                profiler_hz = static_cast<std::uint32_t>(parsed);
            } catch (const std::exception&) {
                std::cerr << "error: --prof-hz must be a positive integer\n";
                return 1;
            }
            continue;
        }

        if (arg == "--path") {
            if (i + 1 >= argc) {
                std::cerr << "error: --path requires a value\n";
                return 1;
            }
            cli_path = argv[++i];
            continue;
        }

        if (arg == "--mailbox") {
            if (i + 1 >= argc) {
                std::cerr << "error: --mailbox requires a value\n";
                return 1;
            }
            mailbox_endpoint = argv[++i];
            continue;
        }

        /// First non-option argument is the input file
        if (input_file.empty()) {
            input_file = arg;
        } else {
            program_args.push_back(arg);
        }
    }

    if (input_file.empty()) {
        std::cerr << "error: no input file specified\n";
        print_usage(argv[0]);
        return 1;
    }
    if (profiling_enabled
        && profiler_mode != "trace"
        && profiler_mode != "sample") {
        std::cerr << "error: --prof mode must be 'trace' or 'sample'\n";
        return 1;
    }
    if (disasm_mode && profiling_enabled) {
        std::cerr << "error: --prof is not supported with --disasm\n";
        return 1;
    }
    if (!profiling_enabled && (profiler_format.has_value() || profiler_hz != 1000)) {
        std::cerr << "error: --prof-format/--prof-hz require --prof\n";
        return 1;
    }

    /// Validate input file
    fs::path file_path(input_file);
    if (!fs::exists(file_path)) {
        std::cerr << "error: file not found: " << input_file << "\n";
        return 1;
    }

    /// Build module path resolver
    auto resolver =
        eta::interpreter::ModulePathResolver::from_args_or_env(cli_path, strict_shadows);

    /**
     * Also add the directory containing the input file so sibling
     * modules can be found without an explicit --path.
     */
    auto parent_dir = fs::absolute(file_path).parent_path();
    resolver.add_dir(parent_dir);

    /// Create driver (pass argv[0] as the etai path)
    std::string self_path = argv[0];
#if defined(__linux__)
    {
        std::error_code ec;
        auto resolved = fs::read_symlink("/proc/self/exe", ec);
        if (!ec) self_path = resolved.string();
    }
#endif
    const std::size_t heap_bytes =
        eta::session::Driver::parse_heap_env_var("ETA_HEAP_SOFT_LIMIT");
    eta::session::Driver driver(std::move(resolver), heap_bytes, self_path, std::move(program_args));

    auto resolve = driver.file_resolver();
    /// Detect .etac extension for pre-compiled bytecode.
    const bool is_etac = file_path.extension() == ".etac";

    /// Install mailbox socket if we are a spawned child
#ifdef ETA_HAS_NNG
    if (!mailbox_endpoint.empty()) {
        if (!driver.install_mailbox(mailbox_endpoint)) {
            std::cerr << "error: --mailbox: failed to connect to parent at "
                      << mailbox_endpoint << "\n";
            return 1;
        }
    }
#else
    if (!mailbox_endpoint.empty()) {
        std::cerr << "warning: --mailbox ignored (built without ETA_HAS_NNG)\n";
    }
#endif
    std::optional<std::uint64_t> profiler_session;
    if (profiling_enabled) {
        auto& profiler = eta::runtime::prof::runtime_profiler();
        auto session = (profiler_mode == "sample")
            ? profiler.start_sample_session(profiler_hz)
            : profiler.start_trace_session();
        if (!session) {
            std::cerr << "error: failed to start profiler session\n";
            return 1;
        }
        profiler_session = *session;
    }

    auto emit_profile_report = [&]() -> bool {
        if (!profiler_session.has_value()) return true;

        auto& profiler = eta::runtime::prof::runtime_profiler();
        const bool stopped = (profiler_mode == "sample")
            ? profiler.stop_sample_session(*profiler_session)
            : profiler.stop_trace_session(*profiler_session);
        if (!stopped) {
            std::cerr << "error: failed to stop profiler session\n";
            return false;
        }

        const std::string format = profiler_format.has_value()
            ? *profiler_format
            : (profiler_mode == "sample" ? "speedscope" : "pretty");

        std::optional<std::string> report;
        if (format == "pretty") {
            report = profiler.render_pretty_report_for_session(*profiler_session);
        } else if (format == "json") {
            report = profiler.render_json_report_for_session(*profiler_session);
        } else if (format == "speedscope") {
            report = profiler.render_speedscope_report_for_session(*profiler_session);
        } else if (format == "eta-prof") {
            report = profiler.render_archive_report_for_session(*profiler_session);
        } else if (format == "chrome") {
            report = profiler.render_chrome_report_for_session(*profiler_session);
        } else if (format == "pprof") {
            auto archive_json = profiler.render_archive_report_for_session(*profiler_session);
            if (!archive_json) {
                std::cerr << "error: failed to render profiler archive for pprof export\n";
                return false;
            }
            auto parsed_archive = eta::runtime::prof::parse_eta_prof_archive(*archive_json);
            if (!parsed_archive) {
                std::cerr << "error: failed to parse profiler archive for pprof export: "
                          << parsed_archive.error() << "\n";
                return false;
            }
            auto pprof = eta::runtime::prof::write_pprof_profile(*parsed_archive);
            if (!pprof) {
                std::cerr << "error: " << pprof.error() << "\n";
                return false;
            }
            report = std::move(*pprof);
        } else {
            std::cerr << "error: --prof-format must be one of pretty|json|speedscope|eta-prof|chrome|pprof\n";
            return false;
        }

        if (!report) {
            std::cerr << "error: failed to render profiler report\n";
            return false;
        }

        if (profiler_out_path.has_value()) {
            std::ofstream out(*profiler_out_path, std::ios::out | std::ios::binary);
            if (!out) {
                std::cerr << "error: cannot open profiler output file: "
                          << profiler_out_path->string() << "\n";
                return false;
            }
            out << *report;
        } else {
            std::cout << *report;
        }
        return true;
    };

    if (is_etac) {
        if (disasm_mode) {
            /// Load and disassemble the .etac file
            std::ifstream in(file_path, std::ios::in | std::ios::binary);
            if (!in) { std::cerr << "error: cannot open " << input_file << "\n"; return 1; }
            eta::runtime::vm::BytecodeSerializer serializer(driver.heap(), driver.intern_table());
            auto etac = serializer.deserialize(in);
            if (!etac) { std::cerr << "error: " << eta::runtime::vm::to_string(etac.error()) << "\n"; return 1; }
            eta::runtime::vm::Disassembler disasm(driver.heap(), driver.intern_table());
            disasm.disassemble_all(etac->registry, std::cout);
            return 0;
        }
        if (!driver.run_etac_file(fs::absolute(file_path))) {
            driver.diagnostics().print_all(std::cerr, /*use_color=*/true, resolve);
            if (!emit_profile_report()) return 1;
            return 1;
        }
    } else {
        if (disasm_mode) {
            /**
             * For disasm mode we still run_file (which compiles + executes),
             * then dump the registry. A compile-only path could be added later.
             */
            if (!driver.run_file(fs::absolute(file_path))) {
                driver.diagnostics().print_all(std::cerr, /*use_color=*/true, resolve);
                return 1;
            }
            eta::runtime::vm::Disassembler disasm(driver.heap(), driver.intern_table());
            disasm.disassemble_all(driver.registry(), std::cout);
            return 0;
        }
        /// Execute the user's file
        if (!driver.run_file(fs::absolute(file_path))) {
            driver.diagnostics().print_all(std::cerr, /*use_color=*/true, resolve);
            if (!emit_profile_report()) return 1;
            return 1;
        }
    }

    if (!emit_profile_report()) return 1;
    return 0;
}


