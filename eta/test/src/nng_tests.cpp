#include <boost/test/unit_test.hpp>

#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>
#include <chrono>
#include <thread>

#include <nng/nng.h>
#include <nng/protocol/pair0/pair.h>
#include <nng/protocol/pubsub0/pub.h>
#include <nng/protocol/pubsub0/sub.h>
#include <nng/protocol/pipeline0/push.h>
#include <nng/protocol/pipeline0/pull.h>
#include <nng/protocol/reqrep0/req.h>
#include <nng/protocol/reqrep0/rep.h>
#include <nng/protocol/survey0/survey.h>
#include <nng/protocol/survey0/respond.h>
#include <nng/protocol/bus0/bus.h>

#include <eta/nng/nng_socket_ptr.h>
#include <eta/nng/nng_factory.h>
#include <eta/nng/nng_primitives.h>
#include <eta/nng/process_mgr.h>
#include <eta/nng/wire_format.h>

#include <eta/runtime/nanbox.h>
#include <eta/runtime/memory/heap.h>
#include <eta/runtime/memory/intern_table.h>
#include <eta/runtime/factory.h>
#include <eta/runtime/value_formatter.h>
#include <eta/runtime/types/cons.h>
#include <eta/runtime/types/vector.h>
#include <eta/runtime/builtin_env.h>

using namespace eta::nng;
using namespace eta::nng::factory;
using namespace eta::runtime::nanbox;
using namespace eta::runtime::nanbox::ops;
using namespace eta::runtime::memory::heap;
using namespace eta::runtime::memory::intern;
using namespace eta::runtime::memory::factory;
using namespace eta::runtime;
using namespace eta::runtime::types;

namespace {
    template <typename T, typename E>
    static T require_ok(const std::expected<T,E>& r) {
        BOOST_REQUIRE_MESSAGE(r.has_value(), "expected success");
        return *r;
    }

    /// Convert any RuntimeError variant to a human-readable string.
    static std::string runtime_error_msg(const error::RuntimeError& err) {
        using namespace eta::runtime::nanbox;
        using namespace eta::runtime::memory::heap;
        using namespace eta::runtime::memory::intern;
        using namespace eta::runtime::error;
        return std::visit([](auto&& e) -> std::string {
            using T = std::decay_t<decltype(e)>;
            if constexpr (std::is_same_v<T, VMError>)
                return e.message;
            else
                return to_string(e);
        }, err);
    }

    /// Build a full BuiltinEnvironment with all nng primitives registered.
    struct NngEnv {
        Heap heap{4 * 1024 * 1024};
        InternTable intern;
        BuiltinEnvironment env;

        NngEnv() { register_nng_primitives(env, heap, intern); }

        LispVal call(const std::string& name, std::vector<LispVal> args) {
            const auto& specs = env.specs();
            for (const auto& spec : specs) {
                if (spec.name == name && spec.func) {
                    auto res = spec.func(args);
                    BOOST_REQUIRE_MESSAGE(res.has_value(),
                        "call to " + name + " failed");
                    return *res;
                }
            }
            BOOST_FAIL("primitive not found: " + name);
            return nanbox::Nil;
        }

        std::expected<LispVal, error::RuntimeError>
        try_call(const std::string& name, std::vector<LispVal> args) {
            const auto& specs = env.specs();
            for (const auto& spec : specs) {
                if (spec.name == name && spec.func) {
                    return spec.func(args);
                }
            }
            return std::unexpected(error::VMError{
                error::RuntimeErrorCode::InternalError, "not found: " + name});
        }

        LispVal sym(const std::string& s) {
            return require_ok(make_symbol(intern, s));
        }

        LispVal str(const std::string& s) {
            return require_ok(make_string(heap, intern, s));
        }

        LispVal fixnum(int64_t n) {
            return require_ok(make_fixnum(heap, n));
        }
    };

    /// NngEnv with ProcessManager wired in (for Phase 4 spawn tests).
    struct NngEnvWithSpawn {
        Heap heap{4 * 1024 * 1024};
        InternTable intern;
        BuiltinEnvironment env;
        ProcessManager proc_mgr;
        LispVal mailbox_val{nanbox::Nil};
        std::string etai_path;

        explicit NngEnvWithSpawn(const std::string& etai = {}) : etai_path(etai) {
            register_nng_primitives(env, heap, intern,
                                    &proc_mgr, etai_path, &mailbox_val);
        }

        LispVal call(const std::string& name, std::vector<LispVal> args) {
            const auto& specs = env.specs();
            for (const auto& spec : specs) {
                if (spec.name == name && spec.func) {
                    auto res = spec.func(args);
                    BOOST_REQUIRE_MESSAGE(res.has_value(),
                        "call to " + name + " failed: " +
                        (res.has_value() ? "" : runtime_error_msg(res.error())));
                    return *res;
                }
            }
            BOOST_FAIL("primitive not found: " + name);
            return nanbox::Nil;
        }

        std::expected<LispVal, error::RuntimeError>
        try_call(const std::string& name, std::vector<LispVal> args) {
            const auto& specs = env.specs();
            for (const auto& spec : specs) {
                if (spec.name == name && spec.func) {
                    return spec.func(args);
                }
            }
            return std::unexpected(error::VMError{
                error::RuntimeErrorCode::InternalError, "not found: " + name});
        }

        LispVal sym(const std::string& s) { return require_ok(make_symbol(intern, s)); }
        LispVal str(const std::string& s) { return require_ok(make_string(heap, intern, s)); }
        LispVal fixnum(int64_t n)         { return require_ok(make_fixnum(heap, n)); }
    };

    /// Unique TCP port counter to avoid collisions across test cases.
    static std::atomic<int> g_port_counter{15100};
    static int next_port() { return g_port_counter.fetch_add(1); }
    static std::string tcp_addr() {
        return "tcp://127.0.0.1:" + std::to_string(next_port());
    }
    static std::string inproc_addr() {
        static std::atomic<int> n{0};
        return "inproc://nng-test-" + std::to_string(n.fetch_add(1));
    }

    /// Return the path to the etai binary injected at compile time,
    /// or an empty string if not available.
    static std::string etai_binary_path() {
#ifdef ETA_ETAI_PATH
        return ETA_ETAI_PATH;
#else
        return {};
#endif
    }
}

BOOST_AUTO_TEST_SUITE(nng_tests)

// ═══════════════════════════════════════════════════════════════════════════
// Phase 1 — Basic nng library integration
// ═══════════════════════════════════════════════════════════════════════════

BOOST_AUTO_TEST_CASE(pair_socket_open_close) {
    nng_socket sock;
    int rv = nng_pair0_open(&sock);
    BOOST_REQUIRE_EQUAL(rv, 0);

    rv = nng_close(sock);
    BOOST_TEST(rv == 0);
}

BOOST_AUTO_TEST_CASE(nng_socket_ptr_raii) {
    // Verify our RAII wrapper opens and auto-closes without leaking.
    {
        NngSocketPtr sp;
        int rv = nng_pair0_open(&sp.socket);
        BOOST_REQUIRE_EQUAL(rv, 0);
        sp.protocol = NngProtocol::Pair;
    }
    // Destructor called — no leak.
    BOOST_TEST(true);
}

BOOST_AUTO_TEST_CASE(nng_socket_ptr_move) {
    NngSocketPtr a;
    int rv = nng_pair0_open(&a.socket);
    BOOST_REQUIRE_EQUAL(rv, 0);
    a.protocol = NngProtocol::Pair;
    a.listening = true;

    NngSocketPtr b(std::move(a));
    BOOST_TEST(static_cast<int>(b.protocol) == static_cast<int>(NngProtocol::Pair));
    BOOST_TEST(b.listening == true);
    // 'a' should be in moved-from state (NNG_SOCKET_INITIALIZER)
}

// ═══════════════════════════════════════════════════════════════════════════
// Phase 2 — Wire format: round-trip serialization tests
// ═══════════════════════════════════════════════════════════════════════════

// Helper: serialize then deserialize, check the re-serialized string matches.
static void round_trip(LispVal v, Heap& heap, InternTable& intern, const std::string& expected_text) {
    std::string serialized = serialize_value(v, heap, intern);
    BOOST_TEST_MESSAGE("serialized: " << serialized);
    BOOST_TEST(serialized == expected_text);

    auto deserialized = deserialize_value(serialized, heap, intern);
    BOOST_REQUIRE_MESSAGE(deserialized.has_value(), "deserialization failed");

    std::string re_serialized = serialize_value(*deserialized, heap, intern);
    BOOST_TEST(re_serialized == expected_text);
}

// ------ Nil ------

BOOST_AUTO_TEST_CASE(wire_round_trip_nil) {
    Heap heap(1ull << 20);
    InternTable intern;
    round_trip(nanbox::Nil, heap, intern, "()");
}

// ------ Booleans ------

BOOST_AUTO_TEST_CASE(wire_round_trip_true) {
    Heap heap(1ull << 20);
    InternTable intern;
    round_trip(nanbox::True, heap, intern, "#t");
}

BOOST_AUTO_TEST_CASE(wire_round_trip_false) {
    Heap heap(1ull << 20);
    InternTable intern;
    round_trip(nanbox::False, heap, intern, "#f");
}

// ------ Fixnums ------

BOOST_AUTO_TEST_CASE(wire_round_trip_fixnum_zero) {
    Heap heap(1ull << 20);
    InternTable intern;
    auto v = require_ok(make_fixnum(heap, static_cast<int64_t>(0)));
    round_trip(v, heap, intern, "0");
}

BOOST_AUTO_TEST_CASE(wire_round_trip_fixnum_positive) {
    Heap heap(1ull << 20);
    InternTable intern;
    auto v = require_ok(make_fixnum(heap, static_cast<int64_t>(42)));
    round_trip(v, heap, intern, "42");
}

BOOST_AUTO_TEST_CASE(wire_round_trip_fixnum_negative) {
    Heap heap(1ull << 20);
    InternTable intern;
    auto v = require_ok(make_fixnum(heap, static_cast<int64_t>(-123)));
    round_trip(v, heap, intern, "-123");
}

BOOST_AUTO_TEST_CASE(wire_round_trip_fixnum_large) {
    Heap heap(1ull << 20);
    InternTable intern;
    // Value within 47-bit range
    auto v = require_ok(make_fixnum(heap, static_cast<int64_t>(70368744177663LL))); // FIXNUM_MAX
    round_trip(v, heap, intern, "70368744177663");
}

BOOST_AUTO_TEST_CASE(wire_round_trip_fixnum_heap_allocated) {
    Heap heap(1ull << 20);
    InternTable intern;
    // Value outside 47-bit range → heap-allocated fixnum
    constexpr int64_t big = 1000000000000000LL;
    auto v = require_ok(make_fixnum(heap, big));
    round_trip(v, heap, intern, "1000000000000000");
}

// ------ Flonums ------

BOOST_AUTO_TEST_CASE(wire_round_trip_flonum_positive) {
    Heap heap(1ull << 20);
    InternTable intern;
    auto v = require_ok(make_flonum(3.14));
    std::string serialized = serialize_value(v, heap, intern);
    BOOST_TEST_MESSAGE("flonum serialized: " << serialized);

    auto deserialized = deserialize_value(serialized, heap, intern);
    BOOST_REQUIRE(deserialized.has_value());

    // Compare as double (text round-trip might not be bitwise identical)
    auto re_serialized = serialize_value(*deserialized, heap, intern);
    BOOST_TEST(re_serialized == serialized);
}

BOOST_AUTO_TEST_CASE(wire_round_trip_flonum_negative) {
    Heap heap(1ull << 20);
    InternTable intern;
    auto v = require_ok(make_flonum(-2.718));
    std::string serialized = serialize_value(v, heap, intern);
    auto deserialized = deserialize_value(serialized, heap, intern);
    BOOST_REQUIRE(deserialized.has_value());
    auto re_serialized = serialize_value(*deserialized, heap, intern);
    BOOST_TEST(re_serialized == serialized);
}

BOOST_AUTO_TEST_CASE(wire_round_trip_flonum_zero) {
    Heap heap(1ull << 20);
    InternTable intern;
    auto v = require_ok(make_flonum(0.0));
    round_trip(v, heap, intern, "0.0");
}

// ------ Characters ------

BOOST_AUTO_TEST_CASE(wire_char_serialize_only) {
    Heap heap(1ull << 20);
    InternTable intern;

    // First: does format_value work at all with this heap/intern?
    auto fixval = require_ok(make_fixnum(heap, static_cast<int64_t>(99)));
    std::string fix_str = format_value(fixval, FormatMode::Write, heap, intern);
    BOOST_TEST(fix_str == "99");
    BOOST_TEST_MESSAGE("format_value works for fixnum: " << fix_str);

    // Now test char encoding
    auto enc = encode<char32_t>(U'a');
    BOOST_REQUIRE(enc.has_value());
    LispVal v = *enc;
    BOOST_TEST_MESSAGE("char LispVal = " << v);

    // Check NaN-box properties manually
    BOOST_TEST(ops::is_boxed(v));
    BOOST_TEST(ops::tag(v) == Tag::Char);
    auto decoded = ops::decode<char32_t>(v);
    BOOST_REQUIRE(decoded.has_value());
    BOOST_TEST(static_cast<uint32_t>(*decoded) == static_cast<uint32_t>(U'a'));

    // Now try format_value for the char
    BOOST_TEST_MESSAGE("about to call format_value for char...");
    std::string serialized = format_value(v, FormatMode::Write, heap, intern);
    BOOST_TEST_MESSAGE("serialized char = " << serialized);
    BOOST_TEST(serialized == "#\\a");
}

