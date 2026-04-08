#include <barrier>
#include <cmath>
#include <expected>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <boost/test/unit_test.hpp>

#include <eta/runtime/nanbox.h>
#include <eta/runtime/memory/intern_table.h>

using namespace eta::runtime;
using namespace eta::runtime::memory::intern;


// Helper: unwrap std::expected in tests; on error, print the error enum value
static InternId expect_ok_id(const std::expected<InternId, InternTableError>& r) {
    BOOST_REQUIRE(r.has_value());
    return *r;
}

static std::string_view expect_ok_sview(const std::expected<std::string_view, InternTableError>& r) {
    BOOST_REQUIRE(r.has_value());
    return *r;
}

BOOST_AUTO_TEST_SUITE(intern_table_tests)

BOOST_AUTO_TEST_CASE(test_fuzzer_crash_case_newlines) {
    // Reproduce the fuzzer crash: input "u\n\n\n" (0x75, 0x0a, 0x0a, 0x0a)
    constexpr uint8_t crash_data[] = {0x75, 0x0a, 0x0a, 0x0a};
    constexpr size_t crash_size = sizeof(crash_data);

    const std::string_view input(reinterpret_cast<const char*>(crash_data), crash_size);

    // This should not crash - test basic handling
    BOOST_TEST_MESSAGE("Testing crash input: u\\n\\n\\n");

    InternTable tbl;

    // - "u"
    // - "" (empty)
    // - "" (empty)
    // - "" (empty)

    // Test 1: Intern the whole string
    BOOST_CHECK_NO_THROW({
        (void) tbl.intern(input);
        BOOST_TEST_MESSAGE("Full string interned successfully");
    });

    // Test 2: Intern individual parts (simulating what fuzzer might do).
    std::vector<std::string_view> parts;
    size_t start = 0;
    for (size_t i = 0; i < crash_size; ++i) {
        if (crash_data[i] == '\n') {
            parts.emplace_back(reinterpret_cast<const char*>(&crash_data[start]), i - start);
            start = i + 1;
        }
    }
    // Add the remaining part.
    if (start < crash_size) {
        parts.emplace_back(reinterpret_cast<const char*>(&crash_data[start]), crash_size - start);
    }

    // Test each part.
    for (const auto& part : parts) {
        BOOST_CHECK_NO_THROW({
            (void) tbl.intern(part);
            BOOST_TEST_MESSAGE("Part interned: length=" << part.size());
        });
    }
}

BOOST_AUTO_TEST_CASE(test_fuzzer_crash_case_single_char_with_newlines) {
    InternTable tbl;

    // Test a single character followed by newlines
    BOOST_CHECK_NO_THROW({
        (void) tbl.intern("u");
        (void) tbl.intern("u\n");
        (void) tbl.intern("u\n\n");
        (void) tbl.intern("u\n\n\n");
    });
}

BOOST_AUTO_TEST_CASE(test_fuzzer_crash_case_empty_strings) {
    InternTable tbl;

    BOOST_CHECK_NO_THROW({
        (void) tbl.intern("");
        (void) tbl.intern("");
        (void) tbl.intern("");
    });
}

BOOST_AUTO_TEST_CASE(test_fuzzer_crash_case_only_newlines) {
    InternTable tbl;

    BOOST_CHECK_NO_THROW({
        (void) tbl.intern("\n");
        (void) tbl.intern("\n\n");
        (void) tbl.intern("\n\n\n");
    });
}


BOOST_AUTO_TEST_CASE(intern_basic_roundtrip) {
    InternTable tbl;

    auto id_hello = expect_ok_id(tbl.intern("hello"));
    BOOST_TEST(id_hello <= nanbox::constants::PAYLOAD_MASK);

    auto id_world = expect_ok_id(tbl.intern("world"));
    BOOST_TEST(id_world <= nanbox::constants::PAYLOAD_MASK);

    // Different strings have different ids (in single-threaded scenario)
    BOOST_TEST(id_hello != id_world);

    // Same string returns the same id
    auto id_hello_again = expect_ok_id(tbl.intern("hello"));
    BOOST_TEST(id_hello_again == id_hello);

    // Lookup id by string
    auto id_from_lookup = expect_ok_id(tbl.get_id("hello"));
    BOOST_TEST(id_from_lookup == id_hello);

    // Lookup string by id
    auto sv_hello = expect_ok_sview(tbl.get_string(id_hello));
    BOOST_TEST(sv_hello == std::string_view{"hello"});
}

BOOST_AUTO_TEST_CASE(missing_entries_return_errors) {
    InternTable tbl;

    // Missing id for a never-interned string
    auto no_id = tbl.get_id("missing");
    BOOST_TEST(!no_id.has_value());
    BOOST_TEST(no_id.error() == InternTableError::MissingId);

    // Missing string for an arbitrary id
    auto no_str = tbl.get_string(123456789ull);
    BOOST_TEST(!no_str.has_value());
    BOOST_TEST(no_str.error() == InternTableError::MissingString);
}

