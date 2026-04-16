#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include "eta/interpreter/driver.h"
#include "eta/interpreter/module_path.h"
#include "eta/runtime/vm/disassembler.h"

namespace fs = std::filesystem;

static void print_usage(const char* prog) {
    std::cerr << "Usage: " << prog << " [options] <file.eta|file.etac>\n"
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
              << "  --mailbox <url>     nng endpoint to dial on startup (spawned child mode).\n"
              << "                      The child dials this endpoint to connect to the parent.\n"
              << "  --disasm            Disassemble bytecode instead of executing.\n"
              << "  --help              Show this help message.\n";
}

int main(int argc, char* argv[]) {
    std::string cli_path;
    std::string input_file;
    std::string mailbox_endpoint;  // --mailbox <url>  (spawned child mode)
    bool disasm_mode = false;

    // Parse CLI arguments
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];

        if (arg == "--help" || arg == "-h") {
            print_usage(argv[0]);
            return 0;
        }

        if (arg == "--disasm") {
            disasm_mode = true;
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

        // First non-option argument is the input file
        if (input_file.empty()) {
            input_file = arg;
        } else {
            std::cerr << "error: unexpected argument: " << arg << "\n";
            print_usage(argv[0]);
            return 1;
        }
    }

    if (input_file.empty()) {
        std::cerr << "error: no input file specified\n";
        print_usage(argv[0]);
        return 1;
    }

    // Validate input file
    fs::path file_path(input_file);
    if (!fs::exists(file_path)) {
        std::cerr << "error: file not found: " << input_file << "\n";
        return 1;
    }

    // Build module path resolver
    auto resolver = eta::interpreter::ModulePathResolver::from_args_or_env(cli_path);

    // Also add the directory containing the input file so sibling
    // modules can be found without an explicit --path.
    auto parent_dir = fs::absolute(file_path).parent_path();
    resolver.add_dir(parent_dir);

    // Create driver (pass argv[0] as the etai path)
    std::string self_path = argv[0];
#if defined(__linux__)
    {
        std::error_code ec;
        auto resolved = fs::read_symlink("/proc/self/exe", ec);
        if (!ec) self_path = resolved.string();
    }
#endif
    const std::size_t heap_bytes =
        eta::interpreter::Driver::parse_heap_env_var("ETA_HEAP_SOFT_LIMIT");
    eta::interpreter::Driver driver(std::move(resolver), heap_bytes, self_path);

    auto resolve = driver.file_resolver();

    // Install mailbox socket if we are a spawned child
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

    // Load prelude (if available in module path)
    {
        auto pr = driver.load_prelude();
        if (pr.found && !pr.loaded) {
            driver.diagnostics().print_all(std::cerr, /*use_color=*/true, resolve);
            return 1;
        }
    }

    // Detect .etac extension for pre-compiled bytecode
    bool is_etac = file_path.extension() == ".etac";

    if (is_etac) {
        if (disasm_mode) {
            // Load and disassemble the .etac file
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
            return 1;
        }
    } else {
        if (disasm_mode) {
            // Compile but don't execute — just disassemble
            // For disasm mode we still run_file (which compiles + executes),
            // then dump the registry. A compile-only path could be added later.
            if (!driver.run_file(fs::absolute(file_path))) {
                driver.diagnostics().print_all(std::cerr, /*use_color=*/true, resolve);
                return 1;
            }
            eta::runtime::vm::Disassembler disasm(driver.heap(), driver.intern_table());
            disasm.disassemble_all(driver.registry(), std::cout);
            return 0;
        }
        // Execute the user's file
        if (!driver.run_file(fs::absolute(file_path))) {
            driver.diagnostics().print_all(std::cerr, /*use_color=*/true, resolve);
            return 1;
        }
    }

    return 0;
}