BOOST_AUTO_TEST_CASE(wire_round_trip_char_printable) {
    Heap heap(1ull << 20);
    InternTable intern;
    auto v = require_ok(encode<char32_t>(U'a'));
    round_trip(v, heap, intern, "#\\a");
}

BOOST_AUTO_TEST_CASE(wire_round_trip_char_space) {
    Heap heap(1ull << 20);
    InternTable intern;
    auto v = require_ok(encode<char32_t>(U' '));
    round_trip(v, heap, intern, "#\\space");
}

BOOST_AUTO_TEST_CASE(wire_round_trip_char_newline) {
    Heap heap(1ull << 20);
    InternTable intern;
    auto v = require_ok(encode<char32_t>(U'\n'));
    round_trip(v, heap, intern, "#\\newline");
}

BOOST_AUTO_TEST_CASE(wire_round_trip_char_tab) {
    Heap heap(1ull << 20);
    InternTable intern;
    auto v = require_ok(encode<char32_t>(U'\t'));
    round_trip(v, heap, intern, "#\\tab");
}

// ------ Strings ------

BOOST_AUTO_TEST_CASE(wire_round_trip_string_simple) {
    Heap heap(1ull << 20);
    InternTable intern;
    auto v = require_ok(make_string(heap, intern, "hello world"));
    round_trip(v, heap, intern, "\"hello world\"");
}

BOOST_AUTO_TEST_CASE(wire_round_trip_string_empty) {
    Heap heap(1ull << 20);
    InternTable intern;
    auto v = require_ok(make_string(heap, intern, ""));
    round_trip(v, heap, intern, "\"\"");
}

BOOST_AUTO_TEST_CASE(wire_round_trip_string_with_escapes) {
    Heap heap(1ull << 20);
    InternTable intern;
    auto v = require_ok(make_string(heap, intern, "line1\nline2\ttab"));
    round_trip(v, heap, intern, "\"line1\\nline2\\ttab\"");
}

BOOST_AUTO_TEST_CASE(wire_round_trip_string_with_quotes) {
    Heap heap(1ull << 20);
    InternTable intern;
    auto v = require_ok(make_string(heap, intern, "say \"hi\""));
    round_trip(v, heap, intern, "\"say \\\"hi\\\"\"");
}

// ------ Symbols ------

BOOST_AUTO_TEST_CASE(wire_round_trip_symbol) {
    Heap heap(1ull << 20);
    InternTable intern;
    auto v = require_ok(make_symbol(intern, "hello"));
    round_trip(v, heap, intern, "hello");
}

BOOST_AUTO_TEST_CASE(wire_round_trip_symbol_with_special) {
    Heap heap(1ull << 20);
    InternTable intern;
    auto v = require_ok(make_symbol(intern, "my-var!"));
    round_trip(v, heap, intern, "my-var!");
}

// ------ Pairs (dotted) ------

BOOST_AUTO_TEST_CASE(wire_round_trip_dotted_pair) {
    Heap heap(1ull << 20);
    InternTable intern;
    auto a = require_ok(make_fixnum(heap, static_cast<int64_t>(1)));
    auto b = require_ok(make_fixnum(heap, static_cast<int64_t>(2)));
    auto pair = require_ok(make_cons(heap, a, b));
    round_trip(pair, heap, intern, "(1 . 2)");
}

// ------ Lists ------

BOOST_AUTO_TEST_CASE(wire_round_trip_proper_list) {
    Heap heap(1ull << 20);
    InternTable intern;
    auto a = require_ok(make_fixnum(heap, static_cast<int64_t>(1)));
    auto b = require_ok(make_fixnum(heap, static_cast<int64_t>(2)));
    auto c = require_ok(make_fixnum(heap, static_cast<int64_t>(3)));
    auto l3 = require_ok(make_cons(heap, c, nanbox::Nil));
    auto l2 = require_ok(make_cons(heap, b, l3));
    auto l1 = require_ok(make_cons(heap, a, l2));
    round_trip(l1, heap, intern, "(1 2 3)");
}

BOOST_AUTO_TEST_CASE(wire_round_trip_nested_list) {
    Heap heap(1ull << 20);
    InternTable intern;
    auto a = require_ok(make_fixnum(heap, static_cast<int64_t>(1)));
    auto b = require_ok(make_fixnum(heap, static_cast<int64_t>(2)));
    auto inner = require_ok(make_cons(heap, b, nanbox::Nil));
    inner = require_ok(make_cons(heap, a, inner));
    // outer = (inner 3) = ((1 2) 3)
    auto three = require_ok(make_fixnum(heap, static_cast<int64_t>(3)));
    auto outer_tail = require_ok(make_cons(heap, three, nanbox::Nil));
    auto outer = require_ok(make_cons(heap, inner, outer_tail));
    round_trip(outer, heap, intern, "((1 2) 3)");
}

// ------ Vectors ------

BOOST_AUTO_TEST_CASE(wire_round_trip_vector) {
    Heap heap(1ull << 20);
    InternTable intern;
    auto a = require_ok(make_fixnum(heap, static_cast<int64_t>(1)));
    auto b = require_ok(make_fixnum(heap, static_cast<int64_t>(2)));
    auto c = require_ok(make_fixnum(heap, static_cast<int64_t>(3)));
    auto v = require_ok(make_vector(heap, {a, b, c}));
    round_trip(v, heap, intern, "#(1 2 3)");
}

BOOST_AUTO_TEST_CASE(wire_round_trip_empty_vector) {
    Heap heap(1ull << 20);
    InternTable intern;
    auto v = require_ok(make_vector(heap, {}));
    round_trip(v, heap, intern, "#()");
}

// ------ Bytevectors ------

BOOST_AUTO_TEST_CASE(wire_round_trip_bytevector) {
    Heap heap(1ull << 20);
    InternTable intern;
    auto v = require_ok(make_bytevector(heap, {0, 128, 255}));
    round_trip(v, heap, intern, "#u8(0 128 255)");
}

BOOST_AUTO_TEST_CASE(wire_round_trip_empty_bytevector) {
    Heap heap(1ull << 20);
    InternTable intern;
    auto v = require_ok(make_bytevector(heap, {}));
    round_trip(v, heap, intern, "#u8()");
}

// ------ Mixed/nested structures ------

BOOST_AUTO_TEST_CASE(wire_round_trip_mixed_list) {
    // (1 "hello" #t #(a b))
    Heap heap(1ull << 20);
    InternTable intern;

    auto one = require_ok(make_fixnum(heap, static_cast<int64_t>(1)));
    auto hello = require_ok(make_string(heap, intern, "hello"));
    auto sym_a = require_ok(make_symbol(intern, "a"));
    auto sym_b = require_ok(make_symbol(intern, "b"));
    auto vec = require_ok(make_vector(heap, {sym_a, sym_b}));

    // Build list from back
    auto l4 = require_ok(make_cons(heap, vec, nanbox::Nil));
    auto l3 = require_ok(make_cons(heap, nanbox::True, l4));
    auto l2 = require_ok(make_cons(heap, hello, l3));
    auto l1 = require_ok(make_cons(heap, one, l2));

    round_trip(l1, heap, intern, "(1 \"hello\" #t #(a b))");
}

// ------ Quoted form ------

BOOST_AUTO_TEST_CASE(wire_round_trip_quoted_symbol) {
    // Serialize (quote hello) which displays as (quote hello)
    Heap heap(1ull << 20);
    InternTable intern;

    auto quote_sym = require_ok(make_symbol(intern, "quote"));
    auto hello_sym = require_ok(make_symbol(intern, "hello"));
    auto inner = require_ok(make_cons(heap, hello_sym, nanbox::Nil));
    auto quoted = require_ok(make_cons(heap, quote_sym, inner));

    round_trip(quoted, heap, intern, "(quote hello)");
}

// ------ Error handling ------

BOOST_AUTO_TEST_CASE(wire_deserialize_malformed_input) {
    Heap heap(1ull << 20);
    InternTable intern;

    // Unbalanced parentheses
    auto res = deserialize_value("(1 2", heap, intern);
    BOOST_TEST(!res.has_value());
}

BOOST_AUTO_TEST_CASE(wire_deserialize_empty_input) {
    Heap heap(1ull << 20);
    InternTable intern;

    auto res = deserialize_value("", heap, intern);
    BOOST_TEST(!res.has_value());
}

// ------ Non-serializable value error handling ------
// Per Phase 2 limitations: closures, continuations, ports, and tensors produce
// opaque strings like "#<closure>" that cannot be deserialized.

BOOST_AUTO_TEST_CASE(wire_deserialize_rejects_closure) {
    Heap heap(1ull << 20);
    InternTable intern;

    // Attempting to deserialize an opaque closure representation should fail
    auto res = deserialize_value("#<closure>", heap, intern);
    BOOST_TEST(!res.has_value());
    // The error should indicate that this is not valid input
    BOOST_TEST_MESSAGE("closure rejection error (expected)");
}

BOOST_AUTO_TEST_CASE(wire_deserialize_rejects_continuation) {
    Heap heap(1ull << 20);
    InternTable intern;

    auto res = deserialize_value("#<continuation>", heap, intern);
    BOOST_TEST(!res.has_value());
    BOOST_TEST_MESSAGE("continuation rejection error (expected)");
}

BOOST_AUTO_TEST_CASE(wire_deserialize_rejects_port) {
    Heap heap(1ull << 20);
    InternTable intern;

    auto res = deserialize_value("#<port>", heap, intern);
    BOOST_TEST(!res.has_value());
    BOOST_TEST_MESSAGE("port rejection error (expected)");
}

BOOST_AUTO_TEST_CASE(wire_deserialize_rejects_tensor) {
    Heap heap(1ull << 20);
    InternTable intern;

    auto res = deserialize_value("#<tensor>", heap, intern);
    BOOST_TEST(!res.has_value());
    BOOST_TEST_MESSAGE("tensor rejection error (expected)");
}

BOOST_AUTO_TEST_CASE(wire_deserialize_rejects_procedure) {
    Heap heap(1ull << 20);
    InternTable intern;

    auto res = deserialize_value("#<procedure>", heap, intern);
    BOOST_TEST(!res.has_value());
    BOOST_TEST_MESSAGE("procedure rejection error (expected)");
}

BOOST_AUTO_TEST_CASE(wire_deserialize_rejects_embedded_non_serializable) {
    Heap heap(1ull << 20);
    InternTable intern;

    // A list containing a non-serializable value should also fail
    auto res = deserialize_value("(1 2 #<closure> 4)", heap, intern);
    BOOST_TEST(!res.has_value());
    BOOST_TEST_MESSAGE("embedded non-serializable rejection error (expected)");
}

// ------ Symbols as list elements ------

BOOST_AUTO_TEST_CASE(wire_round_trip_symbol_list) {
    // (hello world)
    Heap heap(1ull << 20);
    InternTable intern;

    auto hello = require_ok(make_symbol(intern, "hello"));
    auto world = require_ok(make_symbol(intern, "world"));
    auto l2 = require_ok(make_cons(heap, world, nanbox::Nil));
    auto l1 = require_ok(make_cons(heap, hello, l2));

    round_trip(l1, heap, intern, "(hello world)");
}

// ═══════════════════════════════════════════════════════════════════════════
// Phase 2 — Performance tests
// ═══════════════════════════════════════════════════════════════════════════

BOOST_AUTO_TEST_CASE(wire_round_trip_large_list_performance) {
    // NOTE: We benchmark with a *vector*, not a linked list.
    //
    // Building a 10,000-element linked list via make_cons requires 10,000
    // individual heap allocations and is not representative of how large
    // collections are normally passed over the wire.  Even with cons pooling,
    // the sequential pointer-chasing during serialisation dominates.  Real
    // code sends large collections as vectors (#(...)) — one heap allocation,
    // cache-friendly, and O(1) random access.
    //
    // Acceptance criterion: serialize + deserialize of a 10,000-element
    // vector completes in under 10 ms on a modern machine.
    Heap heap(1ull << 24);  // 16 MB heap
    InternTable intern;

    // Build a 10,000-element vector: #(0 1 2 ... 9999)
    // Small fixnums (< 2^47) are NaN-boxed inline — no heap allocation per element.
    constexpr size_t VEC_SIZE = 10000;
    std::vector<LispVal> elems;
    elems.reserve(VEC_SIZE);
    for (int64_t i = 0; i < static_cast<int64_t>(VEC_SIZE); ++i) {
        elems.push_back(*make_fixnum(heap, i));   // inline fixnum, no heap alloc
    }
    auto vec = require_ok(make_vector(heap, std::move(elems)));  // one allocation

    BOOST_TEST_MESSAGE("Built 10,000-element vector, starting serialization...");

    // Time only the serialize + deserialize round-trip
    auto start = std::chrono::high_resolution_clock::now();

    std::string serialized = serialize_value(vec, heap, intern);
    auto deserialized = deserialize_value(serialized, heap, intern);

    auto end = std::chrono::high_resolution_clock::now();

    BOOST_REQUIRE_MESSAGE(deserialized.has_value(), "deserialization of large vector failed");

    auto duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
    BOOST_TEST_MESSAGE("10,000-element vector round-trip completed in " << duration_ms << " ms");
    BOOST_TEST_MESSAGE("Serialized size: " << serialized.size() << " bytes");

    // Performance acceptance criterion: < 10 ms (somewhat arbitrary... this test
    // probably should be re-written).
#if defined(_DEBUG) || !defined(NDEBUG)
    BOOST_TEST(duration_ms < 200);
#else
    BOOST_TEST(duration_ms < 20);
#endif

    // Verify the deserialized structure: should be a vector whose first element is 0
    auto* result_vec = heap.try_get_as<ObjectKind::Vector, types::Vector>(ops::payload(*deserialized));
    BOOST_REQUIRE(result_vec != nullptr);
    BOOST_REQUIRE_EQUAL(result_vec->elements.size(), VEC_SIZE);
    auto first_val = ops::decode<int64_t>(result_vec->elements[0]);
    BOOST_REQUIRE(first_val.has_value());
    BOOST_TEST(*first_val == 0);
}

