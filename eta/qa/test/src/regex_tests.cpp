#include <boost/test/unit_test.hpp>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include <eta/runtime/builtin_env.h>
#include <eta/runtime/core_primitives.h>
#include <eta/runtime/error.h>
#include <eta/runtime/factory.h>
#include <eta/runtime/memory/heap.h>
#include <eta/runtime/memory/intern_table.h>
#include <eta/runtime/nanbox.h>
#include <eta/runtime/numeric_value.h>
#include <eta/runtime/string_view.h>
#include <eta/runtime/types/types.h>

using namespace eta::runtime;
using namespace eta::runtime::nanbox;
using namespace eta::runtime::memory::heap;
using namespace eta::runtime::memory::intern;
using namespace eta::runtime::memory::factory;
using namespace eta::runtime::error;

namespace {

std::expected<LispVal, RuntimeError> call_builtin(
    BuiltinEnvironment& env,
    const std::string& name,
    const std::vector<LispVal>& args) {
    auto idx = env.lookup(name);
    if (!idx) {
        return std::unexpected(RuntimeError{
            VMError{RuntimeErrorCode::InternalError, "missing builtin: " + name}});
    }
    return env.specs()[*idx].func(args);
}

LispVal symbol(InternTable& intern_table, std::string_view text) {
    auto sym = make_symbol(intern_table, std::string(text));
    BOOST_REQUIRE(sym.has_value());
    return *sym;
}

LispVal stringv(Heap& heap, InternTable& intern_table, std::string_view text) {
    auto str = make_string(heap, intern_table, std::string(text));
    BOOST_REQUIRE(str.has_value());
    return *str;
}

LispVal list_from_values(Heap& heap, const std::vector<LispVal>& values) {
    LispVal out = Nil;
    for (auto it = values.rbegin(); it != values.rend(); ++it) {
        auto cell = make_cons(heap, *it, out);
        BOOST_REQUIRE(cell.has_value());
        out = *cell;
    }
    return out;
}

std::vector<LispVal> list_to_values(Heap& heap, LispVal list) {
    std::vector<LispVal> out;
    LispVal cur = list;
    while (cur != Nil) {
        BOOST_REQUIRE(ops::is_boxed(cur));
        BOOST_REQUIRE(ops::tag(cur) == Tag::HeapObject);
        auto* c = heap.try_get_as<ObjectKind::Cons, types::Cons>(ops::payload(cur));
        BOOST_REQUIRE(c != nullptr);
        out.push_back(c->car);
        cur = c->cdr;
    }
    return out;
}

std::string decode_string(InternTable& intern_table, LispVal value) {
    auto sv = StringView::try_from(value, intern_table);
    BOOST_REQUIRE(sv.has_value());
    return std::string(sv->view());
}

std::int64_t decode_int(Heap& heap, LispVal value) {
    auto n = classify_numeric(value, heap);
    BOOST_REQUIRE(n.is_valid());
    BOOST_REQUIRE(n.is_fixnum());
    return n.int_val;
}

std::optional<std::pair<std::int64_t, std::int64_t>> decode_span(Heap& heap, LispVal value) {
    if (value == False) return std::nullopt;
    BOOST_REQUIRE(ops::is_boxed(value));
    BOOST_REQUIRE(ops::tag(value) == Tag::HeapObject);
    auto* c = heap.try_get_as<ObjectKind::Cons, types::Cons>(ops::payload(value));
    BOOST_REQUIRE(c != nullptr);
    return std::pair<std::int64_t, std::int64_t>{
        decode_int(heap, c->car), decode_int(heap, c->cdr)};
}

struct DecodedMatch {
    std::int64_t start{};
    std::int64_t end{};
    std::vector<std::optional<std::pair<std::int64_t, std::int64_t>>> groups;
    std::unordered_map<std::string, std::optional<std::pair<std::int64_t, std::int64_t>>> named;
    std::string input;
};

DecodedMatch decode_match_payload(Heap& heap, InternTable& intern_table, LispVal payload) {
    BOOST_REQUIRE(ops::is_boxed(payload));
    BOOST_REQUIRE(ops::tag(payload) == Tag::HeapObject);
    auto* vec = heap.try_get_as<ObjectKind::Vector, types::Vector>(ops::payload(payload));
    BOOST_REQUIRE(vec != nullptr);
    BOOST_REQUIRE_EQUAL(vec->elements.size(), 5u);

    DecodedMatch out;
    out.start = decode_int(heap, vec->elements[0]);
    out.end = decode_int(heap, vec->elements[1]);

    auto* groups_vec = heap.try_get_as<ObjectKind::Vector, types::Vector>(ops::payload(vec->elements[2]));
    BOOST_REQUIRE(groups_vec != nullptr);
    out.groups.reserve(groups_vec->elements.size());
    for (auto v : groups_vec->elements) {
        out.groups.push_back(decode_span(heap, v));
    }

    LispVal named_list = vec->elements[3];
    for (auto row : list_to_values(heap, named_list)) {
        BOOST_REQUIRE(ops::is_boxed(row));
        BOOST_REQUIRE(ops::tag(row) == Tag::HeapObject);
        auto* pair = heap.try_get_as<ObjectKind::Cons, types::Cons>(ops::payload(row));
        BOOST_REQUIRE(pair != nullptr);

        BOOST_REQUIRE(ops::is_boxed(pair->car));
        BOOST_REQUIRE(ops::tag(pair->car) == Tag::Symbol);
        auto key = intern_table.get_string(ops::payload(pair->car));
        BOOST_REQUIRE(key.has_value());
        out.named.emplace(std::string(*key), decode_span(heap, pair->cdr));
    }

    out.input = decode_string(intern_table, vec->elements[4]);
    return out;
}

std::vector<std::string> decode_symbol_list(Heap& heap, InternTable& intern_table, LispVal list) {
    std::vector<std::string> out;
    for (auto v : list_to_values(heap, list)) {
        BOOST_REQUIRE(ops::is_boxed(v));
        BOOST_REQUIRE(ops::tag(v) == Tag::Symbol);
        auto s = intern_table.get_string(ops::payload(v));
        BOOST_REQUIRE(s.has_value());
        out.emplace_back(*s);
    }
    return out;
}

struct CacheStats {
    std::int64_t hits{};
    std::int64_t misses{};
    std::int64_t compiles{};
    std::int64_t evictions{};
    std::int64_t entries{};
};

CacheStats decode_cache_stats(Heap& heap, LispVal value) {
    BOOST_REQUIRE(ops::is_boxed(value));
    BOOST_REQUIRE(ops::tag(value) == Tag::HeapObject);
    auto* vec = heap.try_get_as<ObjectKind::Vector, types::Vector>(ops::payload(value));
    BOOST_REQUIRE(vec != nullptr);
    BOOST_REQUIRE_EQUAL(vec->elements.size(), 5u);
    return CacheStats{
        .hits = decode_int(heap, vec->elements[0]),
        .misses = decode_int(heap, vec->elements[1]),
        .compiles = decode_int(heap, vec->elements[2]),
        .evictions = decode_int(heap, vec->elements[3]),
        .entries = decode_int(heap, vec->elements[4])
    };
}

} // namespace

