#include <iostream>

#include "eta/cli/cli_internal.h"

using namespace eta::cli;

namespace {

int print_not_yet_implemented(std::string_view command, std::string_view stage) {
    std::cerr << "NotYetImplemented(stage=" << stage << "): eta " << command << "\n";
    return 1;
}

} // namespace

int main(int argc, char* argv[]) {
    if (argc < 2) {
        print_usage(argv[0]);
        return 1;
    }

    const std::string command = argv[1];
    if (command == "--version" || command == "-V") {
        if (argc != 2) {
            std::cerr << "error: --version does not accept additional arguments\n";
            return 1;
        }
        print_version();
        return 0;
    }
    if (command == "--help" || command == "-h" || command == "help") {
        print_usage(argv[0]);
        return 0;
    }

    std::vector<std::string> args;
    args.reserve(static_cast<std::size_t>(argc - 2));
    for (int i = 2; i < argc; ++i) args.emplace_back(argv[i]);

    const fs::path cwd = canonicalize_path(fs::current_path());

    CliResult<int> result = std::unexpected("unknown command");
    if (command == "new") {
        result = command_new(argv[0], args, cwd);
    } else if (command == "init") {
        result = command_init(argv[0], args, cwd);
    } else if (command == "tree") {
        result = command_tree(argv[0], args, cwd);
    } else if (command == "run") {
        result = command_run(argv[0], argv[0], args, cwd);
    } else if (command == "prof") {
        result = command_prof(argv[0], argv[0], args, cwd);
    } else if (command == "add") {
        result = command_add(argv[0], args, cwd);
    } else if (command == "remove") {
        result = command_remove(argv[0], args, cwd);
    } else if (command == "update") {
        result = command_update(argv[0], args, cwd);
    } else if (command == "build") {
        result = command_build(argv[0], argv[0], args, cwd);
    } else if (command == "test") {
        result = command_test(argv[0], argv[0], args, cwd);
    } else if (command == "bench") {
        result = command_bench(argv[0], argv[0], args, cwd);
    } else if (command == "vendor") {
        result = command_vendor(argv[0], args, cwd);
    } else if (command == "install") {
        result = command_install(argv[0], argv[0], args, cwd);
    } else if (command == "clean") {
        result = command_clean(argv[0], args, cwd);
    } else {
        static const std::unordered_map<std::string, std::string> kNotYetCommands = {
            {"repl", "S7"},
            {"publish", "S8"},
            {"fmt", "post-v1"},
            {"doc", "post-v1"},
        };
        if (auto it = kNotYetCommands.find(command); it != kNotYetCommands.end()) {
            return print_not_yet_implemented(command, it->second);
        }
        std::cerr << "error: unknown command: " << command << "\n";
        print_usage(argv[0]);
        return 1;
    }

    if (!result) {
        std::cerr << "error: " << result.error() << "\n";
        return 1;
    }
    return *result;
}
