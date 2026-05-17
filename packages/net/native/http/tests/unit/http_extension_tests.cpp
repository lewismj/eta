#include "eta/http/http_metadata.h"
#include "eta/http/http_session.h"
#include "eta/native/runtime_binding.h"
#include "eta/native/sdk.h"
#include "eta/runtime/factory.h"
#include "eta/runtime/memory/heap.h"
#include "eta/runtime/memory/intern_table.h"
#include "eta/runtime/nanbox.h"
#include "eta/runtime/numeric_value.h"
#include "eta/runtime/string_view.h"
#include "eta/runtime/types/bytevector.h"
#include "eta/runtime/types/cons.h"
#include "eta/runtime/types/primitive.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

namespace {

using eta::runtime::memory::heap::Heap;
using eta::runtime::memory::intern::InternTable;
using eta::runtime::nanbox::False;
using eta::runtime::nanbox::LispVal;
using eta::runtime::nanbox::Nil;
using eta::runtime::nanbox::Tag;
using eta::runtime::nanbox::True;
using eta::runtime::nanbox::ops::is_boxed;
using eta::runtime::nanbox::ops::payload;
using eta::runtime::nanbox::ops::tag;
using PrimitiveArgs = eta::runtime::types::PrimitiveArgs;
using PrimitiveFunc = eta::runtime::types::PrimitiveFunc;
using PrimitiveResult =
    std::expected<eta::runtime::nanbox::LispVal, eta::runtime::error::RuntimeError>;

constexpr const char* kFixtureBody = "eta-http-loopback-body\n";

int expect_true(const bool condition, const std::string& message) {
    if (condition) return 0;
    std::cerr << "http_extension_tests: " << message << '\n';
    return 1;
}

struct RegisteredPrimitive {
    std::uint32_t arity{0};
    std::uint8_t has_rest{0};
    PrimitiveFunc* callable{nullptr};
};

std::unordered_map<std::string, RegisteredPrimitive> g_registered_primitives;
std::vector<std::string> g_reported_errors;

int register_stub(void* user_data,
                  const char* name,
                  const std::uint32_t arity,
                  const std::uint8_t has_rest,
                  void* callable) {
    if (name == nullptr || callable == nullptr) {
        return ETA_NATIVE_STATUS_ERROR;
    }

    auto* map = static_cast<std::unordered_map<std::string, RegisteredPrimitive>*>(user_data);
    if (map == nullptr) return ETA_NATIVE_STATUS_ERROR;

    (*map)[name] = RegisteredPrimitive{
        .arity = arity,
        .has_rest = has_rest,
        .callable = static_cast<PrimitiveFunc*>(callable),
    };
    return ETA_NATIVE_STATUS_OK;
}

void report_error_stub(void*, const char* message) {
    if (message == nullptr) return;
    g_reported_errors.emplace_back(message);
}

int alloc_native_object_stub(void* runtime_context,
                             const EtaNativeObjectVTable* vtable,
                             void* payload_ptr,
                             std::uint64_t* out_val) {
    if (runtime_context == nullptr || vtable == nullptr || out_val == nullptr) {
        return ETA_NATIVE_STATUS_ERROR;
    }

    auto* binding = static_cast<eta::native::SidecarRuntimeBindingV1*>(runtime_context);
    if (binding->heap == nullptr) return ETA_NATIVE_STATUS_ERROR;

    auto allocated = binding->heap->allocate<
        eta::runtime::memory::heap::NativeObjectHeader,
        eta::runtime::memory::heap::ObjectKind::NativeObject>(
        eta::runtime::memory::heap::NativeObjectHeader{vtable, payload_ptr});
    if (!allocated) return ETA_NATIVE_STATUS_ERROR;

    *out_val = eta::runtime::nanbox::ops::box(
        eta::runtime::nanbox::Tag::HeapObject,
        static_cast<LispVal>(*allocated));
    return ETA_NATIVE_STATUS_OK;
}

void* get_native_object_stub(void* runtime_context,
                             const std::uint64_t raw_value,
                             const EtaNativeObjectVTable* vtable) {
    if (runtime_context == nullptr || vtable == nullptr) return nullptr;

    auto* binding = static_cast<eta::native::SidecarRuntimeBindingV1*>(runtime_context);
    if (binding->heap == nullptr) return nullptr;

    const LispVal value = raw_value;
    if (!is_boxed(value) || tag(value) != Tag::HeapObject) return nullptr;

    auto* native_object = binding->heap->try_get_as<
        eta::runtime::memory::heap::ObjectKind::NativeObject,
        eta::runtime::memory::heap::NativeObjectHeader>(payload(value));
    if (native_object == nullptr || native_object->vtable != vtable) return nullptr;
    return native_object->user_data;
}

[[nodiscard]] std::string runtime_error_message(const eta::runtime::error::RuntimeError& error) {
    return std::visit(
        [](const auto& value) -> std::string {
            using T = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<T, eta::runtime::error::VMError>) {
                return value.message;
            } else if constexpr (std::is_same_v<T, eta::runtime::nanbox::NaNBoxError>) {
                return eta::runtime::nanbox::to_string(value);
            } else if constexpr (std::is_same_v<T, eta::runtime::memory::heap::HeapError>) {
                return eta::runtime::memory::heap::to_string(value);
            } else if constexpr (std::is_same_v<T, eta::runtime::memory::intern::InternTableError>) {
                return eta::runtime::memory::intern::to_string(value);
            } else {
                return "unknown runtime error";
            }
        },
        error);
}

