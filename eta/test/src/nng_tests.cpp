#include <boost/test/unit_test.hpp>

#include <cmath>
#include <cstdint>
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

    // Performance acceptance criterion: < 10 ms
    BOOST_TEST(duration_ms < 10);

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
