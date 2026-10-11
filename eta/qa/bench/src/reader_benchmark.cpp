#include <algorithm>
#include <chrono>
#include <fstream>
#include <iostream>
#include <iterator>
#include <thread>

#include "eta/cst/bridge_sexpr.h"

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
        std::cout << '"' << name << "\":{\"median_ms\":" << median << ",\"p95_ms\":" << times[94]
                << ",\"mib_per_second\":";
        if (median > 0) std::cout << (bytes / (1024.0 * 1024.0)) / (median / 1000);
        else std::cout << "null";
        std::cout << ",\"below_timer_resolution\":" << (median == 0 ? "true" : "false") << '}';
    }

    std::size_t peak_rss() {
#ifdef _WIN32
        PROCESS_MEMORY_COUNTERS counters{};
        if (!GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof(counters))) throw std::runtime_error(
            "GetProcessMemoryInfo failed");
        return counters.PeakWorkingSetSize;
#else
        rusage usage{};
        if (getrusage(RUSAGE_SELF, &usage)) throw std::runtime_error("getrusage failed");
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
        if (argc != 2 && !probe) throw std::runtime_error("usage: eta_reader_bench [--probe] FILE");
        const auto *path = argv[probe ? 2 : 1];
        const auto io_start = Clock::now();
        std::ifstream input(path, std::ios::binary);
        if (!input) throw std::runtime_error("cannot open benchmark input");
        const std::string source(std::istreambuf_iterator<char>(input), {});
        const auto io_ms = std::chrono::duration<double, std::milli>(Clock::now() - io_start).count();
        auto parsed = eta::cst::parse(source, {});
        if (!parsed) throw std::runtime_error(parsed.error().message);
        if (probe) {
            std::cout << "{\"first_io_ms\":" << io_ms << ",\"status\":" << static_cast<int>(parsed->status) << "}\n";
            return 0;
        }
        std::cout << '{';
        measure("warm_io", source.size(), [&] {
            std::ifstream stream(path, std::ios::binary);
            if (!stream) throw std::runtime_error("cannot reopen benchmark input");
            const std::string bytes(std::istreambuf_iterator<char>(stream), {});
            if (bytes.size() != source.size()) throw std::runtime_error("benchmark input changed");
        });
        std::cout << ',';
        measure("reader", source.size(), [&] {
            auto original = eta::cst::read_original(*parsed->file);
        });
        std::cout << ',';
        measure("cst_with_validation", source.size(), [&] {
            auto result = eta::cst::parse(source, {});
            if (!result) throw std::runtime_error(result.error().message);
        });
        if (parsed->status == eta::cst::Status::Valid) {
            std::cout << ',';
            measure("bridge", source.size(), [&] {
                auto result = eta::cst::to_sexpr(*parsed);
                if (!result) throw std::runtime_error(result.error().message);
            });
        }
        std::cout << ",\"status\":" << static_cast<int>(parsed->status)
                << ",\"cst_peak_owned_and_reserved_bytes\":" << parsed->file->peak_memory_bytes();
        // Poll during a long comment; measure latency from the actual request,
        // rather than charging time before cancellation to the response.
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
            auto result = eta::cst::parse(comment, {}, operation);
            const auto completed = Clock::now();
            requester.join();
            if (result || result.error().code != "cst.Cancelled") throw std::runtime_error(
                "cancellation did not stop parsing");
            if (i >= 10) cancellation.push_back(
                std::chrono::duration<double, std::milli>(completed - requested).count());
        }
        std::sort(cancellation.begin(), cancellation.end());
        std::cout << ",\"process_peak_rss_bytes\":" << peak_rss()
                  << ",\"cancellation_p95_ms\":" << cancellation[94] << "}\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 2;
    }
}