[[nodiscard]] std::expected<LispVal, std::string> call_primitive(
    const std::string& name,
    const std::vector<LispVal>& args) {
    const auto found = g_registered_primitives.find(name);
    if (found == g_registered_primitives.end() || found->second.callable == nullptr) {
        return std::unexpected("missing primitive: " + name);
    }

    const PrimitiveResult result = (*found->second.callable)(PrimitiveArgs(args));
    if (!result) {
        return std::unexpected(name + ": " + runtime_error_message(result.error()));
    }
    return *result;
}

[[nodiscard]] std::expected<std::string, std::string> decode_string(
    const LispVal value,
    InternTable& intern_table) {
    auto string_view = eta::runtime::StringView::try_from(value, intern_table);
    if (!string_view) return std::unexpected("value is not a string");
    return std::string(string_view->view());
}

[[nodiscard]] std::expected<std::int64_t, std::string> decode_integer(
    const LispVal value,
    Heap& heap) {
    const auto numeric = eta::runtime::classify_numeric(value, heap);
    if (!numeric.is_valid() || numeric.is_flonum()) {
        return std::unexpected("value is not an integer");
    }
    return numeric.int_val;
}

[[nodiscard]] std::expected<std::vector<LispVal>, std::string> decode_list(
    const LispVal value,
    Heap& heap) {
    std::vector<LispVal> out;
    LispVal cursor = value;
    while (cursor != Nil) {
        if (!is_boxed(cursor) || tag(cursor) != Tag::HeapObject) {
            return std::unexpected("value is not a proper list");
        }
        auto* cons = heap.try_get_as<
            eta::runtime::memory::heap::ObjectKind::Cons,
            eta::runtime::types::Cons>(payload(cursor));
        if (cons == nullptr) {
            return std::unexpected("value is not a proper list");
        }
        out.push_back(cons->car);
        cursor = cons->cdr;
    }
    return out;
}

[[nodiscard]] std::expected<std::vector<std::pair<std::string, std::string>>, std::string>
decode_header_pairs(const LispVal value,
                    Heap& heap,
                    InternTable& intern_table) {
    auto list_values = decode_list(value, heap);
    if (!list_values) return std::unexpected(list_values.error());

    std::vector<std::pair<std::string, std::string>> out;
    out.reserve(list_values->size());
    for (const auto entry : *list_values) {
        if (!is_boxed(entry) || tag(entry) != Tag::HeapObject) {
            return std::unexpected("header entry is not a pair");
        }
        auto* pair = heap.try_get_as<
            eta::runtime::memory::heap::ObjectKind::Cons,
            eta::runtime::types::Cons>(payload(entry));
        if (pair == nullptr) return std::unexpected("header entry is not a pair");

        auto name = decode_string(pair->car, intern_table);
        if (!name) return std::unexpected("header name is not a string");
        auto header_value = decode_string(pair->cdr, intern_table);
        if (!header_value) return std::unexpected("header value is not a string");
        out.emplace_back(*name, *header_value);
    }
    return out;
}

[[nodiscard]] std::expected<std::string, std::string> decode_bytevector_string(
    const LispVal value,
    Heap& heap) {
    if (!is_boxed(value) || tag(value) != Tag::HeapObject) {
        return std::unexpected("value is not a bytevector");
    }
    auto* bytevector = heap.try_get_as<
        eta::runtime::memory::heap::ObjectKind::ByteVector,
        eta::runtime::types::ByteVector>(payload(value));
    if (bytevector == nullptr) {
        return std::unexpected("value is not a bytevector");
    }
    return std::string(bytevector->data.begin(), bytevector->data.end());
}

[[nodiscard]] std::expected<LispVal, std::string> make_string_value(
    Heap& heap,
    InternTable& intern_table,
    const std::string& text) {
    auto value = eta::runtime::memory::factory::make_string(heap, intern_table, text);
    if (!value) return std::unexpected("failed to allocate test string");
    return *value;
}

[[nodiscard]] std::expected<LispVal, std::string> make_symbol_value(
    InternTable& intern_table,
    const std::string& text) {
    auto value = eta::runtime::memory::factory::make_symbol(intern_table, text);
    if (!value) return std::unexpected("failed to allocate test symbol");
    return *value;
}

[[nodiscard]] std::expected<LispVal, std::string> make_cons_value(
    Heap& heap,
    const LispVal car,
    const LispVal cdr) {
    auto value = eta::runtime::memory::factory::make_cons(heap, car, cdr);
    if (!value) return std::unexpected("failed to allocate test cons");
    return *value;
}

[[nodiscard]] std::expected<LispVal, std::string> make_list_value(
    Heap& heap,
    const std::vector<LispVal>& values) {
    LispVal out = Nil;
    for (auto it = values.rbegin(); it != values.rend(); ++it) {
        auto cell = make_cons_value(heap, *it, out);
        if (!cell) return std::unexpected(cell.error());
        out = *cell;
    }
    return out;
}

