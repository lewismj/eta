#include <atomic>
#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>
#include <thread>

#include <eta/runtime/nanbox.h>
#include <eta/runtime/memory/intern_table.h>

using namespace eta::runtime::memory::intern;
using namespace eta::runtime::nanbox;

// Fuzzer limits to prevent resource exhaustion
constexpr size_t MAX_INPUT_SIZE = 64 * 1024;  // 64KB max
constexpr size_t MAX_STRINGS = 1000;          // max strings to generate
constexpr size_t MAX_STRING_LENGTH = 4096;    // max individual string length
constexpr size_t MAX_THREADS = 16;            // max concurrent threads

//! Test concurrent interning from multiple threads
void concurrent_intern_test(InternTable& table, const std::vector<std::string>& strings,
                           std::atomic<size_t>& error_count) {
    for (const auto& str : strings) {
        if (auto result = table.intern(str); !result.has_value()) {
            error_count.fetch_add(1, std::memory_order_relaxed);
        }
    }
}

//! Test that interning the same string from multiple threads returns same ID
void idempotency_test(InternTable& table, const std::string& str,
                     std::vector<InternId>& ids, size_t index) {
    if (auto result = table.intern(str); result.has_value()) {
        ids[index] = result.value();
    }
}

//! Test concurrent lookups
void concurrent_lookup_test(const InternTable& table, const std::vector<std::string>& strings,
                           std::atomic<size_t>& error_count) {
    for (const auto& str : strings) {
        auto id_result = table.get_id(str);
        if (id_result.has_value()) {
            auto str_result = table.get_string(id_result.value());
            if (!str_result.has_value() || str_result.value() != str) {
                error_count.fetch_add(1, std::memory_order_relaxed);
            }
        }
    }
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, const size_t sz) {
    //! Early rejection of invalid inputs
    if (sz < 4 || sz > MAX_INPUT_SIZE) {
        return 0;
    }

    InternTable table;
    std::atomic<size_t> error_count{0};

    //! Use first byte to determine thread count (2-8 threads)
    const size_t thread_count = std::min<size_t>((data[0] % 7) + 2, MAX_THREADS);

    //! Use second byte to determine operation mode
    const uint8_t mode = data[1] % 5;

    // Remaining data for string generation
    const uint8_t* payload = data + 2;
    const size_t payload_size = sz - 2;

    // Generate strings from fuzzer input with bounds checking
    std::vector<std::string> strings;
    strings.reserve(std::min<size_t>(100, MAX_STRINGS));  // reasonable initial capacity

    // Split input by null bytes or fixed chunks
    if (payload_size > 0) {
        size_t pos = 0;
        for (size_t i = 0; i < payload_size && strings.size() < MAX_STRINGS; ++i) {
            if (payload[i] == 0 || i == payload_size - 1) {
                size_t len = (i == payload_size - 1 && payload[i] != 0) ? i - pos + 1 : i - pos;

                // Limit string length to prevent excessive memory allocation
                len = std::min(len, MAX_STRING_LENGTH);

                if (len > 0 && strings.size() < MAX_STRINGS) {
                    strings.emplace_back(reinterpret_cast<const char*>(payload + pos), len);
                }
                pos = i + 1;
            }
        }

        // If no null bytes found, create chunks
        if (strings.empty()) {
            size_t chunk_size = std::min<size_t>(payload_size / 10, MAX_STRING_LENGTH);
            chunk_size = std::max<size_t>(1, chunk_size);

            for (size_t i = 0; i < payload_size && strings.size() < MAX_STRINGS; i += chunk_size) {
                size_t len = std::min({chunk_size, payload_size - i, MAX_STRING_LENGTH});
                if (len > 0) {
                    strings.emplace_back(reinterpret_cast<const char*>(payload + i), len);
                }
            }
        }
    }

    // Add some edge case strings (with total count check)
    if (strings.size() < MAX_STRINGS) strings.push_back("");  // Empty string
    if (strings.size() < MAX_STRINGS) strings.push_back("a"); // Single char
    if (strings.size() < MAX_STRINGS) strings.push_back(std::string(100, 'x')); // Long string
    if (strings.size() < MAX_STRINGS) strings.push_back(std::string(1, '\0')); // Null byte

    if (strings.empty()) {
        return 0;
    }

    std::vector<std::thread> threads;
    threads.reserve(thread_count);

    switch (mode) {
        case 0: {
            // Mode 0: Concurrent interning of different strings
            const size_t strings_per_thread = std::max<size_t>(1, strings.size() / thread_count);

            for (size_t t = 0; t < thread_count; ++t) {
                size_t start = t * strings_per_thread;
                size_t end = (t == thread_count - 1) ? strings.size() : start + strings_per_thread;

                // Bounds check
                if (start >= strings.size()) {
                    break;
                }
                end = std::min(end, strings.size());

                // Create subvector safely
                std::vector<std::string> thread_strings;
                thread_strings.reserve(end - start);
                for (size_t i = start; i < end; ++i) {
                    thread_strings.push_back(strings[i]);
                }

                threads.emplace_back(concurrent_intern_test, std::ref(table),
                                    std::move(thread_strings), std::ref(error_count));
            }
            break;
        }

        case 1: {
            // Mode 1: Idempotency test - all threads intern the same strings
            for (size_t t = 0; t < thread_count; ++t) {
                threads.emplace_back(concurrent_intern_test, std::ref(table),
                                    std::cref(strings), std::ref(error_count));
            }
            break;
        }

        case 2: {
            // Mode 2: Verify same string returns same ID from multiple threads
            if (!strings.empty()) {
                std::vector<InternId> ids(thread_count, 0);
                for (size_t t = 0; t < thread_count; ++t) {
                    threads.emplace_back(idempotency_test, std::ref(table),
                                        std::cref(strings[0]), std::ref(ids), t);
                }

                for (auto& thread : threads) {
                    thread.join();
                }
                threads.clear();

                // Verify all IDs are the same (non-zero)
                if (ids[0] != 0) {
                    for (size_t i = 1; i < ids.size(); ++i) {
                        assert(ids[i] == ids[0]);
                    }
                }
            }
            break;
        }

        case 3: {
            // Mode 3: Intern then lookup concurrently
            // First, intern all strings
            for (const auto& str : strings) {
                table.intern(str);
            }

            // Then lookup from multiple threads
            for (size_t t = 0; t < thread_count; ++t) {
                threads.emplace_back(concurrent_lookup_test, std::ref(table),
                                    std::cref(strings), std::ref(error_count));
            }
            break;
        }

        case 4: {
            // Mode 4: Mixed operations - some threads intern, some lookup.
            // Intern half the strings first.
            for (size_t i = 0; i < strings.size() / 2; ++i) {
                table.intern(strings[i]);
            }

            for (size_t t = 0; t < thread_count; ++t) {
                if (t % 2 == 0) {
                    threads.emplace_back(concurrent_intern_test, std::ref(table),
                                        std::cref(strings), std::ref(error_count));
                } else {
                    threads.emplace_back(concurrent_lookup_test, std::ref(table),
                                        std::cref(strings), std::ref(error_count));
                }
            }
            break;
        }
        default:
            break;
    }

    // Wait for all threads to complete.
    for (auto& thread : threads) {
        if (thread.joinable()) {
            thread.join();
        }
    }

    // Verify integrity after concurrent operations.
    for (const auto& str : strings) {
        auto id_result = table.get_id(str);
        if (id_result.has_value()) {
            auto str_result = table.get_string(id_result.value());
            // Verify bidirectional consistency.
            assert(str_result.has_value());
            assert(str_result.value() == str);
        }
    }

    return 0;
}