/// Version and usage text for each subcommand.

#include <iostream>

#include "eta/cli/cli_internal.h"

#ifndef ETA_CLI_VERSION
#error ETA_CLI_VERSION must be defined by CMake
#endif

namespace eta::cli {

void print_version() {
    std::cout << "eta " << ETA_CLI_VERSION << "\n";
}

void print_usage(const char* program) {
    std::cerr
        << "Usage: " << program << " <command> [options]\n"
        << "\n"
        << "Commands:\n"
        << "  eta new <name> [--bin|--lib]\n"
        << "  eta init [--bin|--lib]\n"
        << "  eta tree [--depth N] [workspace options]\n"
        << "  eta run [--profile <release|debug>] [--prof[=sample|trace]] [--prof-hz N] [--prof-format FMT] [--prof-out FILE] [--strict-shadows] [--bin NAME] [--example NAME] [file.eta] [-- args...] [workspace options]\n"
        << "  eta prof run [--mode sample|trace] [--hz N] [--format FMT] [--out FILE] [run options]\n"
        << "  eta prof report [--format pretty|json|speedscope|chrome|pprof] FILE.eta-prof\n"
        << "  eta prof merge --out OUT.eta-prof IN1.eta-prof IN2.eta-prof ...\n"
        << "  eta prof view FILE.speedscope.json|FILE.eta-prof\n"
        << "  eta add <pkg> [--path DIR|--git URL --rev SHA|--tarball PATH --sha256 HEX] [--dev] [workspace options]\n"
        << "  eta remove <pkg> [workspace options]\n"
        << "  eta update [<pkg>...] [workspace options]\n"
        << "  eta build [--profile <release|debug>] [--bin NAME] [workspace options]\n"
        << "  eta test [filter] [workspace options]\n"
        << "  eta bench [filter] [workspace options]\n"
        << "  eta vendor [--target DIR] [workspace options]\n"
        << "  eta install [<pkg>] [--global] [workspace options]\n"
        << "  eta clean [--all] [workspace options]\n"
        << "\n"
        << "Workspace options:\n"
        << "  --workspace            Select all workspace members\n"
        << "  -p, --package <name>   Select one or more workspace members\n"
        << "  --exclude <name>       Exclude package(s) when using --workspace\n"
        << "  --manifest-path <path> Explicit workspace/package eta.toml\n"
        << "\n"
        << "Global options:\n"
        << "  -h, --help             Show top-level help\n"
        << "  -V, --version          Show eta CLI version\n"
        << "\n"
        << "Use `eta <command> --help` for command-specific options.\n";
}

void print_new_usage(const char* program) {
    std::cerr << "Usage: " << program << " new <name> [--bin|--lib]\n";
}

void print_init_usage(const char* program) {
    std::cerr << "Usage: " << program << " init [--bin|--lib]\n";
}

void print_tree_usage(const char* program) {
    std::cerr << "Usage: " << program
              << " tree [--depth N] [--workspace] [-p NAME] [--exclude NAME] [--manifest-path PATH]\n";
}

void print_run_usage(const char* program) {
    std::cerr
        << "Usage: " << program
        << " run [--profile <release|debug>] [--prof[=sample|trace]] [--prof-hz N] [--prof-format FMT] [--prof-out FILE] [--strict-shadows] [--bin NAME] [--example NAME] [file.eta] [-- args...]"
        << " [--workspace] [-p NAME] [--exclude NAME] [--manifest-path PATH]\n";
}

void print_prof_usage(const char* program) {
    std::cerr
        << "Usage: " << program << " prof <subcommand> [options]\n"
        << "\n"
        << "Subcommands:\n"
        << "  run    [--mode sample|trace] [--hz N] [--format FMT] [--out FILE] [run options]\n"
        << "  report [--format pretty|json|speedscope|chrome|pprof] FILE.eta-prof\n"
        << "  merge  --out OUT.eta-prof IN1.eta-prof IN2.eta-prof ...\n"
        << "  view   FILE.speedscope.json|FILE.eta-prof\n"
        << "\n"
        << "Run options:\n"
        << "  [--profile <release|debug>] [--strict-shadows] [--bin NAME] [--example NAME] [file.eta] [-- args...]\n"
        << "  [--workspace] [-p NAME] [--exclude NAME] [--manifest-path PATH]\n"
        << "\n"
        << "Formats: pretty|json|speedscope|eta-prof|chrome|pprof\n";
}

void print_add_usage(const char* program) {
    std::cerr
        << "Usage: " << program
        << " add <pkg> [--path DIR|--git URL --rev SHA|--tarball PATH --sha256 HEX] [--dev]"
        << " [--workspace] [-p NAME] [--exclude NAME] [--manifest-path PATH]\n";
}

void print_remove_usage(const char* program) {
    std::cerr << "Usage: " << program
              << " remove <pkg> [--workspace] [-p NAME] [--exclude NAME] [--manifest-path PATH]\n";
}

void print_update_usage(const char* program) {
    std::cerr << "Usage: " << program
              << " update [<pkg>...] [--workspace] [-p NAME] [--exclude NAME] [--manifest-path PATH]\n";
}

void print_build_usage(const char* program) {
    std::cerr << "Usage: " << program
              << " build [--profile <release|debug>] [--bin NAME] [--workspace] [-p NAME] [--exclude NAME] [--manifest-path PATH]\n";
}

void print_path_command_usage(const char* program,
                              std::string_view command,
                              std::string_view dir) {
    std::cerr << "Usage: " << program
              << " " << command
              << " [filter] [--workspace] [-p NAME] [--exclude NAME] [--manifest-path PATH]\n";
    std::cerr << "If no filter is provided, eta " << command << " runs all tests under "
              << dir << "/\n";
}

void print_vendor_usage(const char* program) {
    std::cerr << "Usage: " << program
              << " vendor [--target DIR] [--workspace] [-p NAME] [--exclude NAME] [--manifest-path PATH]\n";
}

void print_install_usage(const char* program) {
    std::cerr << "Usage: " << program
              << " install [<pkg>] [--global] [--workspace] [-p NAME] [--exclude NAME] [--manifest-path PATH]\n";
}

void print_clean_usage(const char* program) {
    std::cerr << "Usage: " << program
              << " clean [--all] [--workspace] [-p NAME] [--exclude NAME] [--manifest-path PATH]\n";
}

} ///< namespace eta::cli