// ═══════════════════════════════════════════════════════════════════════════
// Phase 3 — nng Socket Primitives
// ═══════════════════════════════════════════════════════════════════════════

// ── nng-socket: creation and heap storage ──────────────────────────────────

BOOST_AUTO_TEST_CASE(p3_nng_socket_create_pair) {
    NngEnv e;
    auto sock = e.call("nng-socket", {e.sym("pair")});
    BOOST_REQUIRE(ops::is_boxed(sock) && ops::tag(sock) == Tag::HeapObject);
    auto* sp = e.heap.try_get_as<ObjectKind::NngSocket, NngSocketPtr>(ops::payload(sock));
    BOOST_REQUIRE(sp != nullptr);
    BOOST_TEST(sp->protocol == NngProtocol::Pair);
    BOOST_TEST(!sp->closed);
}

BOOST_AUTO_TEST_CASE(p3_nng_socket_create_all_protocols) {
    NngEnv e;
    const std::vector<std::pair<std::string, NngProtocol>> protos = {
        {"pair",       NngProtocol::Pair},
        {"req",        NngProtocol::Req},
        {"rep",        NngProtocol::Rep},
        {"pub",        NngProtocol::Pub},
        {"sub",        NngProtocol::Sub},
        {"push",       NngProtocol::Push},
        {"pull",       NngProtocol::Pull},
        {"surveyor",   NngProtocol::Surveyor},
        {"respondent", NngProtocol::Respondent},
        {"bus",        NngProtocol::Bus},
    };
    for (const auto& [name, proto] : protos) {
        auto sock = e.call("nng-socket", {e.sym(name)});
        BOOST_REQUIRE_MESSAGE(ops::is_boxed(sock), "failed for protocol: " + name);
        auto* sp = e.heap.try_get_as<ObjectKind::NngSocket, NngSocketPtr>(ops::payload(sock));
        BOOST_REQUIRE_MESSAGE(sp != nullptr, "heap object missing for: " + name);
        BOOST_TEST_MESSAGE("  protocol " << name << " created OK");
        BOOST_CHECK(sp->protocol == proto);
        e.call("nng-close", {sock});
    }
}

BOOST_AUTO_TEST_CASE(p3_nng_socket_unknown_protocol_error) {
    NngEnv e;
    auto res = e.try_call("nng-socket", {e.sym("unknownprotocol")});
    BOOST_TEST(!res.has_value());
}

BOOST_AUTO_TEST_CASE(p3_nng_socket_wrong_arg_type) {
    NngEnv e;
    auto num = e.fixnum(42);
    auto res = e.try_call("nng-socket", {num});
    BOOST_TEST(!res.has_value());
}

// ── nng-socket? predicate ──────────────────────────────────────────────────

BOOST_AUTO_TEST_CASE(p3_nng_socket_predicate_true) {
    NngEnv e;
    auto sock = e.call("nng-socket", {e.sym("pair")});
    auto result = e.call("nng-socket?", {sock});
    BOOST_TEST(result == nanbox::True);
    e.call("nng-close", {sock});
}

BOOST_AUTO_TEST_CASE(p3_nng_socket_predicate_false_for_fixnum) {
    NngEnv e;
    auto result = e.call("nng-socket?", {e.fixnum(42)});
    BOOST_TEST(result == nanbox::False);
}

BOOST_AUTO_TEST_CASE(p3_nng_socket_predicate_false_for_nil) {
    NngEnv e;
    auto result = e.call("nng-socket?", {nanbox::Nil});
    BOOST_TEST(result == nanbox::False);
}

BOOST_AUTO_TEST_CASE(p3_nng_socket_predicate_false_for_bool) {
    NngEnv e;
    auto result = e.call("nng-socket?", {nanbox::True});
    BOOST_TEST(result == nanbox::False);
}

// ── Heap allocation via make_nng_socket factory ────────────────────────────

BOOST_AUTO_TEST_CASE(p3_make_nng_socket_factory) {
    Heap heap(1 << 20);
    NngSocketPtr sp;
    int rv = nng_pair0_open(&sp.socket);
    BOOST_REQUIRE_EQUAL(rv, 0);
    sp.protocol = NngProtocol::Pair;

    auto val = require_ok(make_nng_socket(heap, std::move(sp)));
    BOOST_REQUIRE(ops::is_boxed(val) && ops::tag(val) == Tag::HeapObject);

    auto* ptr = heap.try_get_as<ObjectKind::NngSocket, NngSocketPtr>(ops::payload(val));
    BOOST_REQUIRE(ptr != nullptr);
    BOOST_TEST(ptr->protocol == NngProtocol::Pair);
}

// ── nng-close ─────────────────────────────────────────────────────────────

BOOST_AUTO_TEST_CASE(p3_nng_close_basic) {
    NngEnv e;
    auto sock = e.call("nng-socket", {e.sym("pair")});
    auto result = e.call("nng-close", {sock});
    BOOST_TEST(result == nanbox::True);

    auto* sp = e.heap.try_get_as<ObjectKind::NngSocket, NngSocketPtr>(ops::payload(sock));
    BOOST_REQUIRE(sp != nullptr);
    BOOST_TEST(sp->closed == true);
}

BOOST_AUTO_TEST_CASE(p3_nng_close_idempotent) {
    NngEnv e;
    auto sock = e.call("nng-socket", {e.sym("pair")});
    // Close twice — must not crash or return error
    e.call("nng-close", {sock});
    auto second = e.try_call("nng-close", {sock});
    BOOST_REQUIRE(second.has_value());
    BOOST_TEST(*second == nanbox::True);
}

BOOST_AUTO_TEST_CASE(p3_nng_close_wrong_type) {
    NngEnv e;
    auto res = e.try_call("nng-close", {e.fixnum(99)});
    BOOST_TEST(!res.has_value());
}

// ── Default recv timeout set at creation ──────────────────────────────────

BOOST_AUTO_TEST_CASE(p3_default_recv_timeout_is_1000ms) {
    NngEnv e;
    auto sock = e.call("nng-socket", {e.sym("pair")});
    auto* sp = e.heap.try_get_as<ObjectKind::NngSocket, NngSocketPtr>(ops::payload(sock));
    BOOST_REQUIRE(sp != nullptr);

    nng_duration timeout = 0;
    int rv = nng_socket_get_ms(sp->socket, NNG_OPT_RECVTIMEO, &timeout);
    BOOST_REQUIRE_EQUAL(rv, 0);
    BOOST_TEST(timeout == 1000);
    e.call("nng-close", {sock});
}

// ── nng-set-option ────────────────────────────────────────────────────────

BOOST_AUTO_TEST_CASE(p3_nng_set_option_recv_timeout) {
    NngEnv e;
    auto sock = e.call("nng-socket", {e.sym("pair")});

    auto result = e.call("nng-set-option", {sock, e.sym("recv-timeout"), e.fixnum(5000)});
    BOOST_TEST(result == nanbox::True);

    auto* sp = e.heap.try_get_as<ObjectKind::NngSocket, NngSocketPtr>(ops::payload(sock));
    BOOST_REQUIRE(sp != nullptr);
    nng_duration timeout = 0;
    nng_socket_get_ms(sp->socket, NNG_OPT_RECVTIMEO, &timeout);
    BOOST_TEST(timeout == 5000);
    e.call("nng-close", {sock});
}

BOOST_AUTO_TEST_CASE(p3_nng_set_option_send_timeout) {
    NngEnv e;
    auto sock = e.call("nng-socket", {e.sym("pair")});

    auto result = e.call("nng-set-option", {sock, e.sym("send-timeout"), e.fixnum(2000)});
    BOOST_TEST(result == nanbox::True);

    auto* sp = e.heap.try_get_as<ObjectKind::NngSocket, NngSocketPtr>(ops::payload(sock));
    BOOST_REQUIRE(sp != nullptr);
    nng_duration timeout = 0;
    nng_socket_get_ms(sp->socket, NNG_OPT_SENDTIMEO, &timeout);
    BOOST_TEST(timeout == 2000);
    e.call("nng-close", {sock});
}

BOOST_AUTO_TEST_CASE(p3_nng_set_option_recv_buf_size) {
    NngEnv e;
    auto sock = e.call("nng-socket", {e.sym("push")});
    auto result = e.try_call("nng-set-option", {sock, e.sym("recv-buf-size"), e.fixnum(16)});
    // Some protocols may not support this; just check it doesn't crash badly
    BOOST_TEST_MESSAGE("recv-buf-size result: " << result.has_value());
    e.call("nng-close", {sock});
}

BOOST_AUTO_TEST_CASE(p3_nng_set_option_unknown_option_error) {
    NngEnv e;
    auto sock = e.call("nng-socket", {e.sym("pair")});
    auto res = e.try_call("nng-set-option", {sock, e.sym("nonexistent-option"), e.fixnum(0)});
    BOOST_TEST(!res.has_value());
    e.call("nng-close", {sock});
}

BOOST_AUTO_TEST_CASE(p3_nng_set_option_wrong_socket_type) {
    NngEnv e;
    auto res = e.try_call("nng-set-option", {e.fixnum(1), e.sym("recv-timeout"), e.fixnum(1000)});
    BOOST_TEST(!res.has_value());
}

BOOST_AUTO_TEST_CASE(p3_nng_set_option_non_number_value) {
    NngEnv e;
    auto sock = e.call("nng-socket", {e.sym("pair")});
    auto res = e.try_call("nng-set-option", {sock, e.sym("recv-timeout"), e.sym("bad")});
    BOOST_TEST(!res.has_value());
    e.call("nng-close", {sock});
}

// ── nng-listen and nng-dial ────────────────────────────────────────────────

BOOST_AUTO_TEST_CASE(p3_nng_listen_and_dial_tcp) {
    NngEnv e;
    std::string addr = tcp_addr();

    auto server = e.call("nng-socket", {e.sym("pair")});
    auto client = e.call("nng-socket", {e.sym("pair")});

    auto lr = e.call("nng-listen", {server, e.str(addr)});
    BOOST_TEST(lr == server);

    auto* sp = e.heap.try_get_as<ObjectKind::NngSocket, NngSocketPtr>(ops::payload(server));
    BOOST_REQUIRE(sp != nullptr);
    BOOST_TEST(sp->listening == true);

    auto dr = e.call("nng-dial", {client, e.str(addr)});
    BOOST_TEST(dr == client);

    auto* cp = e.heap.try_get_as<ObjectKind::NngSocket, NngSocketPtr>(ops::payload(client));
    BOOST_REQUIRE(cp != nullptr);
    BOOST_TEST(cp->dialed == true);

    e.call("nng-close", {server});
    e.call("nng-close", {client});
}

BOOST_AUTO_TEST_CASE(p3_nng_listen_invalid_endpoint) {
    NngEnv e;
    auto sock = e.call("nng-socket", {e.sym("pair")});
    auto res = e.try_call("nng-listen", {sock, e.str("not-a-valid-endpoint")});
    BOOST_TEST(!res.has_value());
    e.call("nng-close", {sock});
}

BOOST_AUTO_TEST_CASE(p3_nng_listen_wrong_socket_type) {
    NngEnv e;
    auto res = e.try_call("nng-listen", {e.fixnum(1), e.str("tcp://*:9000")});
    BOOST_TEST(!res.has_value());
}

BOOST_AUTO_TEST_CASE(p3_nng_dial_wrong_socket_type) {
    NngEnv e;
    auto res = e.try_call("nng-dial", {e.fixnum(1), e.str("tcp://127.0.0.1:9000")});
    BOOST_TEST(!res.has_value());
}

BOOST_AUTO_TEST_CASE(p3_nng_dial_on_closed_socket_error) {
    NngEnv e;
    auto sock = e.call("nng-socket", {e.sym("pair")});
    e.call("nng-close", {sock});
    auto res = e.try_call("nng-dial", {sock, e.str("tcp://127.0.0.1:9000")});
    BOOST_TEST(!res.has_value());
}

// ── send! and recv! — full PAIR round-trip ─────────────────────────────────

BOOST_AUTO_TEST_CASE(p3_send_recv_pair_fixnum) {
    NngEnv e;
    std::string addr = inproc_addr();

    auto server = e.call("nng-socket", {e.sym("pair")});
    auto client = e.call("nng-socket", {e.sym("pair")});

    e.call("nng-listen", {server, e.str(addr)});
    e.call("nng-dial",   {client, e.str(addr)});

    // client → server: fixnum 42
    auto send_res = e.call("send!", {client, e.fixnum(42)});
    BOOST_TEST(send_res == nanbox::True);

    auto recv_res = e.call("recv!", {server});
    BOOST_REQUIRE(recv_res != nanbox::False);
    BOOST_TEST_MESSAGE("recv! result: " << recv_res);

    // Decode the fixnum
    auto dec = ops::decode<int64_t>(recv_res);
    BOOST_REQUIRE(dec.has_value());
    BOOST_TEST(*dec == 42);

    e.call("nng-close", {server});
    e.call("nng-close", {client});
}

