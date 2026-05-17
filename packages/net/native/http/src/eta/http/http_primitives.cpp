#include "eta/http/http_primitives.h"

#include "eta/http/http_metadata.h"
#include "eta/http/http_session.h"
#include "eta/native/runtime_binding.h"
#include "eta/runtime/error.h"
#include "eta/runtime/factory.h"
#include "eta/runtime/nanbox.h"
#include "eta/runtime/numeric_value.h"
#include "eta/runtime/string_view.h"
#include "eta/runtime/types/bytevector.h"
#include "eta/runtime/types/cons.h"
#include "eta/runtime/types/primitive.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <expected>
#include <iterator>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace eta::http_sidecar {

namespace {

using PrimitiveArgs = eta::runtime::types::PrimitiveArgs;
using PrimitiveFunc = eta::runtime::types::PrimitiveFunc;
using PrimitiveResult = std::expected<eta::runtime::nanbox::LispVal, eta::runtime::error::RuntimeError>;
using eta::runtime::error::RuntimeError;
using eta::runtime::error::RuntimeErrorCode;
using eta::runtime::error::VMError;
using eta::runtime::memory::factory::make_bytevector;
using eta::runtime::memory::factory::make_cons;
using eta::runtime::memory::factory::make_fixnum;
using eta::runtime::memory::factory::make_string;
using eta::runtime::memory::factory::make_symbol;
using eta::runtime::nanbox::False;
using eta::runtime::nanbox::LispVal;
using eta::runtime::nanbox::Nil;
using eta::runtime::nanbox::Tag;
using eta::runtime::nanbox::True;
using eta::runtime::nanbox::ops::is_boxed;
using eta::runtime::nanbox::ops::payload;
using eta::runtime::nanbox::ops::tag;
using eta::runtime::StringView;

struct RuntimeState {
    void* runtime_context{nullptr};
    EtaAllocNativeObjectFnV1 alloc_native_object{nullptr};
    EtaGetNativeObjectFnV1 get_native_object{nullptr};
    eta::runtime::memory::heap::Heap* heap{nullptr};
    eta::runtime::memory::intern::InternTable* intern_table{nullptr};
};

RuntimeState g_runtime{};

struct SessionHandle {
    std::shared_ptr<HttpSession> session;
};

struct RequestHandle {
    std::shared_ptr<HttpRequest> request;
};

struct ResponseHandle {
    std::shared_ptr<HttpResponse> response;
};

extern "C" void destroy_session_payload(void* user_data) {
    delete static_cast<SessionHandle*>(user_data);
}

extern "C" void destroy_request_payload(void* user_data) {
    delete static_cast<RequestHandle*>(user_data);
}

extern "C" void destroy_response_payload(void* user_data) {
    delete static_cast<ResponseHandle*>(user_data);
}

constexpr EtaNativeObjectVTable kSessionVTable{
    .type_name = "http-session",
    .destroy = &destroy_session_payload,
    .trace = nullptr,
    .display = nullptr,
};

constexpr EtaNativeObjectVTable kRequestVTable{
    .type_name = "http-request",
    .destroy = &destroy_request_payload,
    .trace = nullptr,
    .display = nullptr,
};

constexpr EtaNativeObjectVTable kResponseVTable{
    .type_name = "http-response",
    .destroy = &destroy_response_payload,
    .trace = nullptr,
    .display = nullptr,
};

[[nodiscard]] RuntimeError type_error(std::string message) {
    return RuntimeError{VMError{RuntimeErrorCode::TypeError, std::move(message)}};
}

[[nodiscard]] RuntimeError user_error(std::string message) {
    return RuntimeError{VMError{RuntimeErrorCode::UserError, std::move(message)}};
}

[[nodiscard]] RuntimeError internal_error(std::string message) {
    return RuntimeError{VMError{RuntimeErrorCode::InternalError, std::move(message)}};
}

[[nodiscard]] bool native_object_api_available(const EtaNativeApiV1* api) {
    if (api == nullptr || api->runtime_context == nullptr) return false;
    const bool has_alloc = ETA_NATIVE_API_V1_HAS_FIELD(api, alloc_native_object)
        && api->alloc_native_object != nullptr;
    const bool has_get = ETA_NATIVE_API_V1_HAS_FIELD(api, get_native_object)
        && api->get_native_object != nullptr;
    return has_alloc && has_get;
}

[[nodiscard]] std::string to_upper_ascii(std::string text) {
    std::transform(
        text.begin(),
        text.end(),
        text.begin(),
        [](const unsigned char ch) { return static_cast<char>(std::toupper(ch)); });
    return text;
}

[[nodiscard]] std::string to_lower_ascii(std::string text) {
    std::transform(
        text.begin(),
        text.end(),
        text.begin(),
        [](const unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return text;
}

[[nodiscard]] std::expected<LispVal, RuntimeError> make_eta_string(std::string_view text) {
    if (g_runtime.heap == nullptr || g_runtime.intern_table == nullptr) {
        return std::unexpected(internal_error("http: runtime string allocator is unavailable"));
    }
    auto value = make_string(*g_runtime.heap, *g_runtime.intern_table, std::string(text));
    if (!value) return std::unexpected(value.error());
    return *value;
}

[[nodiscard]] std::expected<LispVal, RuntimeError> make_eta_symbol(std::string_view text) {
    if (g_runtime.intern_table == nullptr) {
        return std::unexpected(internal_error("http: runtime symbol allocator is unavailable"));
    }
    auto value = make_symbol(*g_runtime.intern_table, std::string(text));
    if (!value) return std::unexpected(value.error());
    return *value;
}

[[nodiscard]] std::expected<LispVal, RuntimeError> make_eta_fixnum(const std::int64_t value) {
    if (g_runtime.heap == nullptr) {
        return std::unexpected(internal_error("http: runtime numeric allocator is unavailable"));
    }
    auto encoded = make_fixnum(*g_runtime.heap, value);
    if (!encoded) return std::unexpected(encoded.error());
    return *encoded;
}

[[nodiscard]] std::expected<LispVal, RuntimeError> make_eta_cons(const LispVal car, const LispVal cdr) {
    if (g_runtime.heap == nullptr) {
        return std::unexpected(internal_error("http: runtime cons allocator is unavailable"));
    }
    auto value = make_cons(*g_runtime.heap, car, cdr);
    if (!value) return std::unexpected(value.error());
    return *value;
}

[[nodiscard]] std::expected<LispVal, RuntimeError> make_eta_bytevector(
    std::vector<std::uint8_t> bytes) {
    if (g_runtime.heap == nullptr) {
        return std::unexpected(internal_error("http: runtime bytevector allocator is unavailable"));
    }
    auto value = make_bytevector(*g_runtime.heap, std::move(bytes));
    if (!value) return std::unexpected(value.error());
    return *value;
}

[[nodiscard]] std::expected<LispVal, RuntimeError> make_eta_list(
    const std::vector<LispVal>& values) {
    LispVal out = Nil;
    for (auto it = values.rbegin(); it != values.rend(); ++it) {
        auto cell = make_eta_cons(*it, out);
        if (!cell) return std::unexpected(cell.error());
        out = *cell;
    }
    return out;
}

[[nodiscard]] std::expected<std::string, RuntimeError> decode_string(
    const LispVal value,
    const char* who,
    const char* arg_label) {
    if (g_runtime.intern_table == nullptr) {
        return std::unexpected(internal_error(
            std::string(who) + ": runtime intern table is unavailable"));
    }
    auto sv = StringView::try_from(value, *g_runtime.intern_table);
    if (!sv) {
        return std::unexpected(type_error(
            std::string(who) + ": " + arg_label + " must be a string"));
    }
    return std::string(sv->view());
}

[[nodiscard]] std::expected<std::string, RuntimeError> decode_symbol_or_string(
    const LispVal value,
    const char* who,
    const char* arg_label) {
    if (!is_boxed(value)) {
        return std::unexpected(type_error(
            std::string(who) + ": " + arg_label + " must be a symbol or string"));
    }

    if (tag(value) == Tag::Symbol) {
        if (g_runtime.intern_table == nullptr) {
            return std::unexpected(internal_error(
                std::string(who) + ": runtime intern table is unavailable"));
        }
        auto text = g_runtime.intern_table->get_string(payload(value));
        if (!text) {
            return std::unexpected(internal_error(
                std::string(who) + ": unresolved symbol payload"));
        }
        return std::string(*text);
    }

    return decode_string(value, who, arg_label);
}

[[nodiscard]] std::expected<bool, RuntimeError> decode_boolean(
    const LispVal value,
    const char* who,
    const char* arg_label) {
    if (value == True) return true;
    if (value == False) return false;
    return std::unexpected(type_error(
        std::string(who) + ": " + arg_label + " must be a boolean"));
}

[[nodiscard]] std::expected<long, RuntimeError> decode_integer(
    const LispVal value,
    const char* who,
    const char* arg_label) {
    if (g_runtime.heap == nullptr) {
        return std::unexpected(internal_error(
            std::string(who) + ": runtime heap is unavailable"));
    }
    const auto numeric = eta::runtime::classify_numeric(value, *g_runtime.heap);
    if (!numeric.is_valid() || numeric.is_flonum()) {
        return std::unexpected(type_error(
            std::string(who) + ": " + arg_label + " must be an integer"));
    }
    if (numeric.int_val < static_cast<std::int64_t>((std::numeric_limits<long>::min)())
        || numeric.int_val > static_cast<std::int64_t>((std::numeric_limits<long>::max)())) {
        return std::unexpected(type_error(
            std::string(who) + ": " + arg_label + " is out of range"));
    }
    return static_cast<long>(numeric.int_val);
}

[[nodiscard]] std::expected<std::vector<std::uint8_t>, RuntimeError> decode_bytevector(
    const LispVal value,
    const char* who,
    const char* arg_label) {
    if (!is_boxed(value) || tag(value) != Tag::HeapObject || g_runtime.heap == nullptr) {
        return std::unexpected(type_error(
            std::string(who) + ": " + arg_label + " must be a bytevector"));
    }

    auto* bytevector = g_runtime.heap->try_get_as<
        eta::runtime::memory::heap::ObjectKind::ByteVector,
        eta::runtime::types::ByteVector>(payload(value));
    if (bytevector == nullptr) {
        return std::unexpected(type_error(
            std::string(who) + ": " + arg_label + " must be a bytevector"));
    }
    return bytevector->data;
}

[[nodiscard]] std::expected<std::vector<std::uint8_t>, RuntimeError> decode_bytes_or_string(
    const LispVal value,
    const char* who,
    const char* arg_label) {
    if (is_boxed(value) && tag(value) == Tag::String) {
        auto text = decode_string(value, who, arg_label);
        if (!text) return std::unexpected(text.error());
        return std::vector<std::uint8_t>(text->begin(), text->end());
    }
    return decode_bytevector(value, who, arg_label);
}

[[nodiscard]] std::expected<std::vector<LispVal>, RuntimeError> decode_list(
    const LispVal value,
    const char* who,
    const char* arg_label) {
    if (g_runtime.heap == nullptr) {
        return std::unexpected(internal_error(
            std::string(who) + ": runtime heap is unavailable"));
    }

    std::vector<LispVal> out;
    LispVal cursor = value;
    while (cursor != Nil) {
        if (!is_boxed(cursor) || tag(cursor) != Tag::HeapObject) {
            return std::unexpected(type_error(
                std::string(who) + ": " + arg_label + " must be a proper list"));
        }
        auto* cons = g_runtime.heap->try_get_as<
            eta::runtime::memory::heap::ObjectKind::Cons,
            eta::runtime::types::Cons>(payload(cursor));
        if (cons == nullptr) {
            return std::unexpected(type_error(
                std::string(who) + ": " + arg_label + " must be a proper list"));
        }
        out.push_back(cons->car);
        cursor = cons->cdr;
    }
    return out;
}

[[nodiscard]] std::expected<std::pair<LispVal, LispVal>, RuntimeError> decode_pair(
    const LispVal value,
    const char* who,
    const char* arg_label) {
    if (g_runtime.heap == nullptr || !is_boxed(value) || tag(value) != Tag::HeapObject) {
        return std::unexpected(type_error(
            std::string(who) + ": " + arg_label + " must be a pair"));
    }
    auto* pair = g_runtime.heap->try_get_as<
        eta::runtime::memory::heap::ObjectKind::Cons,
        eta::runtime::types::Cons>(payload(value));
    if (pair == nullptr) {
        return std::unexpected(type_error(
            std::string(who) + ": " + arg_label + " must be a pair"));
    }
    return std::make_pair(pair->car, pair->cdr);
}

template <typename Payload>
[[nodiscard]] Payload* get_payload(const LispVal value, const EtaNativeObjectVTable* vtable) {
    if (g_runtime.runtime_context == nullptr || g_runtime.get_native_object == nullptr) {
        return nullptr;
    }
    return static_cast<Payload*>(g_runtime.get_native_object(
        g_runtime.runtime_context,
        value,
        vtable));
}

template <typename Payload>
[[nodiscard]] PrimitiveResult alloc_payload(const EtaNativeObjectVTable* vtable,
                                            std::unique_ptr<Payload> payload_ptr) {
    if (g_runtime.runtime_context == nullptr || g_runtime.alloc_native_object == nullptr) {
        return std::unexpected(internal_error("http: native object allocator is unavailable"));
    }

    std::uint64_t raw_value = 0;
    const int status = g_runtime.alloc_native_object(
        g_runtime.runtime_context,
        vtable,
        payload_ptr.get(),
        &raw_value);
    if (status != ETA_NATIVE_STATUS_OK) {
        return std::unexpected(internal_error("http: failed to allocate native object"));
    }

    payload_ptr.release();
    return static_cast<LispVal>(raw_value);
}

[[nodiscard]] std::expected<std::shared_ptr<HttpSession>, RuntimeError> decode_session(
    const LispVal value,
    const char* who,
    const char* arg_label) {
    auto* payload_handle = get_payload<SessionHandle>(value, &kSessionVTable);
    if (payload_handle == nullptr || payload_handle->session == nullptr) {
        return std::unexpected(type_error(
            std::string(who) + ": " + arg_label + " must be an http session"));
    }
    return payload_handle->session;
}

[[nodiscard]] std::expected<std::shared_ptr<HttpRequest>, RuntimeError> decode_request(
    const LispVal value,
    const char* who,
    const char* arg_label) {
    auto* payload_handle = get_payload<RequestHandle>(value, &kRequestVTable);
    if (payload_handle == nullptr || payload_handle->request == nullptr) {
        return std::unexpected(type_error(
            std::string(who) + ": " + arg_label + " must be an http request"));
    }
    return payload_handle->request;
}

[[nodiscard]] std::expected<std::shared_ptr<HttpResponse>, RuntimeError> decode_response(
    const LispVal value,
    const char* who,
    const char* arg_label) {
    auto* payload_handle = get_payload<ResponseHandle>(value, &kResponseVTable);
    if (payload_handle == nullptr || payload_handle->response == nullptr) {
        return std::unexpected(type_error(
            std::string(who) + ": " + arg_label + " must be an http response"));
    }
    return payload_handle->response;
}

[[nodiscard]] std::expected<void, RuntimeError> apply_session_option(
    const std::shared_ptr<HttpSession>& session,
    const std::string& option_name,
    const LispVal option_value,
    const char* who) {
    auto normalized_name = to_lower_ascii(option_name);

    if (option_value == True || option_value == False) {
        auto bool_value = decode_boolean(option_value, who, "option value");
        if (!bool_value) return std::unexpected(bool_value.error());
        auto status = session->set_boolean_option(normalized_name, *bool_value);
        if (!status) return std::unexpected(user_error(status.error()));
        return {};
    }

    auto maybe_integer = decode_integer(option_value, who, "option value");
    if (maybe_integer) {
        auto status = session->set_integer_option(normalized_name, *maybe_integer);
        if (!status) return std::unexpected(user_error(status.error()));
        return {};
    }

    auto maybe_text = decode_symbol_or_string(option_value, who, "option value");
    if (maybe_text) {
        std::expected<void, std::string> status = std::unexpected("unsupported option value");
        if (normalized_name == "http-version") {
            status = session->set_http_version(*maybe_text);
        } else {
            status = session->set_string_option(normalized_name, *maybe_text);
        }
        if (!status) return std::unexpected(user_error(status.error()));
        return {};
    }

    return std::unexpected(type_error(
        std::string(who) + ": option value must be boolean, integer, symbol, or string"));
}

[[nodiscard]] std::expected<void, RuntimeError> apply_request_option(
    const std::shared_ptr<HttpRequest>& request,
    const std::string& option_name,
    const LispVal option_value,
    const char* who) {
    auto normalized_name = to_lower_ascii(option_name);

    if (option_value == True || option_value == False) {
        auto bool_value = decode_boolean(option_value, who, "option value");
        if (!bool_value) return std::unexpected(bool_value.error());
        auto status = request->set_boolean_option(normalized_name, *bool_value);
        if (!status) return std::unexpected(user_error(status.error()));
        return {};
    }

    auto maybe_integer = decode_integer(option_value, who, "option value");
    if (maybe_integer) {
        auto status = request->set_integer_option(normalized_name, *maybe_integer);
        if (!status) return std::unexpected(user_error(status.error()));
        return {};
    }

    auto maybe_text = decode_symbol_or_string(option_value, who, "option value");
    if (maybe_text) {
        std::expected<void, std::string> status = std::unexpected("unsupported option value");
        if (normalized_name == "http-version") {
            status = request->set_http_version(*maybe_text);
        } else {
            status = request->set_string_option(normalized_name, *maybe_text);
        }
        if (!status) return std::unexpected(user_error(status.error()));
        return {};
    }

    return std::unexpected(type_error(
        std::string(who) + ": option value must be boolean, integer, symbol, or string"));
}

[[nodiscard]] std::expected<std::vector<std::pair<std::string, std::string>>, RuntimeError>
decode_form_fields(
    const LispVal value,
    const char* who,
    const char* arg_label) {
    auto list_values = decode_list(value, who, arg_label);
    if (!list_values) return std::unexpected(list_values.error());

    std::vector<std::pair<std::string, std::string>> fields;
    fields.reserve(list_values->size());
    for (const auto entry : *list_values) {
        auto pair = decode_pair(entry, who, "form field");
        if (!pair) return std::unexpected(pair.error());

        auto key = decode_symbol_or_string(pair->first, who, "form key");
        if (!key) return std::unexpected(key.error());
        auto field_value = decode_symbol_or_string(pair->second, who, "form value");
        if (!field_value) return std::unexpected(field_value.error());
        fields.emplace_back(std::move(*key), std::move(*field_value));
    }
    return fields;
}

[[nodiscard]] std::expected<std::vector<HttpMultipartPart>, RuntimeError> decode_multipart_parts(
    const LispVal value,
    const char* who,
    const char* arg_label) {
    auto part_values = decode_list(value, who, arg_label);
    if (!part_values) return std::unexpected(part_values.error());

    std::vector<HttpMultipartPart> parts;
    parts.reserve(part_values->size());
    for (const auto part_value : *part_values) {
        auto part_fields = decode_list(part_value, who, "multipart part");
        if (!part_fields) return std::unexpected(part_fields.error());
        if (part_fields->size() < 2u || part_fields->size() > 4u) {
            return std::unexpected(type_error(
                std::string(who) + ": multipart part must contain 2 to 4 fields"));
        }

        auto name = decode_symbol_or_string((*part_fields)[0], who, "multipart name");
        if (!name) return std::unexpected(name.error());

        HttpMultipartPart part{};
        part.name = std::move(*name);
        if (part_fields->size() == 2u) {
            auto data = decode_bytes_or_string((*part_fields)[1], who, "multipart data");
            if (!data) return std::unexpected(data.error());
            part.data = std::move(*data);
        } else if (part_fields->size() == 3u) {
            auto filename = decode_string((*part_fields)[1], who, "multipart filename");
            if (!filename) return std::unexpected(filename.error());
            part.filename = std::move(*filename);

            auto data = decode_bytes_or_string((*part_fields)[2], who, "multipart data");
            if (!data) return std::unexpected(data.error());
            part.data = std::move(*data);
        } else {
            auto filename = decode_string((*part_fields)[1], who, "multipart filename");
            if (!filename) return std::unexpected(filename.error());
            part.filename = std::move(*filename);

            auto content_type = decode_string((*part_fields)[2], who, "multipart content-type");
            if (!content_type) return std::unexpected(content_type.error());
            part.content_type = std::move(*content_type);

            auto data = decode_bytes_or_string((*part_fields)[3], who, "multipart data");
            if (!data) return std::unexpected(data.error());
            part.data = std::move(*data);
        }

        parts.push_back(std::move(part));
    }
    return parts;
}

PrimitiveResult primitive_http_version(const PrimitiveArgs args) {
    if (!args.empty()) {
        return std::unexpected(type_error("http/version: expected no arguments"));
    }

    const curl_version_info_data* info = curl_version_info(CURLVERSION_NOW);
    if (info == nullptr) {
        return std::unexpected(internal_error("http/version: curl_version_info returned null"));
    }

    auto version = make_eta_string(info->version != nullptr ? info->version : "");
    if (!version) return std::unexpected(version.error());

    std::vector<LispVal> protocol_values;
    if (info->protocols != nullptr) {
        for (const char* const* protocol = info->protocols; *protocol != nullptr; ++protocol) {
            auto protocol_value = make_eta_string(*protocol);
            if (!protocol_value) return std::unexpected(protocol_value.error());
            protocol_values.push_back(*protocol_value);
        }
    }
    auto protocols = make_eta_list(protocol_values);
    if (!protocols) return std::unexpected(protocols.error());

    std::vector<std::string_view> feature_names;
#ifdef CURL_VERSION_SSL
    if ((info->features & CURL_VERSION_SSL) != 0u) feature_names.push_back("ssl");
#endif
#ifdef CURL_VERSION_LIBZ
    if ((info->features & CURL_VERSION_LIBZ) != 0u) feature_names.push_back("libz");
#endif
#ifdef CURL_VERSION_IPV6
    if ((info->features & CURL_VERSION_IPV6) != 0u) feature_names.push_back("ipv6");
#endif
#ifdef CURL_VERSION_ASYNCHDNS
    if ((info->features & CURL_VERSION_ASYNCHDNS) != 0u) feature_names.push_back("asynchdns");
#endif
#ifdef CURL_VERSION_HTTP2
    if ((info->features & CURL_VERSION_HTTP2) != 0u) feature_names.push_back("http2");
#endif
#ifdef CURL_VERSION_BROTLI
    if ((info->features & CURL_VERSION_BROTLI) != 0u) feature_names.push_back("brotli");
#endif
#ifdef CURL_VERSION_ZSTD
    if ((info->features & CURL_VERSION_ZSTD) != 0u) feature_names.push_back("zstd");
#endif
#ifdef CURL_VERSION_HSTS
    if ((info->features & CURL_VERSION_HSTS) != 0u) feature_names.push_back("hsts");
#endif

    std::vector<LispVal> feature_values;
    feature_values.reserve(feature_names.size());
    for (const auto name : feature_names) {
        auto value = make_eta_symbol(name);
        if (!value) return std::unexpected(value.error());
        feature_values.push_back(*value);
    }
    auto features = make_eta_list(feature_values);
    if (!features) return std::unexpected(features.error());

    std::vector<LispVal> triple_values;
    triple_values.push_back(*version);
    triple_values.push_back(*protocols);
    triple_values.push_back(*features);
    return make_eta_list(triple_values);
}

[[nodiscard]] std::string default_user_agent() {
    std::string user_agent = std::string("eta-http/") + kExtensionVersion;
    const curl_version_info_data* info = curl_version_info(CURLVERSION_NOW);
    if (info != nullptr && info->version != nullptr && info->version[0] != '\0') {
        user_agent += " libcurl/";
        user_agent += info->version;
    }
    return user_agent;
}

PrimitiveResult primitive_session_new(const PrimitiveArgs args) {
    if (!args.empty()) {
        return std::unexpected(type_error("http/session-new: expected no arguments"));
    }

    auto session = std::make_shared<HttpSession>(default_user_agent());
    auto initialized = session->initialize();
    if (!initialized) {
        return std::unexpected(user_error(initialized.error()));
    }

    auto payload_handle = std::make_unique<SessionHandle>();
    payload_handle->session = std::move(session);
    return alloc_payload(&kSessionVTable, std::move(payload_handle));
}

PrimitiveResult primitive_session_close(const PrimitiveArgs args) {
    if (args.size() != 1u) {
        return std::unexpected(type_error("http/session-close!: expected session handle"));
    }

    auto* payload_handle = get_payload<SessionHandle>(args[0], &kSessionVTable);
    if (payload_handle == nullptr) {
        return std::unexpected(type_error(
            "http/session-close!: first argument must be an http session"));
    }
    payload_handle->session.reset();
    return True;
}

PrimitiveResult primitive_session_predicate(const PrimitiveArgs args) {
    if (args.size() != 1u) {
        return std::unexpected(type_error("http/session?: expected one argument"));
    }
    auto* payload_handle = get_payload<SessionHandle>(args[0], &kSessionVTable);
    return (payload_handle != nullptr && payload_handle->session != nullptr) ? True : False;
}

PrimitiveResult primitive_request_predicate(const PrimitiveArgs args) {
    if (args.size() != 1u) {
        return std::unexpected(type_error("http/request?: expected one argument"));
    }
    auto* payload_handle = get_payload<RequestHandle>(args[0], &kRequestVTable);
    return (payload_handle != nullptr && payload_handle->request != nullptr) ? True : False;
}

PrimitiveResult primitive_response_predicate(const PrimitiveArgs args) {
    if (args.size() != 1u) {
        return std::unexpected(type_error("http/response?: expected one argument"));
    }
    auto* payload_handle = get_payload<ResponseHandle>(args[0], &kResponseVTable);
    return (payload_handle != nullptr && payload_handle->response != nullptr) ? True : False;
}

PrimitiveResult primitive_session_set_option(const PrimitiveArgs args) {
    if (args.size() != 3u) {
        return std::unexpected(type_error(
            "http/session-set-option!: expected session, option name, and option value"));
    }

    auto session = decode_session(args[0], "http/session-set-option!", "first argument");
    if (!session) return std::unexpected(session.error());

    auto option_name = decode_symbol_or_string(
        args[1],
        "http/session-set-option!",
        "second argument");
    if (!option_name) return std::unexpected(option_name.error());

    auto status = apply_session_option(*session, *option_name, args[2], "http/session-set-option!");
    if (!status) return std::unexpected(status.error());
    return args[0];
}

PrimitiveResult primitive_session_get_option(const PrimitiveArgs args) {
    if (args.size() != 2u) {
        return std::unexpected(type_error(
            "http/session-get-option: expected session and option name"));
    }

    auto session = decode_session(args[0], "http/session-get-option", "first argument");
    if (!session) return std::unexpected(session.error());

    auto option_name = decode_symbol_or_string(
        args[1],
        "http/session-get-option",
        "second argument");
    if (!option_name) return std::unexpected(option_name.error());

    auto option = (*session)->get_option(*option_name);
    if (!option) {
        return std::unexpected(user_error(option.error()));
    }

    switch (option->kind) {
        case HttpOptionValue::Kind::Boolean:
            return option->bool_value ? True : False;
        case HttpOptionValue::Kind::Integer:
            return make_eta_fixnum(static_cast<std::int64_t>(option->int_value));
        case HttpOptionValue::Kind::String:
            return make_eta_string(option->text);
        case HttpOptionValue::Kind::Symbol:
            return make_eta_symbol(option->text);
    }

    return std::unexpected(internal_error("http/session-get-option: unsupported option value kind"));
}

PrimitiveResult primitive_request_new(const PrimitiveArgs args) {
    if (args.size() != 2u) {
        return std::unexpected(type_error(
            "http/request-new: expected session handle and method"));
    }

    auto session = decode_session(
        args[0],
        "http/request-new",
        "first argument");
    if (!session) return std::unexpected(session.error());

    auto method = decode_symbol_or_string(
        args[1],
        "http/request-new",
        "second argument");
    if (!method) return std::unexpected(method.error());
    if (method->empty()) {
        return std::unexpected(type_error("http/request-new: method must not be empty"));
    }

    auto request = std::make_shared<HttpRequest>(*session, to_upper_ascii(std::move(*method)));
    auto initialized = request->initialize();
    if (!initialized) {
        return std::unexpected(user_error(initialized.error()));
    }

    auto payload_handle = std::make_unique<RequestHandle>();
    payload_handle->request = std::move(request);
    return alloc_payload(&kRequestVTable, std::move(payload_handle));
}

PrimitiveResult primitive_request_set_option(const PrimitiveArgs args) {
    if (args.size() != 3u) {
        return std::unexpected(type_error(
            "http/request-set-option!: expected request, option name, and option value"));
    }

    auto request = decode_request(args[0], "http/request-set-option!", "first argument");
    if (!request) return std::unexpected(request.error());

    auto option_name = decode_symbol_or_string(
        args[1],
        "http/request-set-option!",
        "second argument");
    if (!option_name) return std::unexpected(option_name.error());

    auto status = apply_request_option(*request, *option_name, args[2], "http/request-set-option!");
    if (!status) return std::unexpected(status.error());
    return args[0];
}

PrimitiveResult primitive_request_set_url(const PrimitiveArgs args) {
    if (args.size() != 2u) {
        return std::unexpected(type_error(
            "http/request-set-url!: expected request handle and URL string"));
    }

    auto request = decode_request(
        args[0],
        "http/request-set-url!",
        "first argument");
    if (!request) return std::unexpected(request.error());

    auto url = decode_string(
        args[1],
        "http/request-set-url!",
        "second argument");
    if (!url) return std::unexpected(url.error());

    auto set_status = (*request)->set_url(*url);
    if (!set_status) {
        return std::unexpected(user_error(set_status.error()));
    }
    return args[0];
}

PrimitiveResult primitive_request_set_header(const PrimitiveArgs args) {
    if (args.size() != 3u) {
        return std::unexpected(type_error(
            "http/request-set-header!: expected request, header name, and value/#f"));
    }

    auto request = decode_request(args[0], "http/request-set-header!", "first argument");
    if (!request) return std::unexpected(request.error());

    auto name = decode_string(args[1], "http/request-set-header!", "second argument");
    if (!name) return std::unexpected(name.error());

    std::optional<std::string> value{};
    if (args[2] != False) {
        auto header_value = decode_string(args[2], "http/request-set-header!", "third argument");
        if (!header_value) return std::unexpected(header_value.error());
        value = std::move(*header_value);
    }

    auto status = (*request)->set_header(std::move(*name), std::move(value));
    if (!status) return std::unexpected(user_error(status.error()));
    return args[0];
}

PrimitiveResult primitive_request_set_body_bytes(const PrimitiveArgs args) {
    if (args.size() != 2u) {
        return std::unexpected(type_error(
            "http/request-set-body-bytes!: expected request and bytevector"));
    }

    auto request = decode_request(args[0], "http/request-set-body-bytes!", "first argument");
    if (!request) return std::unexpected(request.error());

    auto bytes = decode_bytevector(args[1], "http/request-set-body-bytes!", "second argument");
    if (!bytes) return std::unexpected(bytes.error());

    auto status = (*request)->set_body_bytes(std::move(*bytes));
    if (!status) return std::unexpected(user_error(status.error()));
    return args[0];
}

PrimitiveResult primitive_request_set_body_string(const PrimitiveArgs args) {
    if (args.size() != 3u) {
        return std::unexpected(type_error(
            "http/request-set-body-string!: expected request, string body, and charset"));
    }

    auto request = decode_request(args[0], "http/request-set-body-string!", "first argument");
    if (!request) return std::unexpected(request.error());

    auto text = decode_string(args[1], "http/request-set-body-string!", "second argument");
    if (!text) return std::unexpected(text.error());
    auto charset = decode_string(args[2], "http/request-set-body-string!", "third argument");
    if (!charset) return std::unexpected(charset.error());

    auto status = (*request)->set_body_string(std::move(*text), std::move(*charset));
    if (!status) return std::unexpected(user_error(status.error()));
    return args[0];
}

PrimitiveResult primitive_request_set_body_file(const PrimitiveArgs args) {
    if (args.size() != 2u) {
        return std::unexpected(type_error(
            "http/request-set-body-file!: expected request and file path"));
    }

    auto request = decode_request(args[0], "http/request-set-body-file!", "first argument");
    if (!request) return std::unexpected(request.error());

    auto path = decode_string(args[1], "http/request-set-body-file!", "second argument");
    if (!path) return std::unexpected(path.error());

    auto status = (*request)->set_body_file(std::move(*path));
    if (!status) return std::unexpected(user_error(status.error()));
    return args[0];
}

PrimitiveResult primitive_request_set_body_form(const PrimitiveArgs args) {
    if (args.size() != 2u) {
        return std::unexpected(type_error(
            "http/request-set-body-form!: expected request and form alist"));
    }

    auto request = decode_request(args[0], "http/request-set-body-form!", "first argument");
    if (!request) return std::unexpected(request.error());

    auto form_fields = decode_form_fields(args[1], "http/request-set-body-form!", "second argument");
    if (!form_fields) return std::unexpected(form_fields.error());

    auto status = (*request)->set_body_form(std::move(*form_fields));
    if (!status) return std::unexpected(user_error(status.error()));
    return args[0];
}

PrimitiveResult primitive_request_set_body_multipart(const PrimitiveArgs args) {
    if (args.size() != 2u) {
        return std::unexpected(type_error(
            "http/request-set-body-multipart!: expected request and multipart parts"));
    }

    auto request = decode_request(args[0], "http/request-set-body-multipart!", "first argument");
    if (!request) return std::unexpected(request.error());

    auto parts = decode_multipart_parts(args[1], "http/request-set-body-multipart!", "second argument");
    if (!parts) return std::unexpected(parts.error());

    auto status = (*request)->set_body_multipart(std::move(*parts));
    if (!status) return std::unexpected(user_error(status.error()));
    return args[0];
}

PrimitiveResult primitive_perform(const PrimitiveArgs args) {
    if (args.size() != 1u) {
        return std::unexpected(type_error("http/perform: expected request handle"));
    }

    auto request = decode_request(args[0], "http/perform", "first argument");
    if (!request) return std::unexpected(request.error());

    auto response = (*request)->perform();
    if (!response) return std::unexpected(user_error(response.error()));

    auto payload_handle = std::make_unique<ResponseHandle>();
    payload_handle->response = std::make_shared<HttpResponse>(std::move(*response));
    return alloc_payload(&kResponseVTable, std::move(payload_handle));
}

PrimitiveResult primitive_perform_stream(const PrimitiveArgs args) {
    if (args.size() != 3u) {
        return std::unexpected(type_error(
            "http/perform-stream: expected request, on-headers callback, and on-chunk callback"));
    }

    auto request = decode_request(args[0], "http/perform-stream", "first argument");
    if (!request) return std::unexpected(request.error());

    if (args[1] != False || args[2] != False) {
        return std::unexpected(type_error(
            "http/perform-stream: callback invocation is not available yet; pass #f for callbacks"));
    }

    auto response = (*request)->perform_stream();
    if (!response) return std::unexpected(user_error(response.error()));

    auto payload_handle = std::make_unique<ResponseHandle>();
    payload_handle->response = std::make_shared<HttpResponse>(std::move(*response));
    return alloc_payload(&kResponseVTable, std::move(payload_handle));
}

PrimitiveResult primitive_download(const PrimitiveArgs args) {
    if (args.size() != 3u) {
        return std::unexpected(type_error(
            "http/download: expected URL string, output path, and options alist"));
    }

    auto url = decode_string(args[0], "http/download", "first argument");
    if (!url) return std::unexpected(url.error());

    auto output_path = decode_string(args[1], "http/download", "second argument");
    if (!output_path) return std::unexpected(output_path.error());

    auto option_entries = decode_list(args[2], "http/download", "third argument");
    if (!option_entries) return std::unexpected(option_entries.error());

    auto session = std::make_shared<HttpSession>(default_user_agent());
    auto initialized = session->initialize();
    if (!initialized) return std::unexpected(user_error(initialized.error()));

    for (const auto entry : *option_entries) {
        auto pair = decode_pair(entry, "http/download", "option entry");
        if (!pair) return std::unexpected(pair.error());

        auto option_name = decode_symbol_or_string(pair->first, "http/download", "option name");
        if (!option_name) return std::unexpected(option_name.error());

        auto status = apply_session_option(session, *option_name, pair->second, "http/download");
        if (!status) return std::unexpected(status.error());
    }

    auto request = std::make_shared<HttpRequest>(std::move(session), "GET");
    auto request_init = request->initialize();
    if (!request_init) return std::unexpected(user_error(request_init.error()));

    auto set_url_status = request->set_url(*url);
    if (!set_url_status) return std::unexpected(user_error(set_url_status.error()));

    auto response = request->download_to_file(*output_path);
    if (!response) return std::unexpected(user_error(response.error()));

    auto payload_handle = std::make_unique<ResponseHandle>();
    payload_handle->response = std::make_shared<HttpResponse>(std::move(*response));
    return alloc_payload(&kResponseVTable, std::move(payload_handle));
}

PrimitiveResult primitive_response_status(const PrimitiveArgs args) {
    if (args.size() != 1u) {
        return std::unexpected(type_error("http/response-status: expected response handle"));
    }

    auto response = decode_response(
        args[0],
        "http/response-status",
        "first argument");
    if (!response) return std::unexpected(response.error());

    return make_eta_fixnum((*response)->status);
}

PrimitiveResult primitive_response_body_bytes(const PrimitiveArgs args) {
    if (args.size() != 1u) {
        return std::unexpected(type_error(
            "http/response-body-bytes: expected response handle"));
    }

    auto response = decode_response(
        args[0],
        "http/response-body-bytes",
        "first argument");
    if (!response) return std::unexpected(response.error());

    return make_eta_bytevector((*response)->body);
}

PrimitiveResult primitive_response_headers(const PrimitiveArgs args) {
    if (args.size() != 1u) {
        return std::unexpected(type_error(
            "http/response-headers: expected response handle"));
    }

    auto response = decode_response(
        args[0],
        "http/response-headers",
        "first argument");
    if (!response) return std::unexpected(response.error());

    LispVal out = Nil;
    for (auto it = (*response)->headers.rbegin(); it != (*response)->headers.rend(); ++it) {
        auto name = make_eta_string(it->name);
        if (!name) return std::unexpected(name.error());
        auto value = make_eta_string(it->value);
        if (!value) return std::unexpected(value.error());
        auto pair = make_eta_cons(*name, *value);
        if (!pair) return std::unexpected(pair.error());
        auto cell = make_eta_cons(*pair, out);
        if (!cell) return std::unexpected(cell.error());
        out = *cell;
    }
    return out;
}

PrimitiveResult primitive_response_effective_url(const PrimitiveArgs args) {
    if (args.size() != 1u) {
        return std::unexpected(type_error(
            "http/response-effective-url: expected response handle"));
    }

    auto response = decode_response(
        args[0],
        "http/response-effective-url",
        "first argument");
    if (!response) return std::unexpected(response.error());

    return make_eta_string((*response)->effective_url);
}

PrimitiveResult primitive_url_encode(const PrimitiveArgs args) {
    if (args.size() != 1u) {
        return std::unexpected(type_error("http/url-encode: expected one string argument"));
    }

    auto text = decode_string(args[0], "http/url-encode", "first argument");
    if (!text) return std::unexpected(text.error());

    char* encoded = curl_easy_escape(nullptr, text->c_str(), static_cast<int>(text->size()));
    if (encoded == nullptr) {
        return std::unexpected(user_error("http/url-encode: curl_easy_escape failed"));
    }

    auto out = make_eta_string(encoded);
    curl_free(encoded);
    return out;
}

PrimitiveResult primitive_url_decode(const PrimitiveArgs args) {
    if (args.size() != 1u) {
        return std::unexpected(type_error("http/url-decode: expected one string argument"));
    }

    auto text = decode_string(args[0], "http/url-decode", "first argument");
    if (!text) return std::unexpected(text.error());

    int decoded_len = 0;
    char* decoded = curl_easy_unescape(
        nullptr,
        text->c_str(),
        static_cast<int>(text->size()),
        &decoded_len);
    if (decoded == nullptr) {
        return std::unexpected(user_error("http/url-decode: curl_easy_unescape failed"));
    }

    auto out = make_eta_string(std::string_view(decoded, static_cast<std::size_t>(decoded_len)));
    curl_free(decoded);
    return out;
}

PrimitiveResult primitive_url_parse(const PrimitiveArgs args) {
    if (args.size() != 1u) {
        return std::unexpected(type_error("http/url-parse: expected one string argument"));
    }

    auto text = decode_string(args[0], "http/url-parse", "first argument");
    if (!text) return std::unexpected(text.error());

    CURLU* url = curl_url();
    if (url == nullptr) {
        return std::unexpected(user_error("http/url-parse: failed to allocate CURL URL handle"));
    }

    const CURLUcode set_status = curl_url_set(url, CURLUPART_URL, text->c_str(), 0u);
    if (set_status != CURLUE_OK) {
        curl_url_cleanup(url);
        return std::unexpected(user_error("http/url-parse: invalid URL"));
    }

    struct UrlPartSpec {
        const char* key;
        CURLUPart part;
    };
    constexpr UrlPartSpec kParts[] = {
        {"scheme", CURLUPART_SCHEME},
        {"user", CURLUPART_USER},
        {"password", CURLUPART_PASSWORD},
        {"host", CURLUPART_HOST},
        {"port", CURLUPART_PORT},
        {"path", CURLUPART_PATH},
        {"query", CURLUPART_QUERY},
        {"fragment", CURLUPART_FRAGMENT},
    };

    std::vector<LispVal> entries;
    entries.reserve(std::size(kParts));
    for (const auto& spec : kParts) {
        auto key = make_eta_symbol(spec.key);
        if (!key) {
            curl_url_cleanup(url);
            return std::unexpected(key.error());
        }

        LispVal value = False;
        char* part_text = nullptr;
        const CURLUcode get_status = curl_url_get(url, spec.part, &part_text, 0u);
        if (get_status == CURLUE_OK && part_text != nullptr) {
            auto text_value = make_eta_string(part_text);
            curl_free(part_text);
            if (!text_value) {
                curl_url_cleanup(url);
                return std::unexpected(text_value.error());
            }
            value = *text_value;
        }

        auto pair = make_eta_cons(*key, value);
        if (!pair) {
            curl_url_cleanup(url);
            return std::unexpected(pair.error());
        }
        entries.push_back(*pair);
    }

    curl_url_cleanup(url);
    return make_eta_list(entries);
}

PrimitiveResult primitive_url_build(const PrimitiveArgs args) {
    if (args.size() != 1u) {
        return std::unexpected(type_error(
            "http/url-build: expected one URL component alist argument"));
    }

    auto entries = decode_list(args[0], "http/url-build", "first argument");
    if (!entries) return std::unexpected(entries.error());

    CURLU* url = curl_url();
    if (url == nullptr) {
        return std::unexpected(user_error("http/url-build: failed to allocate CURL URL handle"));
    }

    auto set_part = [url](const CURLUPart part,
                          const std::string& value) -> std::expected<void, std::string> {
        const CURLUcode status = curl_url_set(url, part, value.c_str(), 0u);
        if (status != CURLUE_OK) {
            return std::unexpected("http/url-build: failed to set URL component");
        }
        return {};
    };

    for (const auto entry : *entries) {
        auto pair = decode_pair(entry, "http/url-build", "URL component");
        if (!pair) {
            curl_url_cleanup(url);
            return std::unexpected(pair.error());
        }

        auto key = decode_symbol_or_string(pair->first, "http/url-build", "URL component key");
        if (!key) {
            curl_url_cleanup(url);
            return std::unexpected(key.error());
        }
        auto normalized_key = to_lower_ascii(*key);

        if (pair->second == False) {
            continue;
        }

        auto value = decode_string(pair->second, "http/url-build", "URL component value");
        if (!value) {
            curl_url_cleanup(url);
            return std::unexpected(value.error());
        }

        std::expected<void, std::string> set_status = std::unexpected("unknown key");
        if (normalized_key == "scheme") {
            set_status = set_part(CURLUPART_SCHEME, *value);
        } else if (normalized_key == "user" || normalized_key == "username") {
            set_status = set_part(CURLUPART_USER, *value);
        } else if (normalized_key == "password") {
            set_status = set_part(CURLUPART_PASSWORD, *value);
        } else if (normalized_key == "host") {
            set_status = set_part(CURLUPART_HOST, *value);
        } else if (normalized_key == "port") {
            set_status = set_part(CURLUPART_PORT, *value);
        } else if (normalized_key == "path") {
            set_status = set_part(CURLUPART_PATH, *value);
        } else if (normalized_key == "query") {
            set_status = set_part(CURLUPART_QUERY, *value);
        } else if (normalized_key == "fragment") {
            set_status = set_part(CURLUPART_FRAGMENT, *value);
        } else {
            curl_url_cleanup(url);
            return std::unexpected(type_error(
                "http/url-build: unknown URL component key '" + normalized_key + "'"));
        }

        if (!set_status) {
            curl_url_cleanup(url);
            return std::unexpected(user_error(set_status.error()));
        }
    }

    char* built = nullptr;
    const CURLUcode build_status = curl_url_get(url, CURLUPART_URL, &built, 0u);
    if (build_status != CURLUE_OK || built == nullptr) {
        curl_url_cleanup(url);
        return std::unexpected(user_error("http/url-build: failed to render URL"));
    }

    auto out = make_eta_string(built);
    curl_free(built);
    curl_url_cleanup(url);
    return out;
}

PrimitiveFunc g_http_version = primitive_http_version;
PrimitiveFunc g_session_new = primitive_session_new;
PrimitiveFunc g_session_close = primitive_session_close;
PrimitiveFunc g_session_predicate = primitive_session_predicate;
PrimitiveFunc g_request_predicate = primitive_request_predicate;
PrimitiveFunc g_response_predicate = primitive_response_predicate;
PrimitiveFunc g_session_set_option = primitive_session_set_option;
PrimitiveFunc g_session_get_option = primitive_session_get_option;
PrimitiveFunc g_request_new = primitive_request_new;
PrimitiveFunc g_request_set_option = primitive_request_set_option;
PrimitiveFunc g_request_set_url = primitive_request_set_url;
PrimitiveFunc g_request_set_header = primitive_request_set_header;
PrimitiveFunc g_request_set_body_bytes = primitive_request_set_body_bytes;
PrimitiveFunc g_request_set_body_string = primitive_request_set_body_string;
PrimitiveFunc g_request_set_body_file = primitive_request_set_body_file;
PrimitiveFunc g_request_set_body_form = primitive_request_set_body_form;
PrimitiveFunc g_request_set_body_multipart = primitive_request_set_body_multipart;
PrimitiveFunc g_perform = primitive_perform;
PrimitiveFunc g_perform_stream = primitive_perform_stream;
PrimitiveFunc g_download = primitive_download;
PrimitiveFunc g_response_status = primitive_response_status;
PrimitiveFunc g_response_body_bytes = primitive_response_body_bytes;
PrimitiveFunc g_response_headers = primitive_response_headers;
PrimitiveFunc g_response_effective_url = primitive_response_effective_url;
PrimitiveFunc g_url_encode = primitive_url_encode;
PrimitiveFunc g_url_decode = primitive_url_decode;
PrimitiveFunc g_url_parse = primitive_url_parse;
PrimitiveFunc g_url_build = primitive_url_build;

int register_one(const EtaNativeApiV1* api,
                 const char* name,
                 const std::uint32_t arity,
                 const std::uint8_t has_rest,
                 void* callable) {
    if (api == nullptr || api->register_primitive == nullptr) {
        return ETA_NATIVE_STATUS_ERROR;
    }
    return api->register_primitive(api->user_data, name, arity, has_rest, callable);
}

} // namespace

int register_http_primitives(const EtaNativeApiV1* api) {
    if (api == nullptr || api->register_primitive == nullptr) {
        if (api != nullptr && api->report_error != nullptr) {
            api->report_error(
                api->user_data,
                "http sidecar requires register_primitive callback support");
        }
        return ETA_NATIVE_STATUS_ERROR;
    }

    if (!native_object_api_available(api)) {
        if (api->report_error != nullptr) {
            api->report_error(
                api->user_data,
                "http sidecar requires NativeObject API support");
        }
        return ETA_NATIVE_STATUS_ERROR;
    }

    auto* binding = static_cast<eta::native::SidecarRuntimeBindingV1*>(api->runtime_context);
    if (binding == nullptr || binding->heap == nullptr || binding->intern_table == nullptr) {
        if (api->report_error != nullptr) {
            api->report_error(
                api->user_data,
                "http sidecar requires runtime heap and intern table support");
        }
        return ETA_NATIVE_STATUS_ERROR;
    }

    g_runtime.runtime_context = api->runtime_context;
    g_runtime.alloc_native_object = api->alloc_native_object;
    g_runtime.get_native_object = api->get_native_object;
    g_runtime.heap = binding->heap;
    g_runtime.intern_table = binding->intern_table;

    struct PrimitiveSpec {
        const char* name;
        std::uint32_t arity;
        std::uint8_t has_rest;
        void* callable;
    };

    const PrimitiveSpec kSpecs[] = {
        {"http/version", 0u, 0u, static_cast<void*>(&g_http_version)},
        {"http/session-new", 0u, 0u, static_cast<void*>(&g_session_new)},
        {"http/session-close!", 1u, 0u, static_cast<void*>(&g_session_close)},
        {"http/session?", 1u, 0u, static_cast<void*>(&g_session_predicate)},
        {"http/request?", 1u, 0u, static_cast<void*>(&g_request_predicate)},
        {"http/response?", 1u, 0u, static_cast<void*>(&g_response_predicate)},
        {"http/session-set-option!", 3u, 0u, static_cast<void*>(&g_session_set_option)},
        {"http/session-get-option", 2u, 0u, static_cast<void*>(&g_session_get_option)},
        {"http/request-new", 2u, 0u, static_cast<void*>(&g_request_new)},
        {"http/request-set-option!", 3u, 0u, static_cast<void*>(&g_request_set_option)},
        {"http/request-set-url!", 2u, 0u, static_cast<void*>(&g_request_set_url)},
        {"http/request-set-header!", 3u, 0u, static_cast<void*>(&g_request_set_header)},
        {"http/request-set-body-bytes!", 2u, 0u, static_cast<void*>(&g_request_set_body_bytes)},
        {"http/request-set-body-string!", 3u, 0u, static_cast<void*>(&g_request_set_body_string)},
        {"http/request-set-body-file!", 2u, 0u, static_cast<void*>(&g_request_set_body_file)},
        {"http/request-set-body-form!", 2u, 0u, static_cast<void*>(&g_request_set_body_form)},
        {"http/request-set-body-multipart!", 2u, 0u, static_cast<void*>(&g_request_set_body_multipart)},
        {"http/perform", 1u, 0u, static_cast<void*>(&g_perform)},
        {"http/perform-stream", 3u, 0u, static_cast<void*>(&g_perform_stream)},
        {"http/download", 3u, 0u, static_cast<void*>(&g_download)},
        {"http/response-status", 1u, 0u, static_cast<void*>(&g_response_status)},
        {"http/response-body-bytes", 1u, 0u, static_cast<void*>(&g_response_body_bytes)},
        {"http/response-headers", 1u, 0u, static_cast<void*>(&g_response_headers)},
        {"http/response-effective-url", 1u, 0u, static_cast<void*>(&g_response_effective_url)},
        {"http/url-encode", 1u, 0u, static_cast<void*>(&g_url_encode)},
        {"http/url-decode", 1u, 0u, static_cast<void*>(&g_url_decode)},
        {"http/url-parse", 1u, 0u, static_cast<void*>(&g_url_parse)},
        {"http/url-build", 1u, 0u, static_cast<void*>(&g_url_build)},
    };

    for (const auto& spec : kSpecs) {
        if (register_one(api, spec.name, spec.arity, spec.has_rest, spec.callable)
            != ETA_NATIVE_STATUS_OK) {
            return ETA_NATIVE_STATUS_ERROR;
        }
    }

    return ETA_NATIVE_STATUS_OK;
}

} // namespace eta::http_sidecar