BOOST_AUTO_TEST_SUITE(regex_tests)

BOOST_AUTO_TEST_CASE(registers_expected_regex_builtins) {
    Heap heap(1ull << 22);
    InternTable intern_table;
    BuiltinEnvironment env;
    register_core_primitives(env, heap, intern_table, nullptr);

    BOOST_TEST(env.lookup("%regex-compile").has_value());
    BOOST_TEST(env.lookup("%regex?").has_value());
    BOOST_TEST(env.lookup("%regex-pattern").has_value());
    BOOST_TEST(env.lookup("%regex-flags").has_value());
    BOOST_TEST(env.lookup("%regex-match?").has_value());
    BOOST_TEST(env.lookup("%regex-search").has_value());
    BOOST_TEST(env.lookup("%regex-find-all").has_value());
    BOOST_TEST(env.lookup("%regex-replace").has_value());
    BOOST_TEST(env.lookup("%regex-replace-fn").has_value());
    BOOST_TEST(env.lookup("%regex-split").has_value());
    BOOST_TEST(env.lookup("%regex-quote").has_value());
    BOOST_TEST(env.lookup("%regex-match?-str").has_value());
    BOOST_TEST(env.lookup("%regex-search-str").has_value());
    BOOST_TEST(env.lookup("%regex-find-all-str").has_value());
    BOOST_TEST(env.lookup("%regex-replace-str").has_value());
    BOOST_TEST(env.lookup("%regex-split-str").has_value());
    BOOST_TEST(env.lookup("%regex-cache-stats").has_value());
    BOOST_TEST(env.lookup("%regex-cache-reset!").has_value());
}