BOOST_AUTO_TEST_CASE(empty_and_unicode_strings) {
    InternTable tbl;

    // Empty string
    auto id_empty = expect_ok_id(tbl.intern(""));
    BOOST_TEST(id_empty <= nanbox::constants::PAYLOAD_MASK);
    BOOST_TEST(expect_ok_sview(tbl.get_string(id_empty)) == std::string_view{""});
    BOOST_TEST(expect_ok_id(tbl.get_id("")) == id_empty);

    // UTF-8 content
    const std::string sushi = "寿司🍣";  // UTF-8 in char, not char8_t
    auto id_utf8 = expect_ok_id(tbl.intern(sushi));
    BOOST_TEST(expect_ok_sview(tbl.get_string(id_utf8)) == std::string_view{sushi});
    BOOST_TEST(expect_ok_id(tbl.get_id(sushi)) == id_utf8);

    // Long string
    std::string long_str(20000, 'x');
    auto id_long = expect_ok_id(tbl.intern(long_str));
    BOOST_TEST(expect_ok_sview(tbl.get_string(id_long)).size() == long_str.size());
    BOOST_TEST(expect_ok_sview(tbl.get_string(id_long)) == std::string_view{long_str});
}

BOOST_AUTO_TEST_CASE(interned_string_lifetime_is_immortal) {
    InternTable tbl;

    // Create a temporary that will go out of scope
    InternId id_tmp = 0;
    {
        const std::string tmp = std::string("ephemeral-") + std::to_string(42);
        id_tmp = expect_ok_id(tbl.intern(tmp));
        // tmp goes out of scope here
    }

    // The interned string must remain accessible
    auto sv = expect_ok_sview(tbl.get_string(id_tmp));
    BOOST_TEST(sv == std::string_view{"ephemeral-42"});

    // Also lookup by string still works
    BOOST_TEST(expect_ok_id(tbl.get_id("ephemeral-42")) == id_tmp);
}

BOOST_AUTO_TEST_CASE(concurrency_same_string_all_get_same_id) {
    InternTable tbl;

    constexpr int threads = 8;
    constexpr int per_thread = 1000;

    std::vector<std::thread> ts;
    ts.reserve(threads);
    std::barrier sync_point(threads);

    std::vector<InternId> ids(threads);
    // Boost.Test macros are NOT thread-safe; collect errors for main-thread validation.
    std::atomic<bool> any_intern_failed{false};
    std::atomic<bool> any_id_mismatch{false};

    for (int t = 0; t < threads; ++t) {
        ts.emplace_back([&, t] {
            sync_point.arrive_and_wait();
            InternId last = 0;
            for (int i = 0; i < per_thread; ++i) {
                auto r = tbl.intern("concurrent-key");
                if (!r.has_value()) {
                    any_intern_failed.store(true, std::memory_order_relaxed);
                    return;
                }
                auto id = *r;
                if (i == 0) last = id;
                else if (id != last) {
                    any_id_mismatch.store(true, std::memory_order_relaxed);
                }
            }
            ids[t] = last;
        });
    }
    for (auto& th : ts) th.join();

    BOOST_TEST(!any_intern_failed.load());
    BOOST_TEST(!any_id_mismatch.load());

    // All threads must observe the same id
    for (int t = 1; t < threads; ++t) {
        BOOST_TEST(ids[t] == ids[0]);
    }

    // And the string roundtrips
    BOOST_TEST(expect_ok_sview(tbl.get_string(ids[0])) == std::string_view{"concurrent-key"});
}

BOOST_AUTO_TEST_CASE(concurrency_many_unique_strings_consistent) {
    InternTable tbl;

    constexpr int threads = 8;
    constexpr int uniques_per_thread = 200;

    std::vector<std::thread> ts;
    ts.reserve(threads);
    std::barrier sync_point(threads);

    // Boost.Test macros are NOT thread-safe; collect results for main-thread validation.
    std::mutex mtx;
    std::unordered_map<std::string, InternId> global;
    std::atomic<bool> any_intern_failed{false};
    std::atomic<bool> any_roundtrip_failed{false};
    std::atomic<bool> any_consistency_failed{false};

    for (int t = 0; t < threads; ++t) {
        ts.emplace_back([&, t] {
            std::vector<std::pair<std::string, InternId>> local;
            local.reserve(uniques_per_thread);

            sync_point.arrive_and_wait();

            for (int i = 0; i < uniques_per_thread; ++i) {
                std::string s = "k:" + std::to_string(t) + ":" + std::to_string(i);
                auto r = tbl.intern(s);
                if (!r.has_value()) {
                    any_intern_failed.store(true, std::memory_order_relaxed);
                    return;
                }
                auto id = *r;

                // Round-trip checks (no Boost macros — not thread-safe)
                auto r_id = tbl.get_id(s);
                auto r_sv = tbl.get_string(id);
                if (!r_id.has_value() || *r_id != id ||
                    !r_sv.has_value() || *r_sv != std::string_view{s}) {
                    any_roundtrip_failed.store(true, std::memory_order_relaxed);
                }

                local.emplace_back(std::move(s), id);
            }

            // Merge into global map, checking consistency without Boost macros
            std::scoped_lock lk(mtx);
            for (auto& [s, id] : local) {
                if (auto it = global.find(s); it == global.end()) {
                    global.emplace(s, id);
                } else if (it->second != id) {
                    any_consistency_failed.store(true, std::memory_order_relaxed);
                }
            }
        });
    }

    for (auto& th : ts) th.join();

    BOOST_TEST(!any_intern_failed.load());
    BOOST_TEST(!any_roundtrip_failed.load());
    BOOST_TEST(!any_consistency_failed.load());

    // Spot-check a few values exist and roundtrip after all threads complete
    for (int t = 0; t < threads; ++t) {
        for (int i = 0; i < 3; ++i) {
            std::string s = "k:" + std::to_string(t) + ":" + std::to_string(i);
            auto id = expect_ok_id(tbl.get_id(s));
            BOOST_TEST(expect_ok_sview(tbl.get_string(id)) == std::string_view{s});
        }
    }
}

BOOST_AUTO_TEST_SUITE_END()