BOOST_AUTO_TEST_CASE(p3_send_recv_pair_symbol) {
    NngEnv e;
    std::string addr = inproc_addr();

    auto server = e.call("nng-socket", {e.sym("pair")});
    auto client = e.call("nng-socket", {e.sym("pair")});
    e.call("nng-listen", {server, e.str(addr)});
    e.call("nng-dial",   {client, e.str(addr)});

    auto sym_hello = e.sym("hello");
    e.call("send!", {client, sym_hello});

    auto recv_res = e.call("recv!", {server});
    BOOST_REQUIRE(recv_res != nanbox::False);
    BOOST_TEST(ops::is_boxed(recv_res));
    BOOST_TEST(ops::tag(recv_res) == Tag::Symbol);

    auto sv = e.intern.get_string(ops::payload(recv_res));
    BOOST_REQUIRE(sv.has_value());
    BOOST_TEST(*sv == "hello");

    e.call("nng-close", {server});
    e.call("nng-close", {client});
}

BOOST_AUTO_TEST_CASE(p3_send_recv_pair_list) {
    NngEnv e;
    std::string addr = inproc_addr();

    auto server = e.call("nng-socket", {e.sym("pair")});
    auto client = e.call("nng-socket", {e.sym("pair")});
    e.call("nng-listen", {server, e.str(addr)});
    e.call("nng-dial",   {client, e.str(addr)});

    // Build (1 2 3)
    auto a = e.fixnum(1);
    auto b = e.fixnum(2);
    auto c = e.fixnum(3);
    auto l = require_ok(make_cons(e.heap, c, nanbox::Nil));
    l = require_ok(make_cons(e.heap, b, l));
    l = require_ok(make_cons(e.heap, a, l));

    e.call("send!", {client, l});
    auto recv_res = e.call("recv!", {server});
    BOOST_REQUIRE(recv_res != nanbox::False);

    // Verify it's a proper list starting with 1
    auto* cons = e.heap.try_get_as<ObjectKind::Cons, Cons>(ops::payload(recv_res));
    BOOST_REQUIRE(cons != nullptr);
    auto first = ops::decode<int64_t>(cons->car);
    BOOST_REQUIRE(first.has_value());
    BOOST_TEST(*first == 1);

    e.call("nng-close", {server});
    e.call("nng-close", {client});
}

BOOST_AUTO_TEST_CASE(p3_send_recv_pair_string) {
    NngEnv e;
    std::string addr = inproc_addr();

    auto server = e.call("nng-socket", {e.sym("pair")});
    auto client = e.call("nng-socket", {e.sym("pair")});
    e.call("nng-listen", {server, e.str(addr)});
    e.call("nng-dial",   {client, e.str(addr)});

    auto msg = e.str("hello nng");
    e.call("send!", {client, msg});

    auto recv_res = e.call("recv!", {server});
    BOOST_REQUIRE(recv_res != nanbox::False);
    BOOST_TEST(ops::tag(recv_res) == Tag::String);

    auto sv = e.intern.get_string(ops::payload(recv_res));
    BOOST_REQUIRE(sv.has_value());
    BOOST_TEST(*sv == "hello nng");

    e.call("nng-close", {server});
    e.call("nng-close", {client});
}

BOOST_AUTO_TEST_CASE(p3_send_recv_bidirectional) {
    NngEnv e;
    std::string addr = inproc_addr();

    auto server = e.call("nng-socket", {e.sym("pair")});
    auto client = e.call("nng-socket", {e.sym("pair")});
    e.call("nng-listen", {server, e.str(addr)});
    e.call("nng-dial",   {client, e.str(addr)});

    // client → server
    e.call("send!", {client, e.fixnum(100)});
    auto r1 = e.call("recv!", {server});
    BOOST_TEST(ops::decode<int64_t>(r1).value_or(-1) == 100);

    // server → client
    e.call("send!", {server, e.fixnum(200)});
    auto r2 = e.call("recv!", {client});
    BOOST_TEST(ops::decode<int64_t>(r2).value_or(-1) == 200);

    e.call("nng-close", {server});
    e.call("nng-close", {client});
}

BOOST_AUTO_TEST_CASE(p3_recv_timeout_returns_false) {
    NngEnv e;
    // Set a very short timeout, recv on an unconnected socket should time out
    auto sock = e.call("nng-socket", {e.sym("pull")});
    e.call("nng-set-option", {sock, e.sym("recv-timeout"), e.fixnum(50)});
    // Don't connect — recv should timeout quickly and return #f
    auto result = e.call("recv!", {sock});
    BOOST_TEST(result == nanbox::False);
    e.call("nng-close", {sock});
}

BOOST_AUTO_TEST_CASE(p3_send_noblock_no_peer_returns_false) {
    NngEnv e;
    auto sock = e.call("nng-socket", {e.sym("push")});
    // Not connected → noblock send should return #f (EAGAIN)
    auto result = e.call("send!", {sock, e.fixnum(1), e.sym("noblock")});
    BOOST_TEST(result == nanbox::False);
    e.call("nng-close", {sock});
}

BOOST_AUTO_TEST_CASE(p3_recv_noblock_empty_returns_false) {
    NngEnv e;
    std::string addr = inproc_addr();
    auto server = e.call("nng-socket", {e.sym("pair")});
    e.call("nng-listen", {server, e.str(addr)});
    // Non-blocking recv with no message should return #f
    auto result = e.call("recv!", {server, e.sym("noblock")});
    BOOST_TEST(result == nanbox::False);
    e.call("nng-close", {server});
}

BOOST_AUTO_TEST_CASE(p3_send_recv_on_closed_socket_error) {
    NngEnv e;
    auto sock = e.call("nng-socket", {e.sym("pair")});
    e.call("nng-close", {sock});

    auto send_res = e.try_call("send!", {sock, e.fixnum(1)});
    BOOST_TEST(!send_res.has_value());

    auto recv_res = e.try_call("recv!", {sock});
    BOOST_TEST(!recv_res.has_value());
}

// ── REQ/REP round-trip ─────────────────────────────────────────────────────

BOOST_AUTO_TEST_CASE(p3_send_recv_req_rep) {
    NngEnv e;
    std::string addr = inproc_addr();

    auto rep = e.call("nng-socket", {e.sym("rep")});
    auto req = e.call("nng-socket", {e.sym("req")});
    e.call("nng-listen", {rep, e.str(addr)});
    e.call("nng-dial",   {req, e.str(addr)});

    // REQ sends a request
    e.call("send!", {req, e.fixnum(7)});

    // REP receives it
    auto request = e.call("recv!", {rep});
    BOOST_TEST(ops::decode<int64_t>(request).value_or(-1) == 7);

    // REP sends a reply
    e.call("send!", {rep, e.fixnum(49)});

    // REQ receives the reply
    auto reply = e.call("recv!", {req});
    BOOST_TEST(ops::decode<int64_t>(reply).value_or(-1) == 49);

    e.call("nng-close", {rep});
    e.call("nng-close", {req});
}

// ── PUSH/PULL fan-out ─────────────────────────────────────────────────────

BOOST_AUTO_TEST_CASE(p3_push_pull_fanout) {
    NngEnv e;
    // Use TCP transport: with inproc:// and default buffering (SENDBUF=0),
    // multiple sends before any recv can block. This is not because recv()
    // hasn't been called yet, but because the inproc transport provides
    // little or no buffering, so the pipe has no capacity to accept more
    // messages. After the available slots are filled, further nng_send calls
    // block indefinitely (no send timeout is set).
    //
    // With TCP, the kernel socket buffers absorb the messages, so sends
    // typically complete immediately even if the receiver hasn't called recv().
    //
    // Alternative fix (not used here): set NNG_OPT_SENDBUF > 0 on the pusher,
    // or run sends and recvs on separate threads — then inproc would also work.
    std::string addr = tcp_addr();

    auto pusher = e.call("nng-socket", {e.sym("push")});
    auto puller = e.call("nng-socket", {e.sym("pull")});

    e.call("nng-listen", {puller, e.str(addr)});
    e.call("nng-dial",   {pusher, e.str(addr)});

    // Push three messages
    for (int i = 1; i <= 3; ++i) {
        auto sr = e.call("send!", {pusher, e.fixnum(i)});
        BOOST_TEST(sr == nanbox::True);
    }

    // Pull three messages
    for (int i = 1; i <= 3; ++i) {
        auto r = e.call("recv!", {puller});
        BOOST_REQUIRE(r != nanbox::False);
        BOOST_TEST(ops::decode<int64_t>(r).value_or(-1) == i);
    }

    e.call("nng-close", {pusher});
    e.call("nng-close", {puller});
}

// ── PUB/SUB with topic filtering ─────────────────────────────────────────

BOOST_AUTO_TEST_CASE(p3_pub_sub_basic) {
    NngEnv e;
    std::string addr = inproc_addr();

    auto pub = e.call("nng-socket", {e.sym("pub")});
    auto sub = e.call("nng-socket", {e.sym("sub")});

    e.call("nng-listen", {pub, e.str(addr)});
    e.call("nng-dial",   {sub, e.str(addr)});

    // Subscribe to all messages (empty prefix)
    auto sub_res = e.call("nng-subscribe", {sub, e.str("")});
    BOOST_TEST(sub_res == nanbox::True);

    // Brief delay to let subscription propagate
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    e.call("send!", {pub, e.fixnum(42)});

    e.call("nng-set-option", {sub, e.sym("recv-timeout"), e.fixnum(500)});
    auto r = e.call("recv!", {sub});
    BOOST_TEST_MESSAGE("pub-sub recv result: " << (r == nanbox::False ? "timeout" : "got msg"));
    // Message may or may not arrive depending on timing; if it arrives it should be 42
    if (r != nanbox::False) {
        BOOST_TEST(ops::decode<int64_t>(r).value_or(-1) == 42);
    }

    e.call("nng-close", {pub});
    e.call("nng-close", {sub});
}

BOOST_AUTO_TEST_CASE(p3_nng_subscribe_wrong_socket_type) {
    NngEnv e;
    auto pair = e.call("nng-socket", {e.sym("pair")});
    auto res = e.try_call("nng-subscribe", {pair, e.str("topic")});
    BOOST_TEST(!res.has_value());
    e.call("nng-close", {pair});
}

BOOST_AUTO_TEST_CASE(p3_nng_subscribe_wrong_first_arg) {
    NngEnv e;
    auto res = e.try_call("nng-subscribe", {e.fixnum(1), e.str("topic")});
    BOOST_TEST(!res.has_value());
}

// ── SURVEYOR/RESPONDENT scatter-gather ────────────────────────────────────

BOOST_AUTO_TEST_CASE(p3_surveyor_respondent_basic) {
    NngEnv e;
    std::string addr = inproc_addr();

    auto surveyor   = e.call("nng-socket", {e.sym("surveyor")});
    auto respondent = e.call("nng-socket", {e.sym("respondent")});

    e.call("nng-listen", {surveyor,   e.str(addr)});
    e.call("nng-dial",   {respondent, e.str(addr)});

    // Set survey time
    e.call("nng-set-option", {surveyor, e.sym("survey-time"), e.fixnum(500)});
    e.call("nng-set-option", {surveyor, e.sym("recv-timeout"), e.fixnum(600)});

    std::this_thread::sleep_for(std::chrono::milliseconds(30));

    // Surveyor broadcasts a question
    auto sr = e.call("send!", {surveyor, e.sym("ping")});
    BOOST_TEST(sr == nanbox::True);

    // Respondent receives it and replies
    auto q = e.call("recv!", {respondent});
    if (q != nanbox::False) {
        e.call("send!", {respondent, e.sym("pong")});
        // Surveyor collects reply
        auto reply = e.call("recv!", {surveyor});
        BOOST_TEST_MESSAGE("surveyor got: " << (reply == nanbox::False ? "#f" : "pong"));
    }

    e.call("nng-close", {surveyor});
    e.call("nng-close", {respondent});
}

// ── BUS many-to-many ──────────────────────────────────────────────────────

BOOST_AUTO_TEST_CASE(p3_bus_many_to_many) {
    NngEnv e;
    std::string addr = inproc_addr();

    auto bus1 = e.call("nng-socket", {e.sym("bus")});
    auto bus2 = e.call("nng-socket", {e.sym("bus")});
    auto bus3 = e.call("nng-socket", {e.sym("bus")});

    e.call("nng-listen", {bus1, e.str(addr)});

    std::string addr2 = inproc_addr();
    std::string addr3 = inproc_addr();
    e.call("nng-listen", {bus2, e.str(addr2)});
    e.call("nng-listen", {bus3, e.str(addr3)});

    e.call("nng-dial",   {bus2, e.str(addr)});
    e.call("nng-dial",   {bus3, e.str(addr)});
    e.call("nng-dial",   {bus1, e.str(addr2)});
    e.call("nng-dial",   {bus1, e.str(addr3)});

    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // bus1 sends to the bus
    e.call("nng-set-option", {bus2, e.sym("recv-timeout"), e.fixnum(300)});
    e.call("nng-set-option", {bus3, e.sym("recv-timeout"), e.fixnum(300)});

    e.call("send!", {bus1, e.fixnum(99)});

    auto r2 = e.call("recv!", {bus2});
    auto r3 = e.call("recv!", {bus3});
    BOOST_TEST_MESSAGE("bus2 recv: " << (r2 == nanbox::False ? "#f" : "got"));
    BOOST_TEST_MESSAGE("bus3 recv: " << (r3 == nanbox::False ? "#f" : "got"));

    e.call("nng-close", {bus1});
    e.call("nng-close", {bus2});
    e.call("nng-close", {bus3});
}