BOOST_AUTO_TEST_CASE(regex_compile_and_introspection) {
    Heap heap(1ull << 22);
    InternTable intern_table;
    BuiltinEnvironment env;
    register_core_primitives(env, heap, intern_table, nullptr);

    LispVal flags = list_from_values(heap, {
        symbol(intern_table, "icase")
    });
    auto compiled = call_builtin(env, "%regex-compile", {
        stringv(heap, intern_table, "^foo$"),
        flags
    });
    BOOST_REQUIRE(compiled.has_value());

    auto is_regex = call_builtin(env, "%regex?", {*compiled});
    BOOST_REQUIRE(is_regex.has_value());
    BOOST_TEST(*is_regex == True);

    auto pattern = call_builtin(env, "%regex-pattern", {*compiled});
    BOOST_REQUIRE(pattern.has_value());
    BOOST_TEST(decode_string(intern_table, *pattern) == "^foo$");

    auto got_flags = call_builtin(env, "%regex-flags", {*compiled});
    BOOST_REQUIRE(got_flags.has_value());
    auto flag_names = decode_symbol_list(heap, intern_table, *got_flags);
    const bool has_ecmascript =
        std::find(flag_names.begin(), flag_names.end(), "ecmascript") != flag_names.end();
    const bool has_icase =
        std::find(flag_names.begin(), flag_names.end(), "icase") != flag_names.end();
    BOOST_TEST(has_ecmascript);
    BOOST_TEST(has_icase);
}

