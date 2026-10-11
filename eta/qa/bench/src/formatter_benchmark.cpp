#include <algorithm>
#include <chrono>
#include <fstream>
#include <iostream>
#include <thread>

#include "eta/format/format_source.h"

#ifdef _WIN32
#include <windows.h>
#include <psapi.h>
#else
#include <sys/resource.h>
#endif

namespace {
    using Clock = std::chrono::steady_clock;
    template<class Function>
    void measure(const char *name, std::size_t bytes, Function function) {
        for (int i = 0; i < 10; ++i) function();
        std::vector<double> times;
        for (int i = 0; i < 100; ++i) {
            const auto start = Clock::now();
            function();
            times.push_back(std::chrono::duration<double, std::milli>(Clock::now() - start).count());
        }
        std::sort(times.begin(), times.end());
        const auto median = (times[49] + times[50]) / 2;
        std::cout << '"' << name << "\":{\"median_ms\":" << median << ",\"p95_ms\":" << times[94] << ",\"mib_per_second\":";
        if (median > 0) std::cout << (bytes / (1024.0 * 1024.0)) / (median / 1000);
        else std::cout << "null";
        std::cout << ",\"below_timer_resolution\":" << (median == 0 ? "true" : "false") << '}';
    }

    std::size_t peak_rss() {
#ifdef _WIN32
        PROCESS_MEMORY_COUNTERS counters{};
        if (!GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof(counters))) throw std::runtime_error("cannot obtain peak RSS");
        return counters.PeakWorkingSetSize;
#else
        rusage usage{};
        if (getrusage(RUSAGE_SELF, &usage)) throw std::runtime_error("cannot obtain peak RSS");
#ifdef __APPLE__
        return usage.ru_maxrss;
#else
        return usage.ru_maxrss * 1024;
#endif
#endif
    }
}

int main(int argc, char **argv) {
    try {
        const bool probe = argc == 3 && std::string_view(argv[1]) == "--probe";
        if (argc != 2 && !probe) throw std::runtime_error("usage: eta_formatter_bench [--probe] FILE");
        const auto start = Clock::now();
        std::ifstream input(argv[probe ? 2 : 1], std::ios::binary);
        if (!input) throw std::runtime_error("cannot open benchmark input");
        const std::string source(std::istreambuf_iterator<char>(input), {});
        const auto io_ms = std::chrono::duration<double, std::milli>(Clock::now() - start).count();
        auto formatted = eta::format::format_source(source);
        if (!formatted.output) throw std::runtime_error(formatted.diagnostics.back().message);
        if (probe) { std::cout << "{\"first_io_ms\":" << io_ms << "}\n"; return 0; }
        std::cout << '{';
        measure("cst_with_validation", source.size(), [&] {
            if (!eta::cst::parse(source, {})) throw std::runtime_error("CST benchmark failed");
        });
        std::cout << ',';
        std::vector<double> parsing, layout, validation;
        measure("format_with_validation", source.size(), [&] {
            const auto result = eta::format::format_source(source);
            if (!result.output) throw std::runtime_error("formatter benchmark failed");
            parsing.push_back(result.timings.parse_ms);
            layout.push_back(result.timings.layout_render_ms);
            validation.push_back(result.timings.output_validation_ms);
        });
        auto breakdown = [](const char *name, std::vector<double> times) {
            times.erase(times.begin(), times.begin() + 10);
            std::sort(times.begin(), times.end());
            std::cout << ",\"" << name << "\":{\"median_ms\":" << (times[49] + times[50]) / 2 << ",\"p95_ms\":" << times[94] << '}';
        };
        breakdown("parse", parsing);
        breakdown("layout_render", layout);
        breakdown("output_validation", validation);
        std::vector<double> cancellation;
        for (int i = 0; i < 110; ++i) {
            const std::string comment = ";" + std::string(16 * 1024 * 1024 - 1, 'x');
            auto cancelled = std::make_shared<std::atomic_bool>(false);
            eta::cst::Operation operation;
            operation.cancelled = cancelled;
            Clock::time_point requested;
            std::thread requester([&] {
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
                requested = Clock::now();
                cancelled->store(true, std::memory_order_release);
            });
            const auto result = eta::format::format_source(comment, {}, operation);
            const auto completed = Clock::now();
            requester.join();
            if (result.output || result.exit_class != 2) throw std::runtime_error("cancellation did not stop formatting");
            if (i >= 10) cancellation.push_back(std::chrono::duration<double, std::milli>(completed - requested).count());
        }
        std::sort(cancellation.begin(), cancellation.end());
        std::cout << ",\"format_peak_owned_and_reserved_bytes\":" << formatted.peak_owned_memory_bytes
                  << ",\"passthrough_regions\":" << formatted.passthrough.size()
                  << ",\"process_peak_rss_bytes\":" << peak_rss()
                  << ",\"cancellation_p95_ms\":" << cancellation[94] << "}\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 2;
    }
}