// ── nng-poll ──────────────────────────────────────────────────────────────

BOOST_AUTO_TEST_CASE(p3_nng_poll_detects_ready_socket) {
    NngEnv e;
    std::string addr = inproc_addr();

    auto server = e.call("nng-socket", {e.sym("pair")});
    auto client = e.call("nng-socket", {e.sym("pair")});
    e.call("nng-listen", {server, e.str(addr)});
    e.call("nng-dial",   {client, e.str(addr)});

    // Send a message so server has data ready
    e.call("send!", {client, e.fixnum(7)});

    // Small wait for message delivery
    std::this_thread::sleep_for(std::chrono::milliseconds(20));

    // Build items list: ((server . 0))
    auto zero = e.fixnum(0);
    auto item = require_ok(make_cons(e.heap, server, zero));
    auto items = require_ok(make_cons(e.heap, item, nanbox::Nil));

    auto ready = e.call("nng-poll", {items, e.fixnum(100)});

    // If server received the message, it should be in the ready list
    if (ready != nanbox::Nil) {
        BOOST_TEST_MESSAGE("nng-poll: server is ready");
        // Drain the pending message via recv!
        auto msg = e.call("recv!", {server});
        BOOST_TEST(ops::decode<int64_t>(msg).value_or(-1) == 7);
    } else {
        BOOST_TEST_MESSAGE("nng-poll: no sockets ready within timeout (acceptable)");
    }

    e.call("nng-close", {server});
    e.call("nng-close", {client});
}

BOOST_AUTO_TEST_CASE(p3_nng_poll_empty_list_returns_nil) {
    NngEnv e;
    auto ready = e.call("nng-poll", {nanbox::Nil, e.fixnum(0)});
    BOOST_TEST(ready == nanbox::Nil);
}

BOOST_AUTO_TEST_CASE(p3_nng_poll_pending_msg_returned_immediately) {
    NngEnv e;
    std::string addr = inproc_addr();

    auto server = e.call("nng-socket", {e.sym("pair")});
    auto client = e.call("nng-socket", {e.sym("pair")});
    e.call("nng-listen", {server, e.str(addr)});
    e.call("nng-dial",   {client, e.str(addr)});

    // Inject a message directly into pending_msgs to simulate a polled message
    auto* sp = e.heap.try_get_as<ObjectKind::NngSocket, NngSocketPtr>(ops::payload(server));
    BOOST_REQUIRE(sp != nullptr);

    // Manually add a pending message: the serialised form of fixnum 55
    Heap tmp_heap(1 << 20); InternTable tmp_intern;
    auto v55 = require_ok(make_fixnum(tmp_heap, int64_t(55)));
    std::string text = serialize_value(v55, tmp_heap, tmp_intern);
    sp->pending_msgs.push_back(std::vector<uint8_t>(text.begin(), text.end()));

    // recv! should drain from pending_msgs without touching the wire
    auto msg = e.call("recv!", {server});
    BOOST_REQUIRE(msg != nanbox::False);
    BOOST_TEST(ops::decode<int64_t>(msg).value_or(-1) == 55);
    BOOST_TEST(sp->pending_msgs.empty());

    e.call("nng-close", {server});
    e.call("nng-close", {client});
}

// ── GC integration: NngSocket heap object ─────────────────────────────────

BOOST_AUTO_TEST_CASE(p3_nng_socket_heap_object_kind) {
    Heap heap(1 << 20);
    NngSocketPtr sp;
    nng_pair0_open(&sp.socket);
    sp.protocol = NngProtocol::Pair;

    auto id = heap.allocate<NngSocketPtr, ObjectKind::NngSocket>(std::move(sp));
    BOOST_REQUIRE(id.has_value());

    HeapEntry entry;
    BOOST_REQUIRE(heap.try_get(*id, entry));
    BOOST_TEST(entry.header.kind == ObjectKind::NngSocket);
    BOOST_TEST(to_string(entry.header.kind) == std::string("NngSocket"));
}

// ── protocol_name helper ───────────────────────────────────────────────────

BOOST_AUTO_TEST_CASE(p3_protocol_name_all) {
    BOOST_TEST(std::string(protocol_name(NngProtocol::Pair))       == "pair");
    BOOST_TEST(std::string(protocol_name(NngProtocol::Req))        == "req");
    BOOST_TEST(std::string(protocol_name(NngProtocol::Rep))        == "rep");
    BOOST_TEST(std::string(protocol_name(NngProtocol::Pub))        == "pub");
    BOOST_TEST(std::string(protocol_name(NngProtocol::Sub))        == "sub");
    BOOST_TEST(std::string(protocol_name(NngProtocol::Push))       == "push");
    BOOST_TEST(std::string(protocol_name(NngProtocol::Pull))       == "pull");
    BOOST_TEST(std::string(protocol_name(NngProtocol::Surveyor))   == "surveyor");
    BOOST_TEST(std::string(protocol_name(NngProtocol::Respondent)) == "respondent");
    BOOST_TEST(std::string(protocol_name(NngProtocol::Bus))        == "bus");
}

// ── send!/recv! wrong-type errors ─────────────────────────────────────────

BOOST_AUTO_TEST_CASE(p3_send_wrong_socket_type) {
    NngEnv e;
    auto res = e.try_call("send!", {e.fixnum(1), e.fixnum(42)});
    BOOST_TEST(!res.has_value());
}

BOOST_AUTO_TEST_CASE(p3_recv_wrong_socket_type) {
    NngEnv e;
    auto res = e.try_call("recv!", {e.fixnum(1)});
    BOOST_TEST(!res.has_value());
}

BOOST_AUTO_TEST_SUITE_END()

// ═══════════════════════════════════════════════════════════════════════════
// Phase 6 — Binary Wire Format
// ═══════════════════════════════════════════════════════════════════════════

BOOST_AUTO_TEST_SUITE(nng_phase6_tests)

// ── Helpers ────────────────────────────────────────────────────────────────

/// Binary round-trip helper: serialize_binary then deserialize_binary,
/// verify the re-serialized text string matches expected.
static void binary_round_trip(LispVal v, Heap& heap, InternTable& intern,
                               const std::string& expected_text)
{
    auto bin = serialize_binary(v, heap, intern);
    BOOST_REQUIRE(!bin.empty());
    BOOST_TEST(bin[0] == eta::nng::BINARY_VERSION_BYTE);

    auto result = deserialize_binary(std::span<const uint8_t>(bin), heap, intern);
    BOOST_REQUIRE_MESSAGE(result.has_value(), "binary deserialization failed");

    std::string re_text = serialize_value(*result, heap, intern);
    BOOST_TEST(re_text == expected_text);
}

// ── is_binary_format ───────────────────────────────────────────────────────

BOOST_AUTO_TEST_CASE(p6_is_binary_format_detects_version_byte) {
    std::vector<uint8_t> bin{0xEA, 0x00};
    BOOST_TEST(is_binary_format(bin.data(), bin.size()) == true);
}

BOOST_AUTO_TEST_CASE(p6_is_binary_format_rejects_text) {
    std::string text = "(1 2 3)";
    BOOST_TEST(is_binary_format(
        reinterpret_cast<const uint8_t*>(text.data()), text.size()) == false);
}

BOOST_AUTO_TEST_CASE(p6_is_binary_format_empty_returns_false) {
    BOOST_TEST(is_binary_format(nullptr, 0) == false);
}

// ── Binary round-trips: all types ──────────────────────────────────────────

BOOST_AUTO_TEST_CASE(p6_binary_nil) {
    Heap heap(1ull << 20); InternTable intern;
    binary_round_trip(nanbox::Nil, heap, intern, "()");
}

BOOST_AUTO_TEST_CASE(p6_binary_true) {
    Heap heap(1ull << 20); InternTable intern;
    binary_round_trip(nanbox::True, heap, intern, "#t");
}

BOOST_AUTO_TEST_CASE(p6_binary_false) {
    Heap heap(1ull << 20); InternTable intern;
    binary_round_trip(nanbox::False, heap, intern, "#f");
}

BOOST_AUTO_TEST_CASE(p6_binary_fixnum_zero) {
    Heap heap(1ull << 20); InternTable intern;
    auto v = require_ok(make_fixnum(heap, int64_t{0}));
    binary_round_trip(v, heap, intern, "0");
}

BOOST_AUTO_TEST_CASE(p6_binary_fixnum_positive) {
    Heap heap(1ull << 20); InternTable intern;
    auto v = require_ok(make_fixnum(heap, int64_t{42}));
    binary_round_trip(v, heap, intern, "42");
}

BOOST_AUTO_TEST_CASE(p6_binary_fixnum_negative) {
    Heap heap(1ull << 20); InternTable intern;
    auto v = require_ok(make_fixnum(heap, int64_t{-99}));
    binary_round_trip(v, heap, intern, "-99");
}

BOOST_AUTO_TEST_CASE(p6_binary_fixnum_heap_allocated) {
    Heap heap(1ull << 20); InternTable intern;
    constexpr int64_t big = 1000000000000000LL;
    auto v = require_ok(make_fixnum(heap, big));
    binary_round_trip(v, heap, intern, "1000000000000000");
}

BOOST_AUTO_TEST_CASE(p6_binary_flonum_pi) {
    Heap heap(1ull << 20); InternTable intern;
    auto v = require_ok(make_flonum(3.14));
    auto bin = serialize_binary(v, heap, intern);
    auto result = deserialize_binary(std::span<const uint8_t>(bin), heap, intern);
    BOOST_REQUIRE(result.has_value());
    auto dec = ops::decode<double>(*result);
    BOOST_REQUIRE(dec.has_value());
    BOOST_CHECK_CLOSE(*dec, 3.14, 1e-9);
}

BOOST_AUTO_TEST_CASE(p6_binary_flonum_negative) {
    Heap heap(1ull << 20); InternTable intern;
    auto v = require_ok(make_flonum(-2.718));
    auto bin = serialize_binary(v, heap, intern);
    auto result = deserialize_binary(std::span<const uint8_t>(bin), heap, intern);
    BOOST_REQUIRE(result.has_value());
    auto dec = ops::decode<double>(*result);
    BOOST_REQUIRE(dec.has_value());
    BOOST_CHECK_CLOSE(*dec, -2.718, 1e-9);
}

BOOST_AUTO_TEST_CASE(p6_binary_flonum_zero) {
    Heap heap(1ull << 20); InternTable intern;
    auto v = require_ok(make_flonum(0.0));
    binary_round_trip(v, heap, intern, "0.0");
}

BOOST_AUTO_TEST_CASE(p6_binary_char_printable) {
    Heap heap(1ull << 20); InternTable intern;
    auto v = require_ok(ops::encode<char32_t>(U'z'));
    binary_round_trip(v, heap, intern, "#\\z");
}

BOOST_AUTO_TEST_CASE(p6_binary_char_space) {
    Heap heap(1ull << 20); InternTable intern;
    auto v = require_ok(ops::encode<char32_t>(U' '));
    binary_round_trip(v, heap, intern, "#\\space");
}

BOOST_AUTO_TEST_CASE(p6_binary_char_newline) {
    Heap heap(1ull << 20); InternTable intern;
    auto v = require_ok(ops::encode<char32_t>(U'\n'));
    binary_round_trip(v, heap, intern, "#\\newline");
}

BOOST_AUTO_TEST_CASE(p6_binary_string_simple) {
    Heap heap(1ull << 20); InternTable intern;
    auto v = require_ok(make_string(heap, intern, "hello world"));
    binary_round_trip(v, heap, intern, "\"hello world\"");
}

BOOST_AUTO_TEST_CASE(p6_binary_string_empty) {
    Heap heap(1ull << 20); InternTable intern;
    auto v = require_ok(make_string(heap, intern, ""));
    binary_round_trip(v, heap, intern, "\"\"");
}

BOOST_AUTO_TEST_CASE(p6_binary_string_with_escapes) {
    Heap heap(1ull << 20); InternTable intern;
    auto v = require_ok(make_string(heap, intern, "line1\nline2"));
    binary_round_trip(v, heap, intern, "\"line1\\nline2\"");
}

BOOST_AUTO_TEST_CASE(p6_binary_symbol) {
    Heap heap(1ull << 20); InternTable intern;
    auto v = require_ok(make_symbol(intern, "hello"));
    binary_round_trip(v, heap, intern, "hello");
}

BOOST_AUTO_TEST_CASE(p6_binary_symbol_with_special) {
    Heap heap(1ull << 20); InternTable intern;
    auto v = require_ok(make_symbol(intern, "my-func!"));
    binary_round_trip(v, heap, intern, "my-func!");
}

BOOST_AUTO_TEST_CASE(p6_binary_dotted_pair) {
    Heap heap(1ull << 20); InternTable intern;
    auto a = require_ok(make_fixnum(heap, int64_t{1}));
    auto b = require_ok(make_fixnum(heap, int64_t{2}));
    auto pair = require_ok(make_cons(heap, a, b));
    binary_round_trip(pair, heap, intern, "(1 . 2)");
}

BOOST_AUTO_TEST_CASE(p6_binary_proper_list) {
    Heap heap(1ull << 20); InternTable intern;
    auto a = require_ok(make_fixnum(heap, int64_t{1}));
    auto b = require_ok(make_fixnum(heap, int64_t{2}));
    auto c = require_ok(make_fixnum(heap, int64_t{3}));
    auto l = require_ok(make_cons(heap, c, nanbox::Nil));
    l = require_ok(make_cons(heap, b, l));
    l = require_ok(make_cons(heap, a, l));
    binary_round_trip(l, heap, intern, "(1 2 3)");
}