BOOST_AUTO_TEST_CASE(regex_match_search_and_find_all) {
    Heap heap(1ull << 22);
    InternTable intern_table;
    BuiltinEnvironment env;
    register_core_primitives(env, heap, intern_table, nullptr);

    auto re = call_builtin(env, "%regex-compile", {
        stringv(heap, intern_table, R"((?<word>foo)(\d+))"),
        Nil
    });
    BOOST_REQUIRE(re.has_value());

    auto full_true = call_builtin(env, "%regex-match?", {*re, stringv(heap, intern_table, "foo42")});
    auto full_false = call_builtin(env, "%regex-match?", {*re, stringv(heap, intern_table, "xfoo42")});
    BOOST_REQUIRE(full_true.has_value());
    BOOST_REQUIRE(full_false.has_value());
    BOOST_TEST(*full_true == True);
    BOOST_TEST(*full_false == False);

    auto search_hit = call_builtin(env, "%regex-search", {
        *re,
        stringv(heap, intern_table, "xxfoo42yy"),
        *ops::encode<int64_t>(0)
    });
    BOOST_REQUIRE(search_hit.has_value());
    BOOST_TEST(*search_hit != False);
    auto decoded = decode_match_payload(heap, intern_table, *search_hit);
    BOOST_TEST(decoded.start == 2);
    BOOST_TEST(decoded.end == 7);
    BOOST_REQUIRE_EQUAL(decoded.groups.size(), 3u);
    BOOST_REQUIRE(decoded.groups[1].has_value());
    BOOST_REQUIRE(decoded.groups[2].has_value());
    BOOST_TEST(decoded.groups[1]->first == 2);
    BOOST_TEST(decoded.groups[1]->second == 5);
    BOOST_TEST(decoded.groups[2]->first == 5);
    BOOST_TEST(decoded.groups[2]->second == 7);
    BOOST_REQUIRE(decoded.named.contains("word"));
    BOOST_REQUIRE(decoded.named["word"].has_value());
    BOOST_TEST(decoded.named["word"]->first == 2);
    BOOST_TEST(decoded.named["word"]->second == 5);

    auto search_miss = call_builtin(env, "%regex-search", {
        *re,
        stringv(heap, intern_table, "xxfoo42yy"),
        *ops::encode<int64_t>(3)
    });
    BOOST_REQUIRE(search_miss.has_value());
    BOOST_TEST(*search_miss == False);

    auto re_digits = call_builtin(env, "%regex-compile", {
        stringv(heap, intern_table, R"((\d+))"),
        Nil
    });
    BOOST_REQUIRE(re_digits.has_value());
    auto all = call_builtin(env, "%regex-find-all", {
        *re_digits,
        stringv(heap, intern_table, "a1 b22 c333")
    });
    BOOST_REQUIRE(all.has_value());
    auto rows = list_to_values(heap, *all);
    BOOST_REQUIRE_EQUAL(rows.size(), 3u);

    auto m0 = decode_match_payload(heap, intern_table, rows[0]);
    auto m1 = decode_match_payload(heap, intern_table, rows[1]);
    auto m2 = decode_match_payload(heap, intern_table, rows[2]);
    BOOST_TEST(m0.start == 1);
    BOOST_TEST(m0.end == 2);
    BOOST_TEST(m1.start == 4);
    BOOST_TEST(m1.end == 6);
    BOOST_TEST(m2.start == 8);
    BOOST_TEST(m2.end == 11);
}

BOOST_AUTO_TEST_CASE(regex_replace_replace_fn_split_and_quote) {
    Heap heap(1ull << 22);
    InternTable intern_table;
    BuiltinEnvironment env;
    register_core_primitives(env, heap, intern_table, nullptr);

    auto re_digits_named = call_builtin(env, "%regex-compile", {
        stringv(heap, intern_table, R"((?<id>\d+))"),
        Nil
    });
    BOOST_REQUIRE(re_digits_named.has_value());

    auto replaced = call_builtin(env, "%regex-replace", {
        *re_digits_named,
        stringv(heap, intern_table, "a1b22"),
        stringv(heap, intern_table, "<$<id>>")
    });
    BOOST_REQUIRE(replaced.has_value());
    BOOST_TEST(decode_string(intern_table, *replaced) == "a<1>b<22>");

    auto callback = make_primitive(
        heap,
        [&heap, &intern_table](std::span<const LispVal>) -> std::expected<LispVal, RuntimeError> {
            return make_string(heap, intern_table, "#");
        },
        1,
        false);
    BOOST_REQUIRE(callback.has_value());

    auto replaced_fn = call_builtin(env, "%regex-replace-fn", {
        *re_digits_named,
        stringv(heap, intern_table, "a1b22c"),
        *callback
    });
    BOOST_REQUIRE(replaced_fn.has_value());
    BOOST_TEST(decode_string(intern_table, *replaced_fn) == "a#b#c");

    auto comma = call_builtin(env, "%regex-compile", {
        stringv(heap, intern_table, ","),
        Nil
    });
    BOOST_REQUIRE(comma.has_value());
    auto split = call_builtin(env, "%regex-split", {
        *comma,
        stringv(heap, intern_table, ",a,,b,")
    });
    BOOST_REQUIRE(split.has_value());
    auto* split_vec = heap.try_get_as<ObjectKind::Vector, types::Vector>(ops::payload(*split));
    BOOST_REQUIRE(split_vec != nullptr);
    BOOST_REQUIRE_EQUAL(split_vec->elements.size(), 5u);
    BOOST_TEST(decode_string(intern_table, split_vec->elements[0]) == "");
    BOOST_TEST(decode_string(intern_table, split_vec->elements[1]) == "a");
    BOOST_TEST(decode_string(intern_table, split_vec->elements[2]) == "");
    BOOST_TEST(decode_string(intern_table, split_vec->elements[3]) == "b");
    BOOST_TEST(decode_string(intern_table, split_vec->elements[4]) == "");

    auto quoted = call_builtin(env, "%regex-quote", {
        stringv(heap, intern_table, ".^$|()[]{}*+?\\")
    });
    BOOST_REQUIRE(quoted.has_value());
    BOOST_TEST(decode_string(intern_table, *quoted) == "\\.\\^\\$\\|\\(\\)\\[\\]\\{\\}\\*\\+\\?\\\\");
}

