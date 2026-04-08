#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

#include "eta/interpreter/driver.h"
#include "eta/interpreter/module_path.h"
#include "eta/runtime/vm/bytecode_serializer.h"
#include "eta/runtime/vm/disassembler.h"
#include "eta/semantics/optimization_pipeline.h"
#include "eta/semantics/passes/constant_folding.h"
#include "eta/semantics/passes/dead_code_elimination.h"

namespace fs = std::filesystem;

static void print_usage(const char* prog) {
    std::cerr << "Usage: " << prog << " [options] <file.eta> [-o <file.etac>]\n"
              << "\nOptions:\n"
              << "  -o <output>     Output file (default: <input>.etac).\n"
              << "  -O, --optimize  Enable IR optimization passes.\n"
              << "  -O0             Disable optimization (default).\n"
              << "  --disasm        Print disassembly to stdout instead of writing .etac.\n"
              << "  --no-debug      Strip debug info (source maps) from output.\n"
              << "  --path <dirs>   Module search path.\n"
              << "  --help          Show this help message.\n";
}

int main(int argc, char* argv[]) {
    std::string cli_path;
    std::string input_file;
    std::string output_file;
    bool disasm_mode  = false;
    bool include_debug = true;
    bool optimize = false;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--help" || arg == "-h") { print_usage(argv[0]); return 0; }
        if (arg == "--disasm")  { disasm_mode = true; continue; }
        if (arg == "--no-debug") { include_debug = false; continue; }
        if (arg == "--optimize" || arg == "-O") { optimize = true; continue; }
        if (arg == "-O0") { optimize = false; continue; }
        if (arg == "--path") {
            if (i + 1 >= argc) { std::cerr << "error: --path requires a value\n"; return 1; }
            cli_path = argv[++i];
            continue;
        }
        if (arg == "-o") {
            if (i + 1 >= argc) { std::cerr << "error: -o requires a value\n"; return 1; }
            output_file = argv[++i];
            continue;
        }
        if (input_file.empty()) { input_file = arg; }
        else { std::cerr << "error: unexpected argument: " << arg << "\n"; return 1; }
    }

    if (input_file.empty()) {
        std::cerr << "error: no input file specified\n";
        print_usage(argv[0]);
        return 1;
    }

    fs::path file_path(input_file);
    if (!fs::exists(file_path)) {
        std::cerr << "error: file not found: " << input_file << "\n";
        return 1;
    }

    if (output_file.empty()) {
        output_file = fs::path(input_file).replace_extension(".etac").string();
    }

    // ── Read source for hashing ──────────────────────────────────────
    std::ifstream src_in(file_path, std::ios::in | std::ios::binary);
    if (!src_in) { std::cerr << "error: cannot open " << input_file << "\n"; return 1; }
    std::ostringstream src_buf;
    src_buf << src_in.rdbuf();
    std::string source_text = src_buf.str();
    src_in.close();

    uint64_t source_hash = eta::runtime::vm::BytecodeSerializer::hash_source(source_text);

    // ── Build module path resolver ───────────────────────────────────
    auto resolver = eta::interpreter::ModulePathResolver::from_args_or_env(cli_path);
    resolver.add_dir(fs::absolute(file_path).parent_path());

    // ── Create driver ────────────────────────────────────────────────
    eta::interpreter::Driver driver(std::move(resolver));

    // ── Configure optimization pipeline ──────────────────────────────
    if (optimize) {
        auto& pipeline = driver.optimization_pipeline();
        pipeline.add_pass(std::make_unique<eta::semantics::passes::ConstantFolding>());
        pipeline.add_pass(std::make_unique<eta::semantics::passes::DeadCodeElimination>());
    }

    // Load prelude
    {
        auto pr = driver.load_prelude();
        if (pr.found && !pr.loaded) {
            driver.diagnostics().print_all(std::cerr, true);
            return 1;
        }
    }

    // Record registry size before compilation so we know which
    // functions belong to this file.
    uint32_t base_func_idx = static_cast<uint32_t>(driver.registry().size());

    // Compile + execute (execution is needed for side-effects like
    // populating module tables; a pure compile-only path can be
    // added later).
    if (!driver.run_file(fs::absolute(file_path))) {
        driver.diagnostics().print_all(std::cerr, true);
        return 1;
    }

    // ── Disassemble mode ─────────────────────────────────────────────
    if (disasm_mode) {
        eta::runtime::vm::Disassembler disasm(driver.heap(), driver.intern_table());
        disasm.disassemble_all(driver.registry(), std::cout);
        return 0;
    }

    // ── Build module entries from what was just compiled ──────────────
    // We don't have per-module function index tracking yet, so we
    // create a single entry for the file.  A more precise per-module
    // mapping can be added once the emitter returns per-module metadata.

    // For now, collect all functions added by this file.
    uint32_t end_func_idx = static_cast<uint32_t>(driver.registry().size());

    // Build a sub-registry of just this file's functions.
    eta::semantics::BytecodeFunctionRegistry file_registry;
    std::vector<eta::runtime::vm::ModuleEntry> module_entries;

    // We treat the last function as the module init (the emitter adds it last).
    // In practice each (module ...) form in the file generates one _init.
    // We scan function names to find _init functions.
    const auto& all_funcs = driver.registry().all();
    for (uint32_t i = base_func_idx; i < end_func_idx; ++i) {
        const auto& func = all_funcs[i];
        file_registry.add(eta::runtime::vm::BytecodeFunction(func));

        // Detect _init functions to create module entries
        if (func.name.size() > 5 && func.name.ends_with("_init")) {
            std::string mod_name = func.name.substr(0, func.name.size() - 5);
            eta::runtime::vm::ModuleEntry entry;
            entry.name = mod_name;
            entry.init_func_index = i - base_func_idx;
            entry.total_globals = static_cast<uint32_t>(driver.vm().globals().size());
            // main_func_slot is not easily recoverable here; leave as nullopt for now.
            module_entries.push_back(std::move(entry));
        }
    }

    if (module_entries.empty()) {
        std::cerr << "warning: no modules found in " << input_file << "\n";
    }

    // ── Serialize ────────────────────────────────────────────────────
    std::ofstream out(output_file, std::ios::out | std::ios::binary);
    if (!out) {
        std::cerr << "error: cannot open output file: " << output_file << "\n";
        return 1;
    }

    eta::runtime::vm::BytecodeSerializer serializer(driver.heap(), driver.intern_table());
    if (!serializer.serialize(module_entries, file_registry, source_hash, include_debug, out)) {
        std::cerr << "error: failed to serialize bytecode\n";
        return 1;
    }

    std::cerr << "compiled " << input_file << " → " << output_file
              << " (" << file_registry.size() << " functions, "
              << module_entries.size() << " module(s))\n";
    return 0;
}