[[nodiscard]] std::expected<LispVal, std::string> make_bytevector_value(
    Heap& heap,
    std::vector<std::uint8_t> bytes) {
    auto value = eta::runtime::memory::factory::make_bytevector(heap, std::move(bytes));
    if (!value) return std::unexpected("failed to allocate test bytevector");
    return *value;
}

[[nodiscard]] std::expected<LispVal, std::string> make_fixnum_value(
    Heap& heap,
    const std::int64_t value) {
    auto encoded = eta::runtime::memory::factory::make_fixnum(heap, value);
    if (!encoded) return std::unexpected("failed to allocate test fixnum");
    return *encoded;
}

[[nodiscard]] std::expected<std::vector<std::uint8_t>, std::string> read_file_bytes(
    const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input.is_open()) {
        return std::unexpected("failed to open file: " + path.string());
    }
    std::vector<std::uint8_t> bytes{
        std::istreambuf_iterator<char>(input),
        std::istreambuf_iterator<char>(),
    };
    if (input.bad()) {
        return std::unexpected("failed to read file: " + path.string());
    }
    return bytes;
}

[[nodiscard]] constexpr std::uint32_t rotate_right(const std::uint32_t value, const std::uint32_t bits) {
    return (value >> bits) | (value << (32u - bits));
}

[[nodiscard]] std::array<std::uint8_t, 32> sha256_digest(const std::vector<std::uint8_t>& input) {
    constexpr std::array<std::uint32_t, 64> kRoundConstants{
        0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u,
        0xab1c5ed5u, 0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu,
        0x9bdc06a7u, 0xc19bf174u, 0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu,
        0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau, 0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u,
        0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u, 0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu,
        0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u, 0xa2bfe8a1u, 0xa81a664bu,
        0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u, 0x19a4c116u,
        0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
        0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u,
        0xc67178f2u,
    };

    std::array<std::uint32_t, 8> hash_state{
        0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
        0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u,
    };

    std::vector<std::uint8_t> padded = input;
    padded.push_back(0x80u);
    while ((padded.size() % 64u) != 56u) {
        padded.push_back(0u);
    }

    const std::uint64_t bit_length = static_cast<std::uint64_t>(input.size()) * 8u;
    for (int shift = 56; shift >= 0; shift -= 8) {
        padded.push_back(static_cast<std::uint8_t>((bit_length >> shift) & 0xffu));
    }

    for (std::size_t chunk_offset = 0; chunk_offset < padded.size(); chunk_offset += 64u) {
        std::array<std::uint32_t, 64> schedule{};
        for (std::size_t i = 0; i < 16u; ++i) {
            const std::size_t idx = chunk_offset + (i * 4u);
            schedule[i] = (static_cast<std::uint32_t>(padded[idx]) << 24u)
                | (static_cast<std::uint32_t>(padded[idx + 1u]) << 16u)
                | (static_cast<std::uint32_t>(padded[idx + 2u]) << 8u)
                | static_cast<std::uint32_t>(padded[idx + 3u]);
        }
        for (std::size_t i = 16u; i < 64u; ++i) {
            const std::uint32_t s0 = rotate_right(schedule[i - 15u], 7u)
                ^ rotate_right(schedule[i - 15u], 18u)
                ^ (schedule[i - 15u] >> 3u);
            const std::uint32_t s1 = rotate_right(schedule[i - 2u], 17u)
                ^ rotate_right(schedule[i - 2u], 19u)
                ^ (schedule[i - 2u] >> 10u);
            schedule[i] = schedule[i - 16u] + s0 + schedule[i - 7u] + s1;
        }

        std::uint32_t a = hash_state[0];
        std::uint32_t b = hash_state[1];
        std::uint32_t c = hash_state[2];
        std::uint32_t d = hash_state[3];
        std::uint32_t e = hash_state[4];
        std::uint32_t f = hash_state[5];
        std::uint32_t g = hash_state[6];
        std::uint32_t h = hash_state[7];

        for (std::size_t i = 0; i < 64u; ++i) {
            const std::uint32_t s1 = rotate_right(e, 6u) ^ rotate_right(e, 11u) ^ rotate_right(e, 25u);
            const std::uint32_t choose = (e & f) ^ ((~e) & g);
            const std::uint32_t temp1 = h + s1 + choose + kRoundConstants[i] + schedule[i];
            const std::uint32_t s0 = rotate_right(a, 2u) ^ rotate_right(a, 13u) ^ rotate_right(a, 22u);
            const std::uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
            const std::uint32_t temp2 = s0 + majority;

            h = g;
            g = f;
            f = e;
            e = d + temp1;
            d = c;
            c = b;
            b = a;
            a = temp1 + temp2;
        }

        hash_state[0] += a;
        hash_state[1] += b;
        hash_state[2] += c;
        hash_state[3] += d;
        hash_state[4] += e;
        hash_state[5] += f;
        hash_state[6] += g;
        hash_state[7] += h;
    }

    std::array<std::uint8_t, 32> digest{};
    for (std::size_t i = 0; i < hash_state.size(); ++i) {
        const std::uint32_t word = hash_state[i];
        digest[(i * 4u)] = static_cast<std::uint8_t>((word >> 24u) & 0xffu);
        digest[(i * 4u) + 1u] = static_cast<std::uint8_t>((word >> 16u) & 0xffu);
        digest[(i * 4u) + 2u] = static_cast<std::uint8_t>((word >> 8u) & 0xffu);
        digest[(i * 4u) + 3u] = static_cast<std::uint8_t>(word & 0xffu);
    }
    return digest;
}