BOOST_AUTO_TEST_CASE(regex_string_pattern_cache_hits_and_misses) {
    Heap heap(1ull << 22);
    InternTable intern_table;
    BuiltinEnvironment env;
    register_core_primitives(env, heap, intern_table, nullptr);

    auto reset = call_builtin(env, "%regex-cache-reset!", {});
    BOOST_REQUIRE(reset.has_value());
    BOOST_TEST(*reset == True);

    auto first = call_builtin(env, "%regex-match?-str", {
        stringv(heap, intern_table, "^foo$"),
        stringv(heap, intern_table, "foo")
    });
    BOOST_REQUIRE(first.has_value());
    BOOST_TEST(*first == True);

    auto stats_after_first = call_builtin(env, "%regex-cache-stats", {});
    BOOST_REQUIRE(stats_after_first.has_value());
    auto s1 = decode_cache_stats(heap, *stats_after_first);
    BOOST_TEST(s1.hits == 0);
    BOOST_TEST(s1.misses == 1);
    BOOST_TEST(s1.compiles == 1);
    BOOST_TEST(s1.evictions == 0);
    BOOST_TEST(s1.entries == 1);

    auto second = call_builtin(env, "%regex-match?-str", {
        stringv(heap, intern_table, "^foo$"),
        stringv(heap, intern_table, "foo")
    });
    BOOST_REQUIRE(second.has_value());
    BOOST_TEST(*second == True);

    auto stats_after_second = call_builtin(env, "%regex-cache-stats", {});
    BOOST_REQUIRE(stats_after_second.has_value());
    auto s2 = decode_cache_stats(heap, *stats_after_second);
    BOOST_TEST(s2.hits == 1);
    BOOST_TEST(s2.misses == 1);
    BOOST_TEST(s2.compiles == 1);
    BOOST_TEST(s2.evictions == 0);
    BOOST_TEST(s2.entries == 1);
}

BOOST_AUTO_TEST_CASE(regex_string_pattern_cache_eviction_is_bounded) {
    Heap heap(1ull << 22);
    InternTable intern_table;
    BuiltinEnvironment env;
    register_core_primitives(env, heap, intern_table, nullptr);

    auto reset = call_builtin(env, "%regex-cache-reset!", {});
    BOOST_REQUIRE(reset.has_value());

    constexpr std::int64_t kCapacity = 128;
    constexpr std::int64_t kTotalPatterns = 160;
    for (std::int64_t i = 0; i < kTotalPatterns; ++i) {
        const std::string n = std::to_string(i);
        auto matched = call_builtin(env, "%regex-match?-str", {
            stringv(heap, intern_table, "^id" + n + "$"),
            stringv(heap, intern_table, "id" + n)
        });
        BOOST_REQUIRE(matched.has_value());
        BOOST_TEST(*matched == True);
    }

    auto stats_value = call_builtin(env, "%regex-cache-stats", {});
    BOOST_REQUIRE(stats_value.has_value());
    auto stats = decode_cache_stats(heap, *stats_value);
    BOOST_TEST(stats.compiles == kTotalPatterns);
    BOOST_TEST(stats.misses == kTotalPatterns);
    BOOST_TEST(stats.entries == kCapacity);
    BOOST_TEST(stats.evictions == (kTotalPatterns - kCapacity));
}

