#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

#include "eta/interpreter/driver.h"
#include "eta/interpreter/module_path.h"

namespace fs = std::filesystem;

static void print_usage(const char* prog) {
    std::cerr << "Usage: " << prog << " [options] <file.eta>\n"
              << "\n"
              << "Options:\n"
              << "  --path <dirs>   Search path for .eta modules (";
#ifdef _WIN32
    std::cerr << "semicolon";
#else
    std::cerr << "colon";
#endif
    std::cerr << "-separated).\n"
              << "                  Falls back to ETA_MODULE_PATH environment variable.\n"
              << "  --help          Show this help message.\n";
}

int main(int argc, char* argv[]) {
    std::string cli_path;
    std::string input_file;

    // ── Parse CLI arguments ──────────────────────────────────────────
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];

        if (arg == "--help" || arg == "-h") {
            print_usage(argv[0]);
            return 0;
        }

        if (arg == "--path") {
            if (i + 1 >= argc) {
                std::cerr << "error: --path requires a value\n";
                return 1;
            }
            cli_path = argv[++i];
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

    // ── Validate input file ──────────────────────────────────────────
    fs::path file_path(input_file);
    if (!fs::exists(file_path)) {
        std::cerr << "error: file not found: " << input_file << "\n";
        return 1;
    }

    // ── Build module path resolver ───────────────────────────────────
    auto resolver = eta::interpreter::ModulePathResolver::from_args_or_env(cli_path);

    // Also add the directory containing the input file so sibling
    // modules can be found without an explicit --path.
    auto parent_dir = fs::absolute(file_path).parent_path();
    resolver.add_dir(parent_dir);

    // ── Create driver and run ────────────────────────────────────────
    eta::interpreter::Driver driver(std::move(resolver));

    // Load prelude (if available in module path)
    if (!driver.load_prelude()) {
        driver.diagnostics().print_all(std::cerr, /*use_color=*/true);
        return 1;
    }

    // Execute the user's file
    if (!driver.run_file(fs::absolute(file_path))) {
        driver.diagnostics().print_all(std::cerr, /*use_color=*/true);
        return 1;
    }

    return 0;
}