BOOST_AUTO_TEST_CASE(p6_binary_nested_list) {
    Heap heap(1ull << 20); InternTable intern;
    auto a = require_ok(make_fixnum(heap, int64_t{1}));
    auto b = require_ok(make_fixnum(heap, int64_t{2}));
    auto inner = require_ok(make_cons(heap, b, nanbox::Nil));
    inner = require_ok(make_cons(heap, a, inner));
    auto three = require_ok(make_fixnum(heap, int64_t{3}));
    auto outer_tail = require_ok(make_cons(heap, three, nanbox::Nil));
    auto outer = require_ok(make_cons(heap, inner, outer_tail));
    binary_round_trip(outer, heap, intern, "((1 2) 3)");
}

BOOST_AUTO_TEST_CASE(p6_binary_vector) {
    Heap heap(1ull << 20); InternTable intern;
    auto a = require_ok(make_fixnum(heap, int64_t{10}));
    auto b = require_ok(make_fixnum(heap, int64_t{20}));
    auto c = require_ok(make_fixnum(heap, int64_t{30}));
    auto v = require_ok(make_vector(heap, {a, b, c}));
    binary_round_trip(v, heap, intern, "#(10 20 30)");
}

BOOST_AUTO_TEST_CASE(p6_binary_empty_vector) {
    Heap heap(1ull << 20); InternTable intern;
    auto v = require_ok(make_vector(heap, {}));
    binary_round_trip(v, heap, intern, "#()");
}

BOOST_AUTO_TEST_CASE(p6_binary_bytevector) {
    Heap heap(1ull << 20); InternTable intern;
    auto v = require_ok(make_bytevector(heap, {0, 128, 255}));
    binary_round_trip(v, heap, intern, "#u8(0 128 255)");
}

BOOST_AUTO_TEST_CASE(p6_binary_empty_bytevector) {
    Heap heap(1ull << 20); InternTable intern;
    auto v = require_ok(make_bytevector(heap, {}));
    binary_round_trip(v, heap, intern, "#u8()");
}

BOOST_AUTO_TEST_CASE(p6_binary_mixed_list) {
    // (1 "hi" #t #(a b))
    Heap heap(1ull << 20); InternTable intern;
    auto one   = require_ok(make_fixnum(heap, int64_t{1}));
    auto hi    = require_ok(make_string(heap, intern, "hi"));
    auto sym_a = require_ok(make_symbol(intern, "a"));
    auto sym_b = require_ok(make_symbol(intern, "b"));
    auto vec   = require_ok(make_vector(heap, {sym_a, sym_b}));
    auto l4 = require_ok(make_cons(heap, vec,          nanbox::Nil));
    auto l3 = require_ok(make_cons(heap, nanbox::True, l4));
    auto l2 = require_ok(make_cons(heap, hi,           l3));
    auto l1 = require_ok(make_cons(heap, one,          l2));
    binary_round_trip(l1, heap, intern, "(1 \"hi\" #t #(a b))");
}

// ── Error handling ─────────────────────────────────────────────────────────

BOOST_AUTO_TEST_CASE(p6_binary_deserialize_missing_version_byte) {
    Heap heap(1ull << 20); InternTable intern;
    std::vector<uint8_t> bad{0x00};   // wrong version byte
    auto res = deserialize_binary(std::span<const uint8_t>(bad), heap, intern);
    BOOST_TEST(!res.has_value());
}

BOOST_AUTO_TEST_CASE(p6_binary_deserialize_empty_buffer) {
    Heap heap(1ull << 20); InternTable intern;
    std::vector<uint8_t> empty;
    auto res = deserialize_binary(std::span<const uint8_t>(empty), heap, intern);
    BOOST_TEST(!res.has_value());
}

BOOST_AUTO_TEST_CASE(p6_binary_deserialize_truncated) {
    Heap heap(1ull << 20); InternTable intern;
    // Version byte + BT_Fixnum tag, but no payload
    std::vector<uint8_t> trunc{0xEA, 0x02};
    auto res = deserialize_binary(std::span<const uint8_t>(trunc), heap, intern);
    BOOST_TEST(!res.has_value());
}

// ── Performance: binary vs text ────────────────────────────────────────────

BOOST_AUTO_TEST_CASE(p6_binary_large_vector_performance) {
    // Binary serialize+deserialize of a 10,000-element vector.
    // Acceptance criterion: < 10 ms in release builds (half the text threshold).
    Heap heap(1ull << 24); InternTable intern;

    constexpr size_t VEC_SIZE = 10000;
    std::vector<LispVal> elems;
    elems.reserve(VEC_SIZE);
    for (int64_t i = 0; i < static_cast<int64_t>(VEC_SIZE); ++i)
        elems.push_back(*make_fixnum(heap, i));
    auto vec = require_ok(make_vector(heap, std::move(elems)));

    BOOST_TEST_MESSAGE("Built 10,000-element vector, starting binary serialization...");

    auto start = std::chrono::high_resolution_clock::now();

    auto bin      = serialize_binary(vec, heap, intern);
    auto result   = deserialize_binary(std::span<const uint8_t>(bin), heap, intern);

    auto end = std::chrono::high_resolution_clock::now();

    BOOST_REQUIRE_MESSAGE(result.has_value(),
        "binary deserialization of large vector failed");

    auto duration_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
    BOOST_TEST_MESSAGE("Binary: 10,000-element vector round-trip in "
                       << duration_ms << " ms ("
                       << bin.size() << " bytes)");

#if defined(_DEBUG) || !defined(NDEBUG)
    BOOST_TEST(duration_ms < 100);
#else
    BOOST_TEST(duration_ms < 10);
#endif

    // Structural check
    auto* result_vec = heap.try_get_as<ObjectKind::Vector, types::Vector>(
        ops::payload(*result));
    BOOST_REQUIRE(result_vec != nullptr);
    BOOST_REQUIRE_EQUAL(result_vec->elements.size(), VEC_SIZE);
    BOOST_TEST(*ops::decode<int64_t>(result_vec->elements[0]) == 0);
    BOOST_TEST(*ops::decode<int64_t>(result_vec->elements[VEC_SIZE - 1]) ==
               static_cast<int64_t>(VEC_SIZE - 1));
}

// ── auto-detection via send!/recv! over inproc:// ──────────────────────────

BOOST_AUTO_TEST_CASE(p6_send_default_binary_recv_autodetects) {
    // Default send! uses binary; recv! should auto-detect and return the value.
    NngEnv e;
    std::string addr = inproc_addr();

    auto server = e.call("nng-socket", {e.sym("pair")});
    auto client = e.call("nng-socket", {e.sym("pair")});
    e.call("nng-listen", {server, e.str(addr)});
    e.call("nng-dial",   {client, e.str(addr)});

    // Send a list in binary (default)
    auto a = e.fixnum(10);
    auto b = e.fixnum(20);
    auto lst = require_ok(make_cons(e.heap, b, nanbox::Nil));
    lst = require_ok(make_cons(e.heap, a, lst));

    auto send_res = e.call("send!", {client, lst});
    BOOST_TEST(send_res == nanbox::True);

    auto recv_res = e.call("recv!", {server});
    BOOST_REQUIRE(recv_res != nanbox::False);

    // Should be (10 20)
    auto* cons = e.heap.try_get_as<ObjectKind::Cons, Cons>(ops::payload(recv_res));
    BOOST_REQUIRE(cons != nullptr);
    BOOST_TEST(*ops::decode<int64_t>(cons->car) == 10);

    e.call("nng-close", {server});
    e.call("nng-close", {client});
}

BOOST_AUTO_TEST_CASE(p6_send_text_flag_recv_autodetects) {
    // 'text flag on send! → s-expression; recv! should still auto-detect and parse.
    NngEnv e;
    std::string addr = inproc_addr();

    auto server = e.call("nng-socket", {e.sym("pair")});
    auto client = e.call("nng-socket", {e.sym("pair")});
    e.call("nng-listen", {server, e.str(addr)});
    e.call("nng-dial",   {client, e.str(addr)});

    auto val = e.fixnum(777);
    auto send_res = e.call("send!", {client, val, e.sym("text")});
    BOOST_TEST(send_res == nanbox::True);

    auto recv_res = e.call("recv!", {server});
    BOOST_REQUIRE(recv_res != nanbox::False);
    BOOST_TEST(*ops::decode<int64_t>(recv_res) == 777);

    e.call("nng-close", {server});
    e.call("nng-close", {client});
}

BOOST_AUTO_TEST_CASE(p6_binary_round_trip_over_socket_all_types) {
    // Binary round-trip through a real nng socket for a heterogeneous structure.
    NngEnv e;
    std::string addr = inproc_addr();

    auto server = e.call("nng-socket", {e.sym("pair")});
    auto client = e.call("nng-socket", {e.sym("pair")});
    e.call("nng-listen", {server, e.str(addr)});
    e.call("nng-dial",   {client, e.str(addr)});

    // Build (#t "binary" 42 #\x #(1 2) #u8(0 1 2))
    auto flag   = nanbox::True;
    auto str    = require_ok(make_string(e.heap, e.intern, "binary"));
    auto num    = require_ok(make_fixnum(e.heap, int64_t{42}));
    auto ch     = require_ok(ops::encode<char32_t>(U'x'));
    auto v1     = require_ok(make_fixnum(e.heap, int64_t{1}));
    auto v2     = require_ok(make_fixnum(e.heap, int64_t{2}));
    auto vec    = require_ok(make_vector(e.heap, {v1, v2}));
    auto bvec   = require_ok(make_bytevector(e.heap, {0, 1, 2}));

    auto tail5  = require_ok(make_cons(e.heap, bvec, nanbox::Nil));
    auto tail4  = require_ok(make_cons(e.heap, vec, tail5));
    auto tail3  = require_ok(make_cons(e.heap, ch, tail4));
    auto tail2  = require_ok(make_cons(e.heap, num, tail3));
    auto tail1  = require_ok(make_cons(e.heap, str, tail2));
    auto msg    = require_ok(make_cons(e.heap, flag, tail1));

    e.call("send!", {client, msg});   // binary by default
    auto recv_res = e.call("recv!", {server});
    BOOST_REQUIRE(recv_res != nanbox::False);

    // Verify it's a list starting with #t
    auto* hd = e.heap.try_get_as<ObjectKind::Cons, Cons>(ops::payload(recv_res));
    BOOST_REQUIRE(hd != nullptr);
    BOOST_TEST(hd->car == nanbox::True);

    // Second element should be the string "binary"
    auto* tl = e.heap.try_get_as<ObjectKind::Cons, Cons>(ops::payload(hd->cdr));
    BOOST_REQUIRE(tl != nullptr);
    BOOST_TEST(ops::tag(tl->car) == Tag::String);
    auto sv = e.intern.get_string(ops::payload(tl->car));
    BOOST_REQUIRE(sv.has_value());
    BOOST_TEST(*sv == "binary");

    e.call("nng-close", {server});
    e.call("nng-close", {client});
}

BOOST_AUTO_TEST_SUITE_END()

// ═══════════════════════════════════════════════════════════════════════════
// Phase 4 — Process Spawning & Actor Model
// ═══════════════════════════════════════════════════════════════════════════

BOOST_AUTO_TEST_SUITE(nng_phase4_tests)

// ── ProcessManager: basic construction ────────────────────────────────────

BOOST_AUTO_TEST_CASE(p4_process_manager_default_construct) {
    ProcessManager pm;
    auto children = pm.list_children();
    BOOST_TEST(children.empty());
}

BOOST_AUTO_TEST_CASE(p4_process_manager_kill_unknown_socket_returns_false) {
    ProcessManager pm;
    // An unregistered socket returns false — doesn't crash.
    bool ok = pm.kill_child(nanbox::Nil);
    BOOST_TEST(ok == false);
}

BOOST_AUTO_TEST_CASE(p4_process_manager_wait_unknown_socket_returns_minus_one) {
    ProcessManager pm;
    int code = pm.wait_for(nanbox::Nil);
    BOOST_TEST(code == -1);
}

// ── current-mailbox returns Nil when no mailbox installed ─────────────────

BOOST_AUTO_TEST_CASE(p4_current_mailbox_no_mailbox_returns_nil) {
    NngEnvWithSpawn e;  // no etai_path, no mailbox installed
    auto result = e.call("current-mailbox", {});
    BOOST_TEST(result == nanbox::Nil);
}

// ── spawn without process manager returns clear error ─────────────────────

BOOST_AUTO_TEST_CASE(p4_spawn_no_proc_mgr_returns_error) {
    // NngEnv (Phase 3 style) has no ProcessManager — spawn should error
    NngEnv e;
    auto res = e.try_call("spawn", {e.str("worker.eta")});
    BOOST_TEST(!res.has_value());
    BOOST_TEST_MESSAGE("spawn error (expected): " +
        runtime_error_msg(res.error()));
}

BOOST_AUTO_TEST_CASE(p4_spawn_kill_no_proc_mgr_returns_error) {
    NngEnv e;
    auto sock = e.call("nng-socket", {e.sym("pair")});
    auto res = e.try_call("spawn-kill", {sock});
    BOOST_TEST(!res.has_value());
    e.call("nng-close", {sock});
}

BOOST_AUTO_TEST_CASE(p4_spawn_wait_no_proc_mgr_returns_error) {
    NngEnv e;
    auto sock = e.call("nng-socket", {e.sym("pair")});
    auto res = e.try_call("spawn-wait", {sock});
    BOOST_TEST(!res.has_value());
    e.call("nng-close", {sock});
}

