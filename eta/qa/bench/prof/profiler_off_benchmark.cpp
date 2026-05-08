#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>

#include "eta/reader/expander.h"
#include "eta/reader/lexer.h"
#include "eta/reader/module_linker.h"
#include "eta/reader/parser.h"
#include "eta/runtime/builtin_env.h"
#include "eta/runtime/core_primitives.h"
#include "eta/runtime/nanbox.h"
#include "eta/runtime/prof/profiler.h"
#include "eta/runtime/vm/vm.h"
#include "eta/semantics/emitter.h"
#include "eta/semantics/semantic_analyzer.h"

namespace {

using eta::runtime::nanbox::LispVal;
using eta::runtime::prof::runtime_profiler;
using eta::runtime::vm::BytecodeFunction;
using eta::runtime::vm::VM;
using eta::semantics::BytecodeFunctionRegistry;
using eta::semantics::Emitter;
using eta::semantics::SemanticAnalyzer;

struct BenchConfig {
    std::size_t iterations{10};
    std::size_t loops{50000};
    bool profiler_enabled{false};
    double gate_max_ms{-1.0};
};

struct Program {
    eta::runtime::memory::heap::Heap heap{4 * 1024 * 1024};
    eta::runtime::memory::intern::InternTable intern_table;
    BytecodeFunctionRegistry registry;
    eta::runtime::BuiltinEnvironment builtins;
    BytecodeFunction* main_func{nullptr};
    std::uint32_t total_globals{0};
};

[[nodiscard]] BenchConfig parse_args(int argc, char** argv) {
    BenchConfig cfg;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg(argv[i]);
        if (arg == "--help" || arg == "-h") {
            std::cout
                << "Usage: eta_prof_off_bench [--iterations N] [--loops N] [--enable-profiler]"
                   " [--gate-max-ms X]\n";
            std::exit(0);
        }
        if (arg == "--iterations") {
            if (i + 1 >= argc) throw std::invalid_argument("--iterations requires a value");
            cfg.iterations = static_cast<std::size_t>(std::stoull(argv[++i]));
            continue;
        }
        if (arg == "--loops") {
            if (i + 1 >= argc) throw std::invalid_argument("--loops requires a value");
            cfg.loops = static_cast<std::size_t>(std::stoull(argv[++i]));
            continue;
        }
        if (arg == "--enable-profiler") {
            cfg.profiler_enabled = true;
            continue;
        }
        if (arg == "--gate-max-ms") {
            if (i + 1 >= argc) throw std::invalid_argument("--gate-max-ms requires a value");
            cfg.gate_max_ms = std::stod(argv[++i]);
            continue;
        }
        throw std::invalid_argument("unknown argument: " + std::string(arg));
    }
    if (cfg.iterations == 0) throw std::invalid_argument("--iterations must be >= 1");
    if (cfg.loops == 0) throw std::invalid_argument("--loops must be >= 1");
    return cfg;
}

[[nodiscard]] std::string build_benchmark_source(const std::size_t loops) {
    return "(module profiler.bench "
           "  (define (pred n) (= n 0)) "
           "  (define (loop n acc) "
           "    (if (pred n) acc (loop (- n 1) (+ acc 1)))) "
           "  (define result (loop "
           + std::to_string(loops) + " 0)))";
}

void compile_program(Program& program, const std::string& source) {
    using namespace eta::reader;

    lexer::Lexer lex(1, source);
    parser::Parser parser(lex);
    auto parsed = parser.parse_toplevel();
    if (!parsed) throw std::runtime_error("parse failed");

    expander::Expander expander;
    auto expanded = expander.expand_many(*parsed);
    if (!expanded) throw std::runtime_error("expand failed: " + expanded.error().message);

    ModuleLinker linker;
    auto index_res = linker.index_modules(*expanded);
    if (!index_res) throw std::runtime_error("index failed: " + index_res.error().message);
    auto link_res = linker.link();
    if (!link_res) throw std::runtime_error("link failed: " + link_res.error().message);

    SemanticAnalyzer analyzer;
    auto sem = analyzer.analyze_all(*expanded, linker, program.builtins);
    if (!sem) throw std::runtime_error("semantic failed: " + sem.error().message);
    if (sem->empty()) throw std::runtime_error("no module produced");

    auto& mod = sem->front();
    program.total_globals = mod.total_globals;
    Emitter emitter(mod, program.heap, program.intern_table, program.registry);
    program.main_func = emitter.emit();
    if (!program.main_func) throw std::runtime_error("emit failed");
}

[[nodiscard]] double run_benchmark(Program& program,
                                   const BenchConfig& cfg,
                                   LispVal* last_result) {
    using clock = std::chrono::steady_clock;

    VM vm(program.heap, program.intern_table);
    vm.set_function_resolver([&program](std::uint32_t idx) { return program.registry.get(idx); });

    auto install = program.builtins.install(program.heap, vm.globals(), program.total_globals);
    if (!install) throw std::runtime_error("builtin install failed");

    runtime_profiler().set_enabled(cfg.profiler_enabled);
    runtime_profiler().reset_for_test();

    auto warmup = vm.execute(*program.main_func);
    if (!warmup) throw std::runtime_error("warmup execute failed");

    const auto begin = clock::now();
    LispVal result = *warmup;
    for (std::size_t i = 0; i < cfg.iterations; ++i) {
        auto run = vm.execute(*program.main_func);
        if (!run) throw std::runtime_error("execute failed");
        result = *run;
    }
    const auto end = clock::now();

    runtime_profiler().set_enabled(false);
    if (last_result) *last_result = result;

    const auto elapsed_ms = std::chrono::duration<double, std::milli>(end - begin).count();
    return elapsed_ms / static_cast<double>(cfg.iterations);
}

} // namespace

int main(int argc, char** argv) {
    BenchConfig cfg;
    try {
        cfg = parse_args(argc, argv);
    } catch (const std::exception& ex) {
        std::cerr << "eta_prof_off_bench: " << ex.what() << "\n";
        return 2;
    }

    Program program;
    eta::runtime::register_core_primitives(program.builtins, program.heap, program.intern_table);

    try {
        compile_program(program, build_benchmark_source(cfg.loops));
    } catch (const std::exception& ex) {
        std::cerr << "eta_prof_off_bench: compile error: " << ex.what() << "\n";
        return 1;
    }

    LispVal result = eta::runtime::nanbox::Nil;
    double avg_ms = 0.0;
    try {
        avg_ms = run_benchmark(program, cfg, &result);
    } catch (const std::exception& ex) {
        std::cerr << "eta_prof_off_bench: run error: " << ex.what() << "\n";
        return 1;
    }

    std::cout << "mode=" << (cfg.profiler_enabled ? "on" : "off")
              << " loops=" << cfg.loops
              << " iterations=" << cfg.iterations
              << " avg_ms=" << avg_ms << "\n";

    if (cfg.gate_max_ms >= 0.0 && avg_ms > cfg.gate_max_ms) {
        std::cerr << "eta_prof_off_bench: gate failed (avg_ms=" << avg_ms
                  << " > gate_max_ms=" << cfg.gate_max_ms << ")\n";
        return 1;
    }

    return 0;
}