[[nodiscard]] std::string to_hex(const std::array<std::uint8_t, 32>& digest) {
    std::ostringstream out;
    out << std::hex << std::setfill('0');
    for (const auto byte : digest) {
        out << std::setw(2) << static_cast<int>(byte);
    }
    return out.str();
}

[[nodiscard]] std::optional<std::string> find_header_value(
    const std::vector<std::pair<std::string, std::string>>& headers,
    const std::string& name) {
    const auto it = std::find_if(
        headers.begin(),
        headers.end(),
        [&name](const auto& entry) {
            if (entry.first.size() != name.size()) return false;
            for (std::size_t i = 0; i < name.size(); ++i) {
                if (std::tolower(static_cast<unsigned char>(entry.first[i]))
                    != std::tolower(static_cast<unsigned char>(name[i]))) {
                    return false;
                }
            }
            return true;
        });
    if (it == headers.end()) return std::nullopt;
    return it->second;
}

} // namespace

extern "C" int eta_register_http_extension_v1(const EtaNativeApiV1* api,
                                              EtaExtensionInfoV1* out_info);

int main() {
    using namespace eta::http_sidecar;

    int failures = 0;
    Heap heap(1ull << 24);
    InternTable intern_table;
    eta::native::SidecarRuntimeBindingV1 binding{};
    binding.heap = &heap;
    binding.intern_table = &intern_table;

    failures += expect_true(
        std::strcmp(kLibcurlUpstreamTag, "curl-8_13_0") == 0,
        "libcurl upstream tag should be pinned to curl-8_13_0");
    failures += expect_true(
        std::strcmp(kNativeAbi, ETA_NATIVE_ABI_ID_V1) == 0,
        "native ABI should match eta-native-v1");

    EtaNativeApiV1 api{};
    api.struct_size = sizeof(EtaNativeApiV1);
    api.abi_id = ETA_NATIVE_ABI_ID_V1;
    api.user_data = &g_registered_primitives;
    api.runtime_context = &binding;
    api.register_primitive = &register_stub;
    api.report_error = &report_error_stub;
    api.alloc_native_object = &alloc_native_object_stub;
    api.get_native_object = &get_native_object_stub;

    g_registered_primitives.clear();
    g_reported_errors.clear();

    EtaExtensionInfoV1 info{};
    const int register_status = eta_register_http_extension_v1(&api, &info);
    failures += expect_true(
        register_status == ETA_NATIVE_STATUS_OK,
        "extension entry should succeed with API callbacks");
    failures += expect_true(
        std::strcmp(info.extension_id, kExtensionId) == 0,
        "extension id should match package metadata");
    failures += expect_true(
        std::strcmp(info.extension_version, kExtensionVersion) == 0,
        "extension version should match package metadata");
    failures += expect_true(
        std::strcmp(info.abi_id, ETA_NATIVE_ABI_ID_V1) == 0,
        "extension ABI should match eta-native-v1");
    failures += expect_true(
        g_registered_primitives.size() == 28u,
        "M3 extension should register twenty-eight primitives");
    failures += expect_true(
        g_reported_errors.empty(),
        "extension should not emit host diagnostics on successful registration");

    const auto version_result = call_primitive("http/version", {});
    failures += expect_true(
        version_result.has_value(),
        "http/version should execute successfully");
    if (version_result) {
        auto version_triple = decode_list(*version_result, heap);
        failures += expect_true(version_triple.has_value(), "http/version should return a list");
        if (version_triple) {
            failures += expect_true(
                version_triple->size() == 3u,
                "http/version should return a triple");
            if (version_triple->size() == 3u) {
                auto version_text = decode_string((*version_triple)[0], intern_table);
                failures += expect_true(
                    version_text.has_value() && !version_text->empty(),
                    "http/version should return a non-empty libcurl version string");
            }
        }
    } else {
        failures += expect_true(false, version_result.error());
    }

    const char* fixture_base = std::getenv("ETA_HTTP_FIXTURE_BASE_URL");
    failures += expect_true(
        fixture_base != nullptr && fixture_base[0] != '\0',
        "ETA_HTTP_FIXTURE_BASE_URL should be set by loopback fixture");
    if (fixture_base == nullptr || fixture_base[0] == '\0') {
        return failures == 0 ? 0 : 1;
    }
    const std::string base_url(fixture_base);

    auto method_get = make_symbol_value(intern_table, "get");
    auto method_post = make_symbol_value(intern_table, "post");
    auto sym_max_redirects = make_symbol_value(intern_table, "max-redirects");
    auto sym_timeout_ms = make_symbol_value(intern_table, "timeout-ms");
    failures += expect_true(method_get.has_value(), "method symbol get should allocate");
    failures += expect_true(method_post.has_value(), "method symbol post should allocate");
    failures += expect_true(sym_max_redirects.has_value(), "option symbol max-redirects should allocate");
    failures += expect_true(sym_timeout_ms.has_value(), "option symbol timeout-ms should allocate");
    if (!method_get || !method_post || !sym_max_redirects || !sym_timeout_ms) {
        return failures == 0 ? 0 : 1;
    }

    auto run_request = [&](const LispVal method_symbol,
                           const std::string& path) -> std::expected<std::pair<LispVal, LispVal>, std::string> {
        auto session = call_primitive("http/session-new", {});
        if (!session) return std::unexpected(session.error());

        auto request = call_primitive("http/request-new", {*session, method_symbol});
        if (!request) return std::unexpected(request.error());

        auto url = make_string_value(heap, intern_table, base_url + path);
        if (!url) return std::unexpected(url.error());
        auto set_url = call_primitive("http/request-set-url!", {*request, *url});
        if (!set_url) return std::unexpected(set_url.error());

        return std::make_pair(*session, *request);
    };

    auto run_simple_get = [&](const std::string& path) -> std::expected<LispVal, std::string> {
        auto handles = run_request(*method_get, path);
        if (!handles) return std::unexpected(handles.error());
        return call_primitive("http/perform", {handles->second});
    };

    // Case 2: GET / returns 200 with fixture body.
    auto response_200 = run_simple_get("/");
    failures += expect_true(response_200.has_value(), "GET / should succeed");
    if (response_200) {
        auto status_200 = call_primitive("http/response-status", {*response_200});
        failures += expect_true(status_200.has_value(), "response-status should succeed for GET /");
        if (status_200) {
            auto decoded = decode_integer(*status_200, heap);
            failures += expect_true(
                decoded.has_value() && *decoded == 200,
                "GET / should return status 200");
        }

        auto body = call_primitive("http/response-body-bytes", {*response_200});
        failures += expect_true(body.has_value(), "response-body-bytes should succeed for GET /");
        if (body) {
            auto body_text = decode_bytevector_string(*body, heap);
            failures += expect_true(
                body_text.has_value() && *body_text == kFixtureBody,
                "GET / should return fixture response bytes");
        }

        auto effective_url = call_primitive("http/response-effective-url", {*response_200});
        failures += expect_true(
            effective_url.has_value(),
            "response-effective-url should succeed for GET /");
        if (effective_url) {
            auto text = decode_string(*effective_url, intern_table);
            failures += expect_true(
                text.has_value() && *text == base_url + "/",
                "effective-url should match loopback endpoint");
        }

        auto headers = call_primitive("http/response-headers", {*response_200});
        failures += expect_true(
            headers.has_value(),
            "response-headers should succeed for GET /");
        if (headers) {
            auto decoded_headers = decode_header_pairs(*headers, heap, intern_table);
            failures += expect_true(
                decoded_headers.has_value() && !decoded_headers->empty(),
                "response-headers should return at least one header");
        }
    }

    // Case 3: GET /missing returns 404 without transport error.
    auto response_404 = run_simple_get("/missing");
    failures += expect_true(response_404.has_value(), "GET /missing should succeed");
    if (response_404) {
        auto status_404 = call_primitive("http/response-status", {*response_404});
        failures += expect_true(
            status_404.has_value(),
            "response-status should succeed for GET /missing");
        if (status_404) {
            auto decoded = decode_integer(*status_404, heap);
            failures += expect_true(
                decoded.has_value() && *decoded == 404,
                "GET /missing should return status 404");
        }
    }

    // Case 4: POST bytes round-trips exactly.
    {
        auto handles = run_request(*method_post, "/echo");
        failures += expect_true(handles.has_value(), "POST /echo setup should succeed");
        if (handles) {
            const std::string payload = "post-bytes-roundtrip";
            auto payload_bytes = make_bytevector_value(
                heap,
                std::vector<std::uint8_t>(payload.begin(), payload.end()));
            failures += expect_true(payload_bytes.has_value(), "bytevector payload should allocate");
            if (payload_bytes) {
                auto set_body = call_primitive(
                    "http/request-set-body-bytes!",
                    {handles->second, *payload_bytes});
                failures += expect_true(set_body.has_value(), "request-set-body-bytes should succeed");
                if (set_body) {
                    auto response = call_primitive("http/perform", {handles->second});
                    failures += expect_true(response.has_value(), "POST /echo should succeed");
                    if (response) {
                        auto body = call_primitive("http/response-body-bytes", {*response});
                        failures += expect_true(body.has_value(), "POST /echo body should decode");
                        if (body) {
                            auto body_text = decode_bytevector_string(*body, heap);
                            failures += expect_true(
                                body_text.has_value() && *body_text == payload,
                                "POST /echo should round-trip request bytes");
                        }
                    }
                }
            }
        }
    }

    // Case 5: multipart parts are reflected by the fixture.
    {
        auto handles = run_request(*method_post, "/multipart");
        failures += expect_true(handles.has_value(), "POST /multipart setup should succeed");
        if (handles) {
            auto p1_name = make_string_value(heap, intern_table, "alpha");
            auto p1_data = make_string_value(heap, intern_table, "one");
            auto p2_name = make_string_value(heap, intern_table, "beta");
            auto p2_file = make_string_value(heap, intern_table, "beta.txt");
            auto p2_type = make_string_value(heap, intern_table, "text/plain");
            auto p2_data = make_string_value(heap, intern_table, "two");
            failures += expect_true(
                p1_name && p1_data && p2_name && p2_file && p2_type && p2_data,
                "multipart test values should allocate");
            if (p1_name && p1_data && p2_name && p2_file && p2_type && p2_data) {
                auto part1 = make_list_value(heap, {*p1_name, *p1_data});
                auto part2 = make_list_value(heap, {*p2_name, *p2_file, *p2_type, *p2_data});
                failures += expect_true(part1.has_value() && part2.has_value(), "multipart parts should build");
                if (part1 && part2) {
                    auto parts = make_list_value(heap, {*part1, *part2});
                    failures += expect_true(parts.has_value(), "multipart list should build");
                    if (parts) {
                        auto set_multipart = call_primitive(
                            "http/request-set-body-multipart!",
                            {handles->second, *parts});
                        failures += expect_true(set_multipart.has_value(), "set multipart body should succeed");
                        if (set_multipart) {
                            auto response = call_primitive("http/perform", {handles->second});
                            failures += expect_true(response.has_value(), "POST /multipart should succeed");
                            if (response) {
                                auto body = call_primitive("http/response-body-bytes", {*response});
                                failures += expect_true(body.has_value(), "multipart response should decode");
                                if (body) {
                                    auto text = decode_bytevector_string(*body, heap);
                                    failures += expect_true(
                                        text.has_value() && *text == "alpha,beta",
                                        "multipart response should contain sorted part names");
                                }
                            }
                        }
                    }
                }
            }
        }
    }

    // Case 6: request headers are visible on the server.
    {
        auto handles = run_request(*method_get, "/headers-echo");
        failures += expect_true(handles.has_value(), "GET /headers-echo setup should succeed");
        if (handles) {
            auto header_name = make_string_value(heap, intern_table, "X-Eta-Test");
            auto header_value = make_string_value(heap, intern_table, "header-roundtrip");
            failures += expect_true(
                header_name.has_value() && header_value.has_value(),
                "header test values should allocate");
            if (header_name && header_value) {
                auto set_header = call_primitive(
                    "http/request-set-header!",
                    {handles->second, *header_name, *header_value});
                failures += expect_true(set_header.has_value(), "request-set-header should succeed");
                if (set_header) {
                    auto response = call_primitive("http/perform", {handles->second});
                    failures += expect_true(response.has_value(), "GET /headers-echo should succeed");
                    if (response) {
                        auto body = call_primitive("http/response-body-bytes", {*response});
                        failures += expect_true(body.has_value(), "header echo body should decode");
                        if (body) {
                            auto body_text = decode_bytevector_string(*body, heap);
                            failures += expect_true(
                                body_text.has_value() && *body_text == "header-roundtrip",
                                "header echo should match request header value");
                        }
                    }
                }
            }
        }
    }

    // Case 7: redirects follow when defaults are active.
    {
        auto response = run_simple_get("/redirect");
        failures += expect_true(response.has_value(), "GET /redirect should succeed");
        if (response) {
            auto status_value = call_primitive("http/response-status", {*response});
            failures += expect_true(status_value.has_value(), "redirect status should decode");
            if (status_value) {
                auto status_num = decode_integer(*status_value, heap);
                failures += expect_true(
                    status_num.has_value() && *status_num == 200,
                    "GET /redirect should resolve to status 200 after follow");
            }
            auto effective = call_primitive("http/response-effective-url", {*response});
            failures += expect_true(effective.has_value(), "redirect effective URL should decode");
            if (effective) {
                auto text = decode_string(*effective, intern_table);
                failures += expect_true(
                    text.has_value() && *text == base_url + "/redirect-target",
                    "redirect effective URL should end at /redirect-target");
            }
        }
    }

    // Case 8: max-redirects=0 short-circuits redirect following.
    {
        auto handles = run_request(*method_get, "/redirect");
        failures += expect_true(handles.has_value(), "redirect-cap setup should succeed");
        if (handles) {
            auto zero = make_fixnum_value(heap, 0);
            failures += expect_true(zero.has_value(), "redirect-cap option value should allocate");
            if (!zero) {
                return failures == 0 ? 0 : 1;
            }
            auto set_cap = call_primitive(
                "http/session-set-option!",
                {handles->first, *sym_max_redirects, *zero});
            failures += expect_true(set_cap.has_value(), "session-set-option max-redirects should succeed");
            if (set_cap) {
                auto response = call_primitive("http/perform", {handles->second});
                failures += expect_true(response.has_value(), "redirect-cap request should succeed");
                if (response) {
                    auto status_value = call_primitive("http/response-status", {*response});
                    failures += expect_true(status_value.has_value(), "redirect-cap status should decode");
                    if (status_value) {
                        auto status_num = decode_integer(*status_value, heap);
                        failures += expect_true(
                            status_num.has_value() && *status_num == 302,
                            "max-redirects=0 should return the redirect response");
                    }
                }
            }
        }
    }

    // Case 9: timeout-ms triggers CURLE_OPERATION_TIMEDOUT on a stalled endpoint.
    {
        auto handles = run_request(*method_get, "/stall");
        failures += expect_true(handles.has_value(), "timeout test setup should succeed");
        if (handles) {
            auto timeout_value = make_fixnum_value(heap, 100);
            failures += expect_true(timeout_value.has_value(), "timeout option value should allocate");
            if (timeout_value) {
                auto set_timeout = call_primitive(
                    "http/request-set-option!",
                    {handles->second, *sym_timeout_ms, *timeout_value});
                failures += expect_true(set_timeout.has_value(), "request timeout option should apply");
                if (set_timeout) {
                    auto timed_response = call_primitive("http/perform", {handles->second});
                    failures += expect_true(!timed_response.has_value(), "timeout request should fail");
                    if (!timed_response) {
                        std::string message = timed_response.error();
                        std::transform(
                            message.begin(),
                            message.end(),
                            message.begin(),
                            [](const unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
                        const bool contains_timeout_code = message.find("28") != std::string::npos;
                        const bool contains_timeout_text = message.find("timed out") != std::string::npos;
                        failures += expect_true(
                            contains_timeout_code || contains_timeout_text,
                            "timeout error should report CURLE_OPERATION_TIMEDOUT");
                    }
                }
            }
        }
    }

    // Case 10: TLS verification rejects expired certificates (skip when offline).
    {
        auto session = call_primitive("http/session-new", {});
        failures += expect_true(session.has_value(), "tls verify session should allocate");
        if (session) {
            auto request = call_primitive("http/request-new", {*session, *method_get});
            failures += expect_true(request.has_value(), "tls verify request should allocate");
            if (request) {
                auto tls_url = make_string_value(heap, intern_table, "https://expired.badssl.com/");
                failures += expect_true(tls_url.has_value(), "tls verify URL should allocate");
                if (tls_url) {
                    auto set_url = call_primitive("http/request-set-url!", {*request, *tls_url});
                    failures += expect_true(set_url.has_value(), "tls verify URL should apply");
                    if (set_url) {
                        auto tls_response = call_primitive("http/perform", {*request});
                        if (tls_response.has_value()) {
                            failures += expect_true(false, "TLS verify should fail against expired.badssl.com");
                        } else {
                            std::string message = tls_response.error();
                            std::transform(
                                message.begin(),
                                message.end(),
                                message.begin(),
                                [](const unsigned char ch) { return static_cast<char>(std::tolower(ch)); });

                            const bool offline_skip = message.find("couldn't resolve host") != std::string::npos
                                || message.find("could not resolve host") != std::string::npos
                                || message.find("failed to connect") != std::string::npos
                                || message.find("network is unreachable") != std::string::npos
                                || message.find("timed out") != std::string::npos;
                            const bool verify_failure = message.find("certificate") != std::string::npos
                                || message.find("ssl") != std::string::npos
                                || message.find("60") != std::string::npos;

                            if (!offline_skip) {
                                failures += expect_true(
                                    verify_failure,
                                    "TLS verify failure should report certificate validation error");
                            }
                        }
                    }
                }
            }
        }
    }

    // Case 11: cookies persist across requests in one session.
    {
        auto session = call_primitive("http/session-new", {});
        failures += expect_true(session.has_value(), "cookie test session should allocate");
        if (session) {
            auto request_set = call_primitive("http/request-new", {*session, *method_get});
            failures += expect_true(request_set.has_value(), "cookie set request should allocate");
            if (request_set) {
                auto set_url = make_string_value(heap, intern_table, base_url + "/cookie/set");
                failures += expect_true(set_url.has_value(), "cookie set URL should allocate");
                if (set_url) {
                    auto set_status = call_primitive("http/request-set-url!", {*request_set, *set_url});
                    failures += expect_true(set_status.has_value(), "cookie set URL should apply");
                    if (set_status) {
                        auto response_set = call_primitive("http/perform", {*request_set});
                        failures += expect_true(response_set.has_value(), "cookie set request should succeed");
                    }
                }
            }

            auto request_echo = call_primitive("http/request-new", {*session, *method_get});
            failures += expect_true(request_echo.has_value(), "cookie echo request should allocate");
            if (request_echo) {
                auto echo_url = make_string_value(heap, intern_table, base_url + "/cookie/echo");
                failures += expect_true(echo_url.has_value(), "cookie echo URL should allocate");
                if (echo_url) {
                    auto set_echo_url = call_primitive("http/request-set-url!", {*request_echo, *echo_url});
                    failures += expect_true(set_echo_url.has_value(), "cookie echo URL should apply");
                    if (set_echo_url) {
                        auto response_echo = call_primitive("http/perform", {*request_echo});
                        failures += expect_true(response_echo.has_value(), "cookie echo request should succeed");
                        if (response_echo) {
                            auto body = call_primitive("http/response-body-bytes", {*response_echo});
                            failures += expect_true(body.has_value(), "cookie echo body should decode");
                            if (body) {
                                auto cookie_text = decode_bytevector_string(*body, heap);
                                failures += expect_true(
                                    cookie_text.has_value()
                                        && cookie_text->find("eta_cookie=crumb") != std::string::npos,
                                    "cookie echo should include eta_cookie=crumb");
                            }
                        }
                    }
                }
            }
        }
    }

    // Case 12: streaming download writes 10 MB file whose SHA-256 matches fixture header.
    {
        auto stream_handles = run_request(*method_get, "/download/1k");
        failures += expect_true(stream_handles.has_value(), "perform-stream setup should succeed");
        if (stream_handles) {
            auto stream_response = call_primitive(
                "http/perform-stream",
                {stream_handles->second, False, False});
            failures += expect_true(stream_response.has_value(), "http/perform-stream should succeed");
            if (stream_response) {
                auto body = call_primitive("http/response-body-bytes", {*stream_response});
                failures += expect_true(body.has_value(), "perform-stream body should decode");
                if (body) {
                    auto streamed_text = decode_bytevector_string(*body, heap);
                    failures += expect_true(
                        streamed_text.has_value() && streamed_text->size() == 1024u,
                        "perform-stream should capture full 1 KB response body");
                }
            }
        }

        std::error_code temp_dir_error;
        std::filesystem::path temp_dir = std::filesystem::temp_directory_path(temp_dir_error);
        if (temp_dir_error) {
            temp_dir = std::filesystem::current_path();
        }
        const auto now_ticks = std::chrono::steady_clock::now().time_since_epoch().count();
        const std::filesystem::path download_path =
            temp_dir / ("eta_http_stream_test_" + std::to_string(now_ticks) + ".bin");

        auto url_value = make_string_value(heap, intern_table, base_url + "/download/10mb");
        auto path_value = make_string_value(heap, intern_table, download_path.string());
        failures += expect_true(
            url_value.has_value() && path_value.has_value(),
            "streaming-download arguments should allocate");
        if (url_value && path_value) {
            auto download_response = call_primitive("http/download", {*url_value, *path_value, Nil});
            failures += expect_true(download_response.has_value(), "http/download should succeed");
            if (download_response) {
                auto status_value = call_primitive("http/response-status", {*download_response});
                failures += expect_true(status_value.has_value(), "download response status should decode");
                if (status_value) {
                    auto status_num = decode_integer(*status_value, heap);
                    failures += expect_true(
                        status_num.has_value() && *status_num == 200,
                        "http/download should return status 200");
                }

                auto headers_value = call_primitive("http/response-headers", {*download_response});
                failures += expect_true(headers_value.has_value(), "download headers should decode");
                if (headers_value) {
                    auto headers = decode_header_pairs(*headers_value, heap, intern_table);
                    failures += expect_true(headers.has_value(), "download headers should parse");
                    if (headers) {
                        auto expected_sha = find_header_value(*headers, "X-Body-Sha256");
                        failures += expect_true(
                            expected_sha.has_value(),
                            "download response should expose X-Body-Sha256");
                        if (expected_sha) {
                            auto file_bytes = read_file_bytes(download_path);
                            failures += expect_true(
                                file_bytes.has_value(),
                                "downloaded file should be readable");
                            if (file_bytes) {
                                const auto digest = to_hex(sha256_digest(*file_bytes));
                                failures += expect_true(
                                    digest == *expected_sha,
                                    "downloaded file SHA-256 should match fixture header");
                                failures += expect_true(
                                    file_bytes->size() == (10u * 1024u * 1024u),
                                    "downloaded file should be 10 MB");
                            }
                        }
                    }
                }
            }
        }

        std::error_code remove_error;
        std::filesystem::remove(download_path, remove_error);
    }

    // Case 13: parallel independent sessions complete without transport errors.
    {
        auto run_parallel_session = [&](const int worker_id) -> std::optional<std::string> {
            auto session = std::make_shared<eta::http_sidecar::HttpSession>("eta-http-parallel-test");
            auto init_status = session->initialize();
            if (!init_status) {
                return "parallel worker " + std::to_string(worker_id) + ": " + init_status.error();
            }

            for (int i = 0; i < 8; ++i) {
                auto request = std::make_shared<eta::http_sidecar::HttpRequest>(session, "GET");
                auto request_init = request->initialize();
                if (!request_init) {
                    return "parallel worker " + std::to_string(worker_id) + ": " + request_init.error();
                }
                auto set_url = request->set_url(base_url + "/");
                if (!set_url) {
                    return "parallel worker " + std::to_string(worker_id) + ": " + set_url.error();
                }
                auto response = request->perform();
                if (!response) {
                    return "parallel worker " + std::to_string(worker_id) + ": " + response.error();
                }
                if (response->status != 200 || response->body.empty()) {
                    return "parallel worker " + std::to_string(worker_id)
                        + ": unexpected response status/body";
                }
            }
            return std::nullopt;
        };

        std::optional<std::string> worker1_error;
        std::optional<std::string> worker2_error;
        std::thread worker1([&]() { worker1_error = run_parallel_session(1); });
        std::thread worker2([&]() { worker2_error = run_parallel_session(2); });
        worker1.join();
        worker2.join();

        failures += expect_true(
            !worker1_error.has_value() && !worker2_error.has_value(),
            "parallel sessions should complete without failures");
        if (worker1_error) failures += expect_true(false, *worker1_error);
        if (worker2_error) failures += expect_true(false, *worker2_error);
    }

    return failures == 0 ? 0 : 1;
}