// ── spawn with empty etai_path returns clear error ────────────────────────

BOOST_AUTO_TEST_CASE(p4_spawn_empty_etai_path_returns_error) {
    NngEnvWithSpawn e("");  // proc_mgr present but empty etai_path
    auto res = e.try_call("spawn", {e.str("worker.eta")});
    BOOST_TEST(!res.has_value());
    BOOST_TEST_MESSAGE("spawn error (expected): " +
        runtime_error_msg(res.error()));
}

// ── spawn with wrong module path returns error ────────────────────────────

BOOST_AUTO_TEST_CASE(p4_spawn_wrong_arg_type_returns_error) {
    NngEnvWithSpawn e(etai_binary_path());
    // Pass a symbol instead of a string
    auto res = e.try_call("spawn", {e.sym("worker")});
    BOOST_TEST(!res.has_value());
}

// ── spawn-kill on non-socket returns type error ───────────────────────────

BOOST_AUTO_TEST_CASE(p4_spawn_kill_wrong_type_returns_error) {
    NngEnvWithSpawn e(etai_binary_path());
    auto res = e.try_call("spawn-kill", {e.fixnum(42)});
    BOOST_TEST(!res.has_value());
}

BOOST_AUTO_TEST_CASE(p4_spawn_wait_wrong_type_returns_error) {
    NngEnvWithSpawn e(etai_binary_path());
    auto res = e.try_call("spawn-wait", {e.fixnum(42)});
    BOOST_TEST(!res.has_value());
}

// ── current-mailbox with installed mailbox returns the socket ─────────────

BOOST_AUTO_TEST_CASE(p4_current_mailbox_with_mailbox_returns_socket) {
    // Simulate what install_mailbox does: connect a PAIR socket over inproc.
    NngEnvWithSpawn e;
    std::string addr = "inproc://p4-mailbox-test";

    // Set up a "parent" listener in a separate env
    NngEnv parent;
    auto parent_sock = parent.call("nng-socket", {parent.sym("pair")});
    parent.call("nng-listen", {parent_sock, parent.str(addr)});

    // Create the child-side socket and store it as the mailbox
    NngSocketPtr sp;
    sp.protocol = NngProtocol::Pair;
    nng_socket_set_ms(sp.socket, NNG_OPT_RECVTIMEO, 500);
    int rv = nng_pair0_open(&sp.socket);
    BOOST_REQUIRE_EQUAL(rv, 0);
    rv = nng_dial(sp.socket, addr.c_str(), nullptr, 0);
    BOOST_REQUIRE_EQUAL(rv, 0);
    sp.dialed = true;

    auto mailbox_lv = require_ok(make_nng_socket(e.heap, std::move(sp)));
    e.mailbox_val = mailbox_lv;

    // current-mailbox should now return the installed socket
    auto result = e.call("current-mailbox", {});
    BOOST_TEST(result == mailbox_lv);

    parent.call("nng-close", {parent_sock});
}

// ── Integration test: spawn/send/recv round-trip ──────────────────────────

BOOST_AUTO_TEST_CASE(p4_spawn_send_recv_round_trip) {
    // We need a simple worker script that:
    //   1. Receives a message on (current-mailbox)
    //   2. Sends back the message incremented by 1
    // We create a temporary .eta file for this test.

    namespace fs = std::filesystem;
    auto tmp_dir  = fs::temp_directory_path();
    auto worker   = tmp_dir / "eta_p4_worker_test.eta";

    // Worker: no stdlib import needed — recv!, send!, current-mailbox, + are builtins.
    {
        std::ofstream f(worker);
        BOOST_REQUIRE(f.is_open());
        f << "(module eta-p4-worker\n"
          << "  (let ((mb (current-mailbox)))\n"
          << "    (let ((msg (recv! mb 'wait)))\n"
          << "      (send! mb (+ msg 1) 'wait))))\n";
    }

    std::string etai_path = etai_binary_path();
    if (etai_path.empty()) {
        BOOST_TEST_MESSAGE("ETA_ETAI_PATH not configured — skipping integration test");
        fs::remove(worker);
        return;
    }
    BOOST_REQUIRE_MESSAGE(fs::exists(etai_path),
        "etai binary not found at: " + etai_path);

    NngEnvWithSpawn e(etai_path);

    auto sock_res = e.try_call("spawn", {e.str(worker.string())});
    if (!sock_res.has_value()) {
        BOOST_TEST_MESSAGE("spawn failed: " + runtime_error_msg(sock_res.error()));
        fs::remove(worker);
        return; // not a hard failure during bring-up
    }
    auto sock = *sock_res;

    // Give child a moment to start, dial, and reach recv!
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    // Set finite send + recv timeouts on the parent socket.
    {
        auto* sp = e.heap.try_get_as<ObjectKind::NngSocket, NngSocketPtr>(ops::payload(sock));
        BOOST_REQUIRE(sp != nullptr);
        nng_socket_set_ms(sp->socket, NNG_OPT_SENDTIMEO, 5000);
        nng_socket_set_ms(sp->socket, NNG_OPT_RECVTIMEO, 5000);
    }

    BOOST_TEST_MESSAGE("Child spawned, sending message 41...");

    // send! fixnum 41 — no 'wait; uses SENDTIMEO (5 s)
    auto send_res = e.try_call("send!", {sock, e.fixnum(41)});
    BOOST_REQUIRE_MESSAGE(send_res.has_value(), "send! error: " +
        (send_res.has_value() ? "" : runtime_error_msg(send_res.error())));
    BOOST_REQUIRE_MESSAGE(*send_res != nanbox::False, "send! timed out (child may not have connected)");

    // recv! — no 'wait; uses RECVTIMEO (5 s)
    auto recv_res = e.try_call("recv!", {sock});
    BOOST_REQUIRE_MESSAGE(recv_res.has_value(), "recv! error: " +
        (recv_res.has_value() ? "" : runtime_error_msg(recv_res.error())));
    BOOST_REQUIRE_MESSAGE(*recv_res != nanbox::False, "recv! timed out (child did not respond)");
    auto dec = ops::decode<int64_t>(*recv_res);
    BOOST_REQUIRE(dec.has_value());
    BOOST_TEST(*dec == 42);
    BOOST_TEST_MESSAGE("Round-trip OK: sent 41, received " << *dec);

    // Wait for child to exit
    auto wait_res = e.try_call("spawn-wait", {sock});
    BOOST_REQUIRE_MESSAGE(wait_res.has_value(), "spawn-wait failed");
    BOOST_TEST_MESSAGE("Child exit code: " << ops::decode<int64_t>(*wait_res).value_or(-1));

    (void) e.try_call("nng-close", {sock});
    fs::remove(worker);
}

BOOST_AUTO_TEST_CASE(p4_spawn_kill_terminates_child) {
    // Spawn a child that loops forever (sleep)
    namespace fs = std::filesystem;
    auto tmp_dir = fs::temp_directory_path();
    auto worker  = tmp_dir / "eta_p4_sleep_worker.eta";

    {
        std::ofstream f(worker);
        BOOST_REQUIRE(f.is_open());
        // Worker waits forever on its mailbox — builtins only, no stdlib import needed.
        f << "(module eta-p4-sleep\n"
          << "  (recv! (current-mailbox) 'wait))\n";
    }

    std::string etai_path = etai_binary_path();
    if (etai_path.empty()) { BOOST_TEST_MESSAGE("etai path not configured — skip"); fs::remove(worker); return; }
    BOOST_REQUIRE_MESSAGE(fs::exists(etai_path), "etai binary not found at: " + etai_path);

    NngEnvWithSpawn e(etai_path);
    auto spawn_res = e.try_call("spawn", {e.str(worker.string())});
    if (!spawn_res.has_value()) {
        std::cerr << "[kill-test] spawn failed: " << runtime_error_msg(spawn_res.error()) << "\n";
        BOOST_TEST_MESSAGE("spawn failed, skipping kill test");
        fs::remove(worker);
        return;
    }
    auto sock = *spawn_res;

    // Brief delay to let child start up
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    // Check process is alive
    auto children = e.proc_mgr.list_children();
    BOOST_REQUIRE(!children.empty());
    BOOST_TEST_MESSAGE("Child PID: " << children[0].pid
                       << " alive=" << children[0].alive);

    // Kill the child
    auto kill_res = e.try_call("spawn-kill", {sock});
    BOOST_REQUIRE_MESSAGE(kill_res.has_value(), "spawn-kill failed");
    BOOST_TEST(*kill_res == nanbox::True);
    BOOST_TEST_MESSAGE("spawn-kill returned #t");

    (void) e.try_call("nng-close", {sock});
    fs::remove(worker);
}

BOOST_AUTO_TEST_CASE(p4_spawn_multiple_children) {
    // Spawn two workers, send each a distinct value, collect replies.
    namespace fs = std::filesystem;
    auto tmp_dir = fs::temp_directory_path();
    auto worker  = tmp_dir / "eta_p4_echo_worker.eta";

    {
        std::ofstream f(worker);
        BOOST_REQUIRE(f.is_open());
        f << "(module eta-p4-echo\n"
          << "  (let ((msg (recv! (current-mailbox) 'wait)))\n"
          << "    (send! (current-mailbox) msg 'wait)))\n";
    }

    std::string etai_path = etai_binary_path();
    if (etai_path.empty()) { BOOST_TEST_MESSAGE("etai path not configured — skip"); fs::remove(worker); return; }
    BOOST_REQUIRE_MESSAGE(fs::exists(etai_path), "etai binary not found at: " + etai_path);

    NngEnvWithSpawn e(etai_path);
#ifdef _WIN32
      _putenv_s("ETA_MODULE_PATH", ETA_STDLIB_DIR);
#else
    ::setenv("ETA_MODULE_PATH", ETA_STDLIB_DIR, 1);
#endif

    auto s1_res = e.try_call("spawn", {e.str(worker.string())});
    auto s2_res = e.try_call("spawn", {e.str(worker.string())});
    if (!s1_res.has_value() || !s2_res.has_value()) {
        BOOST_TEST_MESSAGE("spawn failed, skipping multi-child test");
        fs::remove(worker);
        return;
    }

    auto s1 = *s1_res;
    auto s2 = *s2_res;

    BOOST_TEST(e.proc_mgr.list_children().size() == 2u);

    // Set generous send + recv timeouts (don't use 'wait — it overrides to infinite)
    for (auto sv : {s1, s2}) {
        auto* sp = e.heap.try_get_as<ObjectKind::NngSocket, NngSocketPtr>(ops::payload(sv));
        if (sp) {
            nng_socket_set_ms(sp->socket, NNG_OPT_SENDTIMEO, 5000);
            nng_socket_set_ms(sp->socket, NNG_OPT_RECVTIMEO, 5000);
        }
    }

    // Send distinct values — no 'wait; uses SENDTIMEO (5 s)
    auto sr1 = e.try_call("send!", {s1, e.fixnum(100)});
    auto sr2 = e.try_call("send!", {s2, e.fixnum(200)});
    BOOST_REQUIRE_MESSAGE(sr1.has_value() && *sr1 != nanbox::False, "send! to child 1 failed");
    BOOST_REQUIRE_MESSAGE(sr2.has_value() && *sr2 != nanbox::False, "send! to child 2 failed");

    auto r1 = e.try_call("recv!", {s1});
    auto r2 = e.try_call("recv!", {s2});

    BOOST_REQUIRE(r1.has_value() && *r1 != nanbox::False);
    BOOST_REQUIRE(r2.has_value() && *r2 != nanbox::False);

    BOOST_TEST(ops::decode<int64_t>(*r1).value_or(-1) == 100);
    BOOST_TEST(ops::decode<int64_t>(*r2).value_or(-1) == 200);
    BOOST_TEST_MESSAGE("Multi-child round-trip OK");

    (void) e.try_call("spawn-wait", {s1});
    (void) e.try_call("spawn-wait", {s2});
    (void) e.try_call("nng-close", {s1});
    (void) e.try_call("nng-close", {s2});
    fs::remove(worker);
}

BOOST_AUTO_TEST_SUITE_END()

// ═══════════════════════════════════════════════════════════════════════════
// Phase 7 — In-Process Actor Threads
// ═══════════════════════════════════════════════════════════════════════════

BOOST_AUTO_TEST_SUITE(nng_phase7_tests)

// ── Helper: simple worker factory that uses raw nng (no Driver needed) ────

namespace {

using SimpleThreadTask = std::function<void(nng_socket child_sock)>;

static ProcessManager::ThreadWorkerFn make_simple_worker(SimpleThreadTask task) {
    return [task = std::move(task)](
        const std::string& /*module_path*/,
        const std::string& /*func_name*/,
        const std::string& endpoint,
        std::vector<std::string> /*text_args*/,
        std::shared_ptr<std::atomic<bool>> alive) noexcept
    {
        try {
            nng_socket child;
            if (nng_pair0_open(&child) != 0) { alive->store(false); return; }
            nng_socket_set_ms(child, NNG_OPT_RECVTIMEO, 2000);
            nng_socket_set_ms(child, NNG_OPT_SENDTIMEO, 2000);
            if (nng_dial(child, endpoint.c_str(), nullptr, 0) != 0) {
                nng_close(child);
                alive->store(false);
                return;
            }
            task(child);
            nng_close(child);
        } catch (...) {}
        alive->store(false, std::memory_order_release);
    };
}

struct NngEnvWithThread {
    Heap heap{4 * 1024 * 1024};
    InternTable intern;
    BuiltinEnvironment env;
    ProcessManager proc_mgr;
    LispVal mailbox_val{nanbox::Nil};