BOOST_AUTO_TEST_CASE(regex_string_pattern_cache_is_thread_safe) {
    Heap setup_heap(1ull << 22);
    InternTable setup_intern;
    BuiltinEnvironment setup_env;
    register_core_primitives(setup_env, setup_heap, setup_intern, nullptr);

    auto reset = call_builtin(setup_env, "%regex-cache-reset!", {});
    BOOST_REQUIRE(reset.has_value());

    constexpr int kThreadCount = 8;
    constexpr int kUniquePatterns = 12;
    constexpr int kRounds = 8;

    std::atomic<bool> worker_ok{true};
    std::mutex error_mu;
    std::string error_message;

    std::vector<std::thread> workers;
    workers.reserve(kThreadCount);
    for (int t = 0; t < kThreadCount; ++t) {
        workers.emplace_back([&worker_ok, &error_mu, &error_message]() {
            Heap thread_heap(1ull << 22);
            InternTable thread_intern;
            BuiltinEnvironment thread_env;
            register_core_primitives(thread_env, thread_heap, thread_intern, nullptr);

            for (int round = 0; round < kRounds; ++round) {
                for (int i = 0; i < kUniquePatterns; ++i) {
                    const std::string n = std::to_string(i);
                    auto matched = call_builtin(thread_env, "%regex-match?-str", {
                        stringv(thread_heap, thread_intern, "^job" + n + "$"),
                        stringv(thread_heap, thread_intern, "job" + n)
                    });
                    if (!matched.has_value() || *matched != True) {
                        worker_ok.store(false, std::memory_order_relaxed);
                        std::lock_guard lock(error_mu);
                        error_message = "regex cache worker failed";
                        return;
                    }
                }
            }
        });
    }
    for (auto& th : workers) {
        th.join();
    }

    BOOST_TEST_CONTEXT(error_message) {
        BOOST_TEST(worker_ok.load(std::memory_order_relaxed));
    }

    auto stats_value = call_builtin(setup_env, "%regex-cache-stats", {});
    BOOST_REQUIRE(stats_value.has_value());
    auto stats = decode_cache_stats(setup_heap, *stats_value);
    BOOST_TEST(stats.compiles == kUniquePatterns);
    BOOST_TEST(stats.misses == kUniquePatterns);
    BOOST_TEST(stats.hits > 0);
    BOOST_TEST(stats.entries == kUniquePatterns);
}

BOOST_AUTO_TEST_CASE(regex_compile_error_surfaces_pattern_text) {
    Heap heap(1ull << 22);
    InternTable intern_table;
    BuiltinEnvironment env;
    register_core_primitives(env, heap, intern_table, nullptr);

    auto bad = call_builtin(env, "%regex-compile", {
        stringv(heap, intern_table, "["),
        Nil
    });
    BOOST_REQUIRE(!bad.has_value());

    auto* vm_error = std::get_if<VMError>(&bad.error());
    BOOST_REQUIRE(vm_error != nullptr);
    BOOST_TEST(static_cast<int>(vm_error->code) ==
               static_cast<int>(RuntimeErrorCode::UserError));
    BOOST_TEST(vm_error->tag_override == "runtime.regex-error");
    BOOST_TEST(vm_error->message.find("pattern") != std::string::npos);
    BOOST_TEST(vm_error->message.find("[") != std::string::npos);
}

BOOST_AUTO_TEST_SUITE_END()