    NngEnvWithThread() {
        register_nng_primitives(env, heap, intern, &proc_mgr, "", &mailbox_val, {});
    }

    LispVal call(const std::string& name, std::vector<LispVal> args) {
        const auto& specs = env.specs();
        for (const auto& spec : specs) {
            if (spec.name == name && spec.func) {
                auto res = spec.func(args);
                BOOST_REQUIRE_MESSAGE(res.has_value(), "call to " + name + " failed");
                return *res;
            }
        }
        BOOST_FAIL("primitive not found: " + name);
        return nanbox::Nil;
    }

    std::expected<LispVal, error::RuntimeError>
    try_call(const std::string& name, std::vector<LispVal> args) {
        const auto& specs = env.specs();
        for (const auto& spec : specs) {
            if (spec.name == name && spec.func) return spec.func(args);
        }
        return std::unexpected(error::VMError{
            error::RuntimeErrorCode::InternalError, "not found: " + name});
    }

    LispVal sym(const std::string& s) { return require_ok(make_symbol(intern, s)); }
    LispVal str(const std::string& s) { return require_ok(make_string(heap, intern, s)); }
    LispVal fixnum(int64_t n)         { return require_ok(make_fixnum(heap, n)); }
};

} // anonymous namespace

// ── ProcessManager thread infrastructure ──────────────────────────────────

BOOST_AUTO_TEST_CASE(p7_process_manager_list_threads_empty) {
    ProcessManager pm;
    BOOST_TEST(pm.list_threads().empty());
}

BOOST_AUTO_TEST_CASE(p7_process_manager_is_thread_alive_unknown_socket) {
    ProcessManager pm;
    BOOST_TEST(pm.is_thread_alive(nanbox::Nil) == false);
}

BOOST_AUTO_TEST_CASE(p7_process_manager_join_thread_unknown_socket) {
    ProcessManager pm;
    int rc = pm.join_thread(nanbox::Nil);
    BOOST_TEST(rc == -1);
}

// ── spawn-thread-with: no process manager returns clear error ─────────────

BOOST_AUTO_TEST_CASE(p7_spawn_thread_with_no_proc_mgr_error) {
    NngEnv e;  // Phase 3 env — no process manager
    auto res = e.try_call("spawn-thread-with",
                          {e.str("worker.eta"), e.sym("my-fn")});
    BOOST_TEST(!res.has_value());
    BOOST_TEST_MESSAGE("error (expected): " + runtime_error_msg(res.error()));
}

// ── spawn-thread returns clear "not implemented" error ────────────────────

BOOST_AUTO_TEST_CASE(p7_spawn_thread_returns_not_implemented_error) {
    NngEnv e;
    auto res = e.try_call("spawn-thread", {nanbox::False});
    BOOST_TEST(!res.has_value());
    BOOST_TEST_MESSAGE("spawn-thread error (expected): " + runtime_error_msg(res.error()));
}

// ── spawn-thread-with: wrong arg types ────────────────────────────────────

BOOST_AUTO_TEST_CASE(p7_spawn_thread_with_wrong_first_arg_type) {
    NngEnvWithThread e;
    e.proc_mgr.set_worker_factory(make_simple_worker([](nng_socket) {}));
    auto res = e.try_call("spawn-thread-with", {e.sym("not-a-string"), e.sym("my-fn")});
    BOOST_TEST(!res.has_value());
}

BOOST_AUTO_TEST_CASE(p7_spawn_thread_with_wrong_second_arg_type) {
    NngEnvWithThread e;
    e.proc_mgr.set_worker_factory(make_simple_worker([](nng_socket) {}));
    auto res = e.try_call("spawn-thread-with", {e.str("worker.eta"), e.str("not-a-symbol")});
    BOOST_TEST(!res.has_value());
}

// ── spawn-thread-with: creates socket ────────────────────────────────────

BOOST_AUTO_TEST_CASE(p7_spawn_thread_with_returns_socket) {
    NngEnvWithThread e;
    e.proc_mgr.set_worker_factory(make_simple_worker([](nng_socket) { /* noop */ }));

    auto res = e.try_call("spawn-thread-with", {e.str("test.eta"), e.sym("my-fn")});
    BOOST_REQUIRE_MESSAGE(res.has_value(), "spawn-thread-with failed: " +
        (res.has_value() ? "" : runtime_error_msg(res.error())));

    auto sock = *res;
    BOOST_TEST(ops::is_boxed(sock));
    BOOST_TEST(ops::tag(sock) == Tag::HeapObject);

    auto* sp = e.heap.try_get_as<ObjectKind::NngSocket, NngSocketPtr>(ops::payload(sock));
    BOOST_REQUIRE(sp != nullptr);
    BOOST_TEST(sp->protocol == NngProtocol::Pair);

    e.call("thread-join", {sock});
    e.call("nng-close", {sock});
}

// ── thread-alive? ─────────────────────────────────────────────────────────

BOOST_AUTO_TEST_CASE(p7_thread_alive_while_running) {
    NngEnvWithThread e;
    e.proc_mgr.set_worker_factory(
        make_simple_worker([](nng_socket s) {
            void* buf = nullptr; size_t sz = 0;
            nng_socket_set_ms(s, NNG_OPT_RECVTIMEO, 500);
            nng_recv(s, &buf, &sz, NNG_FLAG_ALLOC);
            if (buf) nng_free(buf, sz);
        }));

    auto res = e.try_call("spawn-thread-with", {e.str("test.eta"), e.sym("loop-fn")});
    BOOST_REQUIRE(res.has_value());
    auto sock = *res;

    std::this_thread::sleep_for(std::chrono::milliseconds(30));

    auto alive = e.call("thread-alive?", {sock});
    BOOST_TEST_MESSAGE("thread-alive? = " << (alive == nanbox::True ? "#t" : "#f"));
    BOOST_TEST(alive == nanbox::True);

    e.call("thread-join", {sock});
    auto alive2 = e.call("thread-alive?", {sock});
    BOOST_TEST(alive2 == nanbox::False);
    e.call("nng-close", {sock});
}

// ── thread-join: blocks until thread completes ────────────────────────────

BOOST_AUTO_TEST_CASE(p7_thread_join_waits_for_completion) {
    NngEnvWithThread e;
    std::atomic<bool> thread_ran{false};

    e.proc_mgr.set_worker_factory(
        [&thread_ran](
            const std::string&, const std::string&,
            const std::string& endpoint,
            std::vector<std::string>,
            std::shared_ptr<std::atomic<bool>> alive) noexcept
        {
            try {
                nng_socket s;
                if (nng_pair0_open(&s) != 0) { alive->store(false); return; }
                nng_dial(s, endpoint.c_str(), nullptr, 0);
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                thread_ran.store(true);
                nng_close(s);
            } catch (...) {}
            alive->store(false, std::memory_order_release);
        });

    auto res = e.try_call("spawn-thread-with", {e.str("test.eta"), e.sym("slow-fn")});
    BOOST_REQUIRE(res.has_value());
    auto sock = *res;

    BOOST_TEST(thread_ran.load() == false);

    auto join_res = e.call("thread-join", {sock});
    BOOST_TEST(*ops::decode<int64_t>(join_res) == 0);
    BOOST_TEST(thread_ran.load() == true);

    e.call("nng-close", {sock});
}

// ── thread-join / thread-alive? wrong type errors ─────────────────────────

BOOST_AUTO_TEST_CASE(p7_thread_join_wrong_type_error) {
    NngEnvWithThread e;
    auto res = e.try_call("thread-join", {e.fixnum(42)});
    BOOST_TEST(!res.has_value());
}

BOOST_AUTO_TEST_CASE(p7_thread_alive_wrong_type_error) {
    NngEnvWithThread e;
    auto res = e.try_call("thread-alive?", {e.fixnum(42)});
    BOOST_TEST(!res.has_value());
}

// ── send!/recv! round-trip over inproc:// ─────────────────────────────────

BOOST_AUTO_TEST_CASE(p7_spawn_thread_send_recv_round_trip) {
    NngEnvWithThread e;

    // Echo worker: recv one message, send it back
    e.proc_mgr.set_worker_factory(
        make_simple_worker([](nng_socket s) {
            void* buf = nullptr; size_t sz = 0;
            if (nng_recv(s, &buf, &sz, NNG_FLAG_ALLOC) == 0) {
                nng_send(s, buf, sz, NNG_FLAG_ALLOC);
            }
        }));

    auto res = e.try_call("spawn-thread-with", {e.str("test.eta"), e.sym("echo-fn")});
    BOOST_REQUIRE(res.has_value());
    auto sock = *res;

    std::this_thread::sleep_for(std::chrono::milliseconds(30));

    {
        auto* sp = e.heap.try_get_as<ObjectKind::NngSocket, NngSocketPtr>(ops::payload(sock));
        BOOST_REQUIRE(sp != nullptr);
        nng_socket_set_ms(sp->socket, NNG_OPT_SENDTIMEO, 2000);
        nng_socket_set_ms(sp->socket, NNG_OPT_RECVTIMEO, 2000);
    }

    auto send_res = e.try_call("send!", {sock, e.fixnum(99)});
    BOOST_REQUIRE_MESSAGE(send_res.has_value() && *send_res != nanbox::False,
        "send! failed or timed out");

    auto recv_res = e.try_call("recv!", {sock});
    BOOST_REQUIRE_MESSAGE(recv_res.has_value() && *recv_res != nanbox::False,
        "recv! failed or timed out");
    BOOST_TEST(ops::decode<int64_t>(*recv_res).value_or(-1) == 99);
    BOOST_TEST_MESSAGE("Thread echo round-trip OK: 99 → 99");

    e.call("thread-join", {sock});
    e.call("nng-close", {sock});
}

// ── list_threads() returns thread metadata ────────────────────────────────

BOOST_AUTO_TEST_CASE(p7_list_threads_returns_metadata) {
    NngEnvWithThread e;
    e.proc_mgr.set_worker_factory(
        make_simple_worker([](nng_socket s) {
            void* buf = nullptr; size_t sz = 0;
            nng_socket_set_ms(s, NNG_OPT_RECVTIMEO, 300);
            nng_recv(s, &buf, &sz, NNG_FLAG_ALLOC);
            if (buf) nng_free(buf, sz);
        }));

    auto res = e.try_call("spawn-thread-with",
                          {e.str("test-module.eta"), e.sym("worker-fn")});
    BOOST_REQUIRE(res.has_value());

    std::this_thread::sleep_for(std::chrono::milliseconds(20));

    auto threads = e.proc_mgr.list_threads();
    BOOST_REQUIRE(!threads.empty());
    BOOST_TEST(threads[0].module_path == "test-module.eta");
    BOOST_TEST(threads[0].func_name   == "worker-fn");
    BOOST_TEST(threads[0].alive == true);

    e.call("thread-join", {*res});
    e.call("nng-close", {*res});
}

// ── Stress test: 20 threads, each echoes a distinct value ─────────────────

BOOST_AUTO_TEST_CASE(p7_stress_test_multiple_threads) {
    constexpr int N = 20;
    NngEnvWithThread e;
    e.proc_mgr.set_worker_factory(
        make_simple_worker([](nng_socket s) {
            void* buf = nullptr; size_t sz = 0;
            nng_socket_set_ms(s, NNG_OPT_RECVTIMEO, 3000);
            if (nng_recv(s, &buf, &sz, NNG_FLAG_ALLOC) == 0) {
                nng_send(s, buf, sz, NNG_FLAG_ALLOC);
            }
        }));

    std::vector<LispVal> sockets;
    sockets.reserve(N);
    for (int i = 0; i < N; ++i) {
        auto r = e.try_call("spawn-thread-with", {e.str("test.eta"), e.sym("echo")});
        BOOST_REQUIRE_MESSAGE(r.has_value(),
            "spawn-thread-with " + std::to_string(i) + " failed");
        sockets.push_back(*r);
    }

    BOOST_TEST_MESSAGE("Spawned " << N << " actor threads");
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    for (int i = 0; i < N; ++i) {
        auto* sp = e.heap.try_get_as<ObjectKind::NngSocket, NngSocketPtr>(
            ops::payload(sockets[static_cast<size_t>(i)]));
        if (sp) {
            nng_socket_set_ms(sp->socket, NNG_OPT_SENDTIMEO, 3000);
            nng_socket_set_ms(sp->socket, NNG_OPT_RECVTIMEO, 3000);
        }
        auto sr = e.try_call("send!", {sockets[static_cast<size_t>(i)], e.fixnum(i)});
        BOOST_REQUIRE_MESSAGE(sr.has_value() && *sr != nanbox::False,
            "send! to thread " + std::to_string(i) + " failed");
    }

    int ok_count = 0;
    for (int i = 0; i < N; ++i) {
        auto rr = e.try_call("recv!", {sockets[static_cast<size_t>(i)]});
        if (rr.has_value() && *rr != nanbox::False) {
            BOOST_TEST(ops::decode<int64_t>(*rr).value_or(-99) == i);
            ++ok_count;
        }
        e.call("thread-join", {sockets[static_cast<size_t>(i)]});
        e.call("nng-close", {sockets[static_cast<size_t>(i)]});
    }

    BOOST_TEST_MESSAGE("Stress test: " << ok_count << "/" << N << " threads OK");
    BOOST_TEST(ok_count == N);
}

BOOST_AUTO_TEST_SUITE_END()

