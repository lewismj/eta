#include "eta/http/http_session.h"
#include "eta/http/http_multi.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <iterator>
#include <ostream>
#include <sstream>
#include <utility>

namespace eta::http_sidecar {

namespace {

struct CurlSlistDeleter {
    void operator()(curl_slist* value) const noexcept {
        if (value != nullptr) {
            curl_slist_free_all(value);
        }
    }
};

struct CurlMimeDeleter {
    void operator()(curl_mime* value) const noexcept {
        if (value != nullptr) {
            curl_mime_free(value);
        }
    }
};

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

[[nodiscard]] std::string trim_ascii(std::string_view text) {
    std::size_t start = 0;
    std::size_t end = text.size();
    while (start < end && std::isspace(static_cast<unsigned char>(text[start])) != 0) {
        ++start;
    }
    while (end > start && std::isspace(static_cast<unsigned char>(text[end - 1u])) != 0) {
        --end;
    }
    return std::string(text.substr(start, end - start));
}

[[nodiscard]] bool iequals_ascii(std::string_view lhs, std::string_view rhs) {
    if (lhs.size() != rhs.size()) return false;
    for (std::size_t i = 0; i < lhs.size(); ++i) {
        const auto left = static_cast<unsigned char>(lhs[i]);
        const auto right = static_cast<unsigned char>(rhs[i]);
        if (std::tolower(left) != std::tolower(right)) return false;
    }
    return true;
}

[[nodiscard]] bool contains_header_name(const std::vector<HttpHeader>& headers, std::string_view name) {
    return std::any_of(headers.begin(), headers.end(), [name](const HttpHeader& header) {
        return iequals_ascii(header.name, name);
    });
}

[[nodiscard]] std::string curl_error_text(std::string_view prefix,
                                          const CURLcode code,
                                          const std::array<char, CURL_ERROR_SIZE>& buffer) {
    std::ostringstream out;
    out << prefix << ": " << static_cast<int>(code);
    if (buffer[0] != '\0') {
        out << " (" << buffer.data() << ")";
    } else {
        out << " (" << curl_easy_strerror(code) << ")";
    }
    return out.str();
}

[[nodiscard]] std::expected<HttpVersion, std::string> parse_http_version(
    const std::string_view who,
    std::string value) {
    value = to_lower_ascii(std::move(value));
    if (value == "any") return HttpVersion::Any;
    if (value == "http/1.1" || value == "http1.1") return HttpVersion::Http11;
    if (value == "http/2" || value == "http2") return HttpVersion::Http2;
    if (value == "http/2-tls" || value == "http/2tls" || value == "http2-tls") {
        return HttpVersion::Http2Tls;
    }
    return std::unexpected(std::string(who) + ": unsupported http-version value '" + value + "'");
}

[[nodiscard]] HttpOptionValue make_boolean_option(const bool value) {
    return HttpOptionValue{
        .kind = HttpOptionValue::Kind::Boolean,
        .bool_value = value,
    };
}

[[nodiscard]] HttpOptionValue make_integer_option(const long value) {
    return HttpOptionValue{
        .kind = HttpOptionValue::Kind::Integer,
        .int_value = value,
    };
}

[[nodiscard]] HttpOptionValue make_string_option(std::string value) {
    return HttpOptionValue{
        .kind = HttpOptionValue::Kind::String,
        .text = std::move(value),
    };
}

[[nodiscard]] HttpOptionValue make_symbol_option(std::string value) {
    return HttpOptionValue{
        .kind = HttpOptionValue::Kind::Symbol,
        .text = std::move(value),
    };
}

[[nodiscard]] std::string http_version_symbol(const HttpVersion version) {
    switch (version) {
        case HttpVersion::Any: return "any";
        case HttpVersion::Http11: return "http/1.1";
        case HttpVersion::Http2: return "http/2";
        case HttpVersion::Http2Tls: return "http/2-tls";
    }
    return "http/2-tls";
}

template <typename T>
[[nodiscard]] std::expected<void, std::string> set_common_bool_option(
    T& target,
    const std::string& normalized_name,
    const bool value,
    const char* who) {
    if (normalized_name == "follow-redirects") {
        target.follow_redirects = value;
        return {};
    }
    if (normalized_name == "verify-tls") {
        target.verify_tls = value;
        return {};
    }
    if (normalized_name == "verbose") {
        target.verbose = value;
        return {};
    }
    return std::unexpected(
        std::string(who) + ": option '" + normalized_name + "' expects a non-boolean value or is unknown");
}

template <typename T>
[[nodiscard]] std::expected<void, std::string> set_common_integer_option(
    T& target,
    const std::string& normalized_name,
    const long value,
    const char* who) {
    if (value < 0L) {
        return std::unexpected(
            std::string(who) + ": integer options must be non-negative");
    }

    if (normalized_name == "max-redirects") {
        target.max_redirects = value;
        return {};
    }
    if (normalized_name == "connect-timeout-ms") {
        target.connect_timeout_ms = value;
        return {};
    }
    if (normalized_name == "timeout-ms") {
        target.timeout_ms = value;
        return {};
    }
    if (normalized_name == "low-speed-limit-bps") {
        target.low_speed_limit_bps = value;
        return {};
    }
    if (normalized_name == "low-speed-time-s") {
        target.low_speed_time_s = value;
        return {};
    }
    return std::unexpected(
        std::string(who) + ": option '" + normalized_name + "' expects a non-integer value or is unknown");
}

template <typename T>
[[nodiscard]] std::expected<void, std::string> set_common_string_option(
    T& target,
    const std::string& normalized_name,
    std::string value,
    const char* who) {
    if (normalized_name == "user-agent") {
        target.user_agent = std::move(value);
        return {};
    }
    if (normalized_name == "accept-encoding") {
        target.accept_encoding = std::move(value);
        return {};
    }
    if (normalized_name == "ca-bundle") {
        target.ca_bundle = std::move(value);
        return {};
    }
    if (normalized_name == "ca-path") {
        target.ca_path = std::move(value);
        return {};
    }
    if (normalized_name == "client-cert") {
        target.client_cert = std::move(value);
        return {};
    }
    if (normalized_name == "client-key") {
        target.client_key = std::move(value);
        return {};
    }
    if (normalized_name == "username") {
        target.username = std::move(value);
        return {};
    }
    if (normalized_name == "password") {
        target.password = std::move(value);
        return {};
    }
    if (normalized_name == "bearer-token") {
        target.bearer_token = std::move(value);
        return {};
    }
    if (normalized_name == "proxy") {
        target.proxy = std::move(value);
        return {};
    }
    if (normalized_name == "cookie-jar") {
        target.cookie_jar = std::move(value);
        return {};
    }
    if (normalized_name == "cookie-file") {
        target.cookie_file = std::move(value);
        return {};
    }
    if (normalized_name == "unix-socket-path") {
        target.unix_socket_path = std::move(value);
        return {};
    }
    return std::unexpected(
        std::string(who) + ": option '" + normalized_name + "' expects a non-string value or is unknown");
}

template <typename Target, typename Value>
void apply_override(Value& target, const std::optional<Target>& source) {
    if (source.has_value()) {
        target = *source;
    }
}

[[nodiscard]] std::expected<std::vector<std::uint8_t>, std::string> read_file_body(
    const std::string& path,
    std::string_view operation) {
    std::ifstream input(path, std::ios::binary);
    if (!input.is_open()) {
        return std::unexpected(std::string(operation) + ": failed to open request body file '" + path + "'");
    }
    std::vector<std::uint8_t> bytes{
        std::istreambuf_iterator<char>{input},
        std::istreambuf_iterator<char>{},
    };
    if (input.bad()) {
        return std::unexpected(std::string(operation) + ": failed to read request body file '" + path + "'");
    }
    return bytes;
}

[[nodiscard]] std::expected<std::vector<std::uint8_t>, std::string> encode_form_fields(
    CURL* easy,
    const std::vector<std::pair<std::string, std::string>>& fields,
    std::string_view operation) {
    std::string encoded;
    for (std::size_t i = 0; i < fields.size(); ++i) {
        const auto& [key, value] = fields[i];
        char* escaped_key = curl_easy_escape(easy, key.c_str(), static_cast<int>(key.size()));
        if (escaped_key == nullptr) {
            return std::unexpected(std::string(operation) + ": failed to URL-encode form key");
        }
        char* escaped_value = curl_easy_escape(easy, value.c_str(), static_cast<int>(value.size()));
        if (escaped_value == nullptr) {
            curl_free(escaped_key);
            return std::unexpected(std::string(operation) + ": failed to URL-encode form value");
        }

        if (i > 0u) {
            encoded.push_back('&');
        }
        encoded.append(escaped_key);
        encoded.push_back('=');
        encoded.append(escaped_value);

        curl_free(escaped_key);
        curl_free(escaped_value);
    }

    return std::vector<std::uint8_t>(encoded.begin(), encoded.end());
}

[[nodiscard]] std::expected<long, std::string> curl_http_version(
    const HttpVersion version,
    std::string_view operation) {
    const curl_version_info_data* info = curl_version_info(CURLVERSION_NOW);
#ifdef CURL_VERSION_HTTP2
    const bool has_http2 = info != nullptr && (info->features & CURL_VERSION_HTTP2) != 0u;
#else
    const bool has_http2 = false;
#endif

    switch (version) {
        case HttpVersion::Any:
#ifdef CURL_HTTP_VERSION_NONE
            return CURL_HTTP_VERSION_NONE;
#else
            return CURL_HTTP_VERSION_NONE;
#endif
        case HttpVersion::Http11: return CURL_HTTP_VERSION_1_1;
        case HttpVersion::Http2:
            return has_http2 ? CURL_HTTP_VERSION_2_0 : CURL_HTTP_VERSION_1_1;
        case HttpVersion::Http2Tls:
            if (!has_http2) return CURL_HTTP_VERSION_1_1;
#ifdef CURL_HTTP_VERSION_2TLS
            return CURL_HTTP_VERSION_2TLS;
#else
            return CURL_HTTP_VERSION_2_0;
#endif
    }
    return std::unexpected(std::string(operation) + ": unsupported HTTP version enum value");
}

} // namespace

HttpSession::HttpSession(std::string default_user_agent)
    : default_user_agent_(std::move(default_user_agent)) {
    options_.user_agent = default_user_agent_;
    options_.accept_encoding = "";
}

HttpSession::~HttpSession() {
    if (share_ != nullptr) {
        curl_share_cleanup(share_);
        share_ = nullptr;
    }
}

void HttpSession::lock_callback(CURL*,
                                const curl_lock_data,
                                const curl_lock_access,
                                void* userptr) {
    auto* self = static_cast<HttpSession*>(userptr);
    if (self == nullptr) return;
    self->share_mutex_.lock();
}

void HttpSession::unlock_callback(CURL*,
                                  const curl_lock_data,
                                  void* userptr) {
    auto* self = static_cast<HttpSession*>(userptr);
    if (self == nullptr) return;
    self->share_mutex_.unlock();
}

std::expected<void, std::string> HttpSession::initialize() {
    if (initialized_) return {};

    share_ = curl_share_init();
    if (share_ == nullptr) {
        return std::unexpected("http/session-new: failed to allocate CURLSH handle");
    }

    const auto lock_status =
        curl_share_setopt(share_, CURLSHOPT_LOCKFUNC, &HttpSession::lock_callback);
    if (lock_status != CURLSHE_OK) {
        curl_share_cleanup(share_);
        share_ = nullptr;
        return std::unexpected("http/session-new: failed to configure CURL share lock callback");
    }

    const auto unlock_status =
        curl_share_setopt(share_, CURLSHOPT_UNLOCKFUNC, &HttpSession::unlock_callback);
    if (unlock_status != CURLSHE_OK) {
        curl_share_cleanup(share_);
        share_ = nullptr;
        return std::unexpected("http/session-new: failed to configure CURL share unlock callback");
    }

    const auto userdata_status = curl_share_setopt(share_, CURLSHOPT_USERDATA, this);
    if (userdata_status != CURLSHE_OK) {
        curl_share_cleanup(share_);
        share_ = nullptr;
        return std::unexpected("http/session-new: failed to configure CURL share userdata");
    }

    const auto cookie_status = curl_share_setopt(share_, CURLSHOPT_SHARE, CURL_LOCK_DATA_COOKIE);
    if (cookie_status != CURLSHE_OK) {
        curl_share_cleanup(share_);
        share_ = nullptr;
        return std::unexpected("http/session-new: failed to enable CURL cookie sharing");
    }

    const auto dns_status = curl_share_setopt(share_, CURLSHOPT_SHARE, CURL_LOCK_DATA_DNS);
    if (dns_status != CURLSHE_OK) {
        curl_share_cleanup(share_);
        share_ = nullptr;
        return std::unexpected("http/session-new: failed to enable CURL DNS sharing");
    }

    initialized_ = true;
    return {};
}

std::expected<void, std::string> HttpSession::set_boolean_option(
    const std::string_view name,
    const bool value) {
    const std::string normalized_name = to_lower_ascii(std::string(name));
    return set_common_bool_option(options_, normalized_name, value, "http/session-set-option!");
}

std::expected<void, std::string> HttpSession::set_integer_option(
    const std::string_view name,
    const long value) {
    const std::string normalized_name = to_lower_ascii(std::string(name));
    return set_common_integer_option(options_, normalized_name, value, "http/session-set-option!");
}

std::expected<void, std::string> HttpSession::set_string_option(
    const std::string_view name,
    std::string value) {
    const std::string normalized_name = to_lower_ascii(std::string(name));
    return set_common_string_option(
        options_,
        normalized_name,
        std::move(value),
        "http/session-set-option!");
}

std::expected<void, std::string> HttpSession::set_http_version(const std::string_view value) {
    auto parsed = parse_http_version("http/session-set-option!", std::string(value));
    if (!parsed) return std::unexpected(parsed.error());
    options_.http_version = *parsed;
    return {};
}

std::expected<HttpOptionValue, std::string> HttpSession::get_option(const std::string_view name) const {
    const std::string normalized_name = to_lower_ascii(std::string(name));
    if (normalized_name == "follow-redirects") return make_boolean_option(options_.follow_redirects);
    if (normalized_name == "max-redirects") return make_integer_option(options_.max_redirects);
    if (normalized_name == "connect-timeout-ms") return make_integer_option(options_.connect_timeout_ms);
    if (normalized_name == "timeout-ms") return make_integer_option(options_.timeout_ms);
    if (normalized_name == "user-agent") return make_string_option(options_.user_agent);
    if (normalized_name == "accept-encoding") return make_string_option(options_.accept_encoding);
    if (normalized_name == "verify-tls") return make_boolean_option(options_.verify_tls);
    if (normalized_name == "http-version") return make_symbol_option(http_version_symbol(options_.http_version));
    if (normalized_name == "ca-bundle") return make_string_option(options_.ca_bundle);
    if (normalized_name == "ca-path") return make_string_option(options_.ca_path);
    if (normalized_name == "client-cert") return make_string_option(options_.client_cert);
    if (normalized_name == "client-key") return make_string_option(options_.client_key);
    if (normalized_name == "username") return make_string_option(options_.username);
    if (normalized_name == "password") return make_string_option(options_.password);
    if (normalized_name == "bearer-token") return make_string_option(options_.bearer_token);
    if (normalized_name == "proxy") return make_string_option(options_.proxy);
    if (normalized_name == "cookie-jar") return make_string_option(options_.cookie_jar);
    if (normalized_name == "cookie-file") return make_string_option(options_.cookie_file);
    if (normalized_name == "verbose") return make_boolean_option(options_.verbose);
    if (normalized_name == "low-speed-limit-bps") return make_integer_option(options_.low_speed_limit_bps);
    if (normalized_name == "low-speed-time-s") return make_integer_option(options_.low_speed_time_s);
    if (normalized_name == "unix-socket-path") return make_string_option(options_.unix_socket_path);
    return std::unexpected("http/session-get-option: unknown option '" + normalized_name + "'");
}

HttpRequest::HttpRequest(std::shared_ptr<HttpSession> session, std::string method)
    : session_(std::move(session)),
      method_(to_upper_ascii(std::move(method))) {
}

HttpRequest::~HttpRequest() {
    if (easy_ != nullptr) {
        curl_easy_cleanup(easy_);
        easy_ = nullptr;
    }
}

std::expected<void, std::string> HttpRequest::initialize() {
    if (easy_ != nullptr) return {};

    easy_ = curl_easy_init();
    if (easy_ == nullptr) {
        return std::unexpected("http/request-new: failed to allocate CURL easy handle");
    }
    return {};
}

std::expected<void, std::string> HttpRequest::set_url(const std::string_view url_text) {
    if (url_text.empty()) {
        return std::unexpected("http/request-set-url!: URL must not be empty");
    }

    CURLU* url = curl_url();
    if (url == nullptr) {
        return std::unexpected("http/request-set-url!: failed to allocate CURL URL handle");
    }

    std::string input_url(url_text);
    const CURLUcode parse_status = curl_url_set(url, CURLUPART_URL, input_url.c_str(), 0u);
    if (parse_status != CURLUE_OK) {
        curl_url_cleanup(url);
        return std::unexpected("http/request-set-url!: invalid URL");
    }

    char* normalized = nullptr;
    const CURLUcode get_status = curl_url_get(url, CURLUPART_URL, &normalized, 0u);
    if (get_status != CURLUE_OK || normalized == nullptr) {
        curl_url_cleanup(url);
        return std::unexpected("http/request-set-url!: failed to normalize URL");
    }

    url_ = normalized;
    curl_free(normalized);
    curl_url_cleanup(url);
    return {};
}

std::expected<void, std::string> HttpRequest::set_header(
    std::string name,
    std::optional<std::string> value) {
    name = trim_ascii(name);
    if (name.empty()) {
        return std::unexpected("http/request-set-header!: header name must not be empty");
    }

    if (!value.has_value()) {
        headers_.erase(
            std::remove_if(
                headers_.begin(),
                headers_.end(),
                [&name](const HttpHeader& header) { return iequals_ascii(header.name, name); }),
            headers_.end());
        return {};
    }

    headers_.push_back(HttpHeader{
        .name = std::move(name),
        .value = std::move(*value),
    });
    return {};
}

std::expected<void, std::string> HttpRequest::set_body_bytes(std::vector<std::uint8_t> bytes) {
    body_.kind = BodyKind::Bytes;
    body_.bytes = std::move(bytes);
    body_.file_path.clear();
    body_.form_fields.clear();
    body_.multipart_parts.clear();
    body_.content_type.clear();
    return {};
}

std::expected<void, std::string> HttpRequest::set_body_string(
    std::string text,
    std::string charset) {
    body_.kind = BodyKind::Bytes;
    body_.bytes.assign(text.begin(), text.end());
    body_.file_path.clear();
    body_.form_fields.clear();
    body_.multipart_parts.clear();

    charset = trim_ascii(charset);
    if (charset.empty()) {
        body_.content_type = "text/plain";
    } else {
        body_.content_type = "text/plain; charset=" + charset;
    }
    return {};
}

std::expected<void, std::string> HttpRequest::set_body_file(std::string path) {
    path = trim_ascii(path);
    if (path.empty()) {
        return std::unexpected("http/request-set-body-file!: file path must not be empty");
    }
    body_.kind = BodyKind::File;
    body_.bytes.clear();
    body_.file_path = std::move(path);
    body_.form_fields.clear();
    body_.multipart_parts.clear();
    body_.content_type.clear();
    return {};
}

std::expected<void, std::string> HttpRequest::set_body_form(
    std::vector<std::pair<std::string, std::string>> fields) {
    body_.kind = BodyKind::Form;
    body_.bytes.clear();
    body_.file_path.clear();
    body_.form_fields = std::move(fields);
    body_.multipart_parts.clear();
    body_.content_type = "application/x-www-form-urlencoded";
    return {};
}

std::expected<void, std::string> HttpRequest::set_body_multipart(std::vector<HttpMultipartPart> parts) {
    body_.kind = BodyKind::Multipart;
    body_.bytes.clear();
    body_.file_path.clear();
    body_.form_fields.clear();
    body_.multipart_parts = std::move(parts);
    body_.content_type.clear();
    return {};
}

std::expected<void, std::string> HttpRequest::set_boolean_option(
    const std::string_view name,
    const bool value) {
    const std::string normalized_name = to_lower_ascii(std::string(name));
    return set_common_bool_option(overrides_, normalized_name, value, "http/request-set-option!");
}

std::expected<void, std::string> HttpRequest::set_integer_option(
    const std::string_view name,
    const long value) {
    const std::string normalized_name = to_lower_ascii(std::string(name));
    return set_common_integer_option(overrides_, normalized_name, value, "http/request-set-option!");
}

std::expected<void, std::string> HttpRequest::set_string_option(
    const std::string_view name,
    std::string value) {
    const std::string normalized_name = to_lower_ascii(std::string(name));
    return set_common_string_option(
        overrides_,
        normalized_name,
        std::move(value),
        "http/request-set-option!");
}

std::expected<void, std::string> HttpRequest::set_http_version(const std::string_view value) {
    auto parsed = parse_http_version("http/request-set-option!", std::string(value));
    if (!parsed) return std::unexpected(parsed.error());
    overrides_.http_version = *parsed;
    return {};
}

bool HttpRequest::has_header(const std::string_view name) const {
    return contains_header_name(headers_, name);
}

HttpOptions HttpRequest::resolved_options() const {
    HttpOptions options = session_->options();
    apply_override(options.follow_redirects, overrides_.follow_redirects);
    apply_override(options.max_redirects, overrides_.max_redirects);
    apply_override(options.connect_timeout_ms, overrides_.connect_timeout_ms);
    apply_override(options.timeout_ms, overrides_.timeout_ms);
    apply_override(options.user_agent, overrides_.user_agent);
    apply_override(options.accept_encoding, overrides_.accept_encoding);
    apply_override(options.verify_tls, overrides_.verify_tls);
    apply_override(options.http_version, overrides_.http_version);
    apply_override(options.ca_bundle, overrides_.ca_bundle);
    apply_override(options.ca_path, overrides_.ca_path);
    apply_override(options.client_cert, overrides_.client_cert);
    apply_override(options.client_key, overrides_.client_key);
    apply_override(options.username, overrides_.username);
    apply_override(options.password, overrides_.password);
    apply_override(options.bearer_token, overrides_.bearer_token);
    apply_override(options.proxy, overrides_.proxy);
    apply_override(options.cookie_jar, overrides_.cookie_jar);
    apply_override(options.cookie_file, overrides_.cookie_file);
    apply_override(options.verbose, overrides_.verbose);
    apply_override(options.low_speed_limit_bps, overrides_.low_speed_limit_bps);
    apply_override(options.low_speed_time_s, overrides_.low_speed_time_s);
    apply_override(options.unix_socket_path, overrides_.unix_socket_path);
    return options;
}

std::size_t HttpRequest::body_write_callback(char* ptr,
                                             const std::size_t size,
                                             const std::size_t nmemb,
                                             void* userdata) {
    auto* context = static_cast<BodyWriteContext*>(userdata);
    if (context == nullptr || context->response == nullptr || ptr == nullptr) return 0u;

    const std::size_t byte_count = size * nmemb;
    const auto* begin = reinterpret_cast<const std::uint8_t*>(ptr);
    if (context->output != nullptr) {
        context->output->write(ptr, static_cast<std::streamsize>(byte_count));
        if (!context->output->good()) {
            if (context->write_error != nullptr && context->write_error->empty()) {
                *context->write_error = std::string(
                    context->operation.empty() ? std::string_view("http") : context->operation)
                    + ": failed to write streamed response chunk";
            }
            return 0u;
        }
        return byte_count;
    }

    context->response->body.insert(context->response->body.end(), begin, begin + byte_count);
    return byte_count;
}

std::size_t HttpRequest::header_write_callback(char* ptr,
                                               const std::size_t size,
                                               const std::size_t nmemb,
                                               void* userdata) {
    auto* response = static_cast<HttpResponse*>(userdata);
    if (response == nullptr || ptr == nullptr) return 0u;

    const std::size_t byte_count = size * nmemb;
    std::string_view line(ptr, byte_count);
    if (line == "\r\n" || line == "\n") return byte_count;
    if (line.size() >= 5u && line.substr(0u, 5u) == "HTTP/") return byte_count;

    const auto colon = line.find(':');
    if (colon == std::string_view::npos) return byte_count;

    std::string name = trim_ascii(line.substr(0u, colon));
    std::string value = trim_ascii(line.substr(colon + 1u));
    if (name.empty()) return byte_count;

    response->headers.push_back(HttpHeader{
        .name = std::move(name),
        .value = std::move(value),
    });
    return byte_count;
}

std::expected<HttpResponse, std::string> HttpRequest::perform() {
    return perform_internal("http/perform", false, nullptr);
}

std::expected<HttpResponse, std::string> HttpRequest::perform_stream() {
    return perform_internal("http/perform-stream", true, nullptr);
}

std::expected<HttpResponse, std::string> HttpRequest::download_to_file(std::string path) {
    path = trim_ascii(path);
    if (path.empty()) {
        return std::unexpected("http/download: output path must not be empty");
    }

    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output.is_open()) {
        return std::unexpected("http/download: failed to open output file '" + path + "'");
    }

    auto response = perform_internal("http/download", true, &output);
    output.flush();
    if (!output.good()) {
        return std::unexpected("http/download: failed to flush output file '" + path + "'");
    }
    return response;
}

std::expected<HttpResponse, std::string> HttpRequest::perform_internal(
    const std::string_view operation,
    const bool use_multi,
    std::ostream* stream_target) {
    if (session_ == nullptr || !session_->initialized()) {
        return std::unexpected(std::string(operation) + ": request session is not initialized");
    }
    if (easy_ == nullptr) {
        return std::unexpected(std::string(operation) + ": request handle is not initialized");
    }
    if (url_.empty()) {
        return std::unexpected(std::string(operation) + ": request URL is not set");
    }

    HttpResponse response;
    curl_easy_reset(easy_);
    error_buffer_.fill('\0');
    const HttpOptions options = resolved_options();

    auto setopt = [this, operation](const CURLoption option,
                                    const auto value,
                                    const char* label) -> std::expected<void, std::string> {
        const CURLcode code = curl_easy_setopt(easy_, option, value);
        if (code == CURLE_OK) return {};

        std::ostringstream out;
        out << operation << ": failed to set " << label << " (" << static_cast<int>(code) << ")";
        return std::unexpected(out.str());
    };

    if (auto status = setopt(CURLOPT_ERRORBUFFER, error_buffer_.data(), "error-buffer"); !status) {
        return std::unexpected(status.error());
    }
    if (auto status = setopt(CURLOPT_URL, url_.c_str(), "url"); !status) {
        return std::unexpected(status.error());
    }
    if (auto status = setopt(CURLOPT_SHARE, session_->share_handle(), "share"); !status) {
        return std::unexpected(status.error());
    }
    if (options.cookie_file.empty()) {
        if (auto status = setopt(CURLOPT_COOKIEFILE, "", "cookie-file"); !status) {
            return std::unexpected(status.error());
        }
    } else if (auto status = setopt(CURLOPT_COOKIEFILE, options.cookie_file.c_str(), "cookie-file"); !status) {
        return std::unexpected(status.error());
    }

    const bool follow_redirects = options.follow_redirects && options.max_redirects > 0L;
    if (auto status = setopt(CURLOPT_FOLLOWLOCATION, follow_redirects ? 1L : 0L, "follow-redirects");
        !status) {
        return std::unexpected(status.error());
    }
    if (auto status = setopt(CURLOPT_MAXREDIRS, options.max_redirects, "max-redirects"); !status) {
        return std::unexpected(status.error());
    }
    if (auto status = setopt(CURLOPT_CONNECTTIMEOUT_MS, options.connect_timeout_ms, "connect-timeout-ms");
        !status) {
        return std::unexpected(status.error());
    }
    if (auto status = setopt(CURLOPT_TIMEOUT_MS, options.timeout_ms, "timeout-ms"); !status) {
        return std::unexpected(status.error());
    }
    if (auto status = setopt(CURLOPT_SSL_VERIFYPEER, options.verify_tls ? 1L : 0L, "verify-tls-peer");
        !status) {
        return std::unexpected(status.error());
    }
    if (auto status = setopt(CURLOPT_SSL_VERIFYHOST, options.verify_tls ? 2L : 0L, "verify-tls-host");
        !status) {
        return std::unexpected(status.error());
    }
    if (auto status = setopt(CURLOPT_USERAGENT, options.user_agent.c_str(), "user-agent"); !status) {
        return std::unexpected(status.error());
    }
    if (auto status = setopt(CURLOPT_ACCEPT_ENCODING, options.accept_encoding.c_str(), "accept-encoding");
        !status) {
        return std::unexpected(status.error());
    }

    auto resolved_http_version = curl_http_version(options.http_version, operation);
    if (!resolved_http_version) return std::unexpected(resolved_http_version.error());
    if (auto status = setopt(CURLOPT_HTTP_VERSION, *resolved_http_version, "http-version"); !status) {
        return std::unexpected(status.error());
    }

    if (!options.ca_bundle.empty()) {
        if (auto status = setopt(CURLOPT_CAINFO, options.ca_bundle.c_str(), "ca-bundle"); !status) {
            return std::unexpected(status.error());
        }
    }
    if (!options.ca_path.empty()) {
        if (auto status = setopt(CURLOPT_CAPATH, options.ca_path.c_str(), "ca-path"); !status) {
            return std::unexpected(status.error());
        }
    }
    if (!options.client_cert.empty()) {
        if (auto status = setopt(CURLOPT_SSLCERT, options.client_cert.c_str(), "client-cert"); !status) {
            return std::unexpected(status.error());
        }
    }
    if (!options.client_key.empty()) {
        if (auto status = setopt(CURLOPT_SSLKEY, options.client_key.c_str(), "client-key"); !status) {
            return std::unexpected(status.error());
        }
    }
    if (!options.username.empty()) {
        if (auto status = setopt(CURLOPT_USERNAME, options.username.c_str(), "username"); !status) {
            return std::unexpected(status.error());
        }
    }
    if (!options.password.empty()) {
        if (auto status = setopt(CURLOPT_PASSWORD, options.password.c_str(), "password"); !status) {
            return std::unexpected(status.error());
        }
    }
    if (!options.bearer_token.empty()) {
        if (auto status = setopt(CURLOPT_XOAUTH2_BEARER, options.bearer_token.c_str(), "bearer-token");
            !status) {
            return std::unexpected(status.error());
        }
    }
    if (!options.proxy.empty()) {
        if (auto status = setopt(CURLOPT_PROXY, options.proxy.c_str(), "proxy"); !status) {
            return std::unexpected(status.error());
        }
    }
    if (!options.cookie_jar.empty()) {
        if (auto status = setopt(CURLOPT_COOKIEJAR, options.cookie_jar.c_str(), "cookie-jar"); !status) {
            return std::unexpected(status.error());
        }
    }
    if (auto status = setopt(CURLOPT_VERBOSE, options.verbose ? 1L : 0L, "verbose"); !status) {
        return std::unexpected(status.error());
    }
    if (auto status = setopt(CURLOPT_LOW_SPEED_LIMIT, options.low_speed_limit_bps, "low-speed-limit-bps");
        !status) {
        return std::unexpected(status.error());
    }
    if (auto status = setopt(CURLOPT_LOW_SPEED_TIME, options.low_speed_time_s, "low-speed-time-s"); !status) {
        return std::unexpected(status.error());
    }
    if (!options.unix_socket_path.empty()) {
        if (auto status = setopt(CURLOPT_UNIX_SOCKET_PATH, options.unix_socket_path.c_str(), "unix-socket-path");
            !status) {
            return std::unexpected(status.error());
        }
    }

    std::string write_error;
    BodyWriteContext write_context{
        .response = &response,
        .output = stream_target,
        .operation = operation,
        .write_error = &write_error,
    };

    if (auto status = setopt(CURLOPT_NOSIGNAL, 1L, "nosignal"); !status) {
        return std::unexpected(status.error());
    }
    if (auto status = setopt(CURLOPT_WRITEFUNCTION, &HttpRequest::body_write_callback, "write-callback");
        !status) {
        return std::unexpected(status.error());
    }
    if (auto status = setopt(CURLOPT_WRITEDATA, &write_context, "write-data"); !status) {
        return std::unexpected(status.error());
    }
    if (auto status = setopt(CURLOPT_HEADERFUNCTION, &HttpRequest::header_write_callback, "header-callback");
        !status) {
        return std::unexpected(status.error());
    }
    if (auto status = setopt(CURLOPT_HEADERDATA, &response, "header-data"); !status) {
        return std::unexpected(status.error());
    }

    if (method_ == "GET") {
        if (auto status = setopt(CURLOPT_HTTPGET, 1L, "httpget"); !status) {
            return std::unexpected(status.error());
        }
    } else if (method_ == "POST") {
        if (auto status = setopt(CURLOPT_POST, 1L, "post"); !status) {
            return std::unexpected(status.error());
        }
    } else if (method_ == "HEAD") {
        if (auto status = setopt(CURLOPT_NOBODY, 1L, "nobody"); !status) {
            return std::unexpected(status.error());
        }
        if (auto status = setopt(CURLOPT_CUSTOMREQUEST, "HEAD", "custom-request"); !status) {
            return std::unexpected(status.error());
        }
    } else {
        if (auto status = setopt(CURLOPT_CUSTOMREQUEST, method_.c_str(), "custom-request"); !status) {
            return std::unexpected(status.error());
        }
    }

    std::vector<HttpHeader> request_headers = headers_;
    if (!body_.content_type.empty() && !contains_header_name(request_headers, "Content-Type")) {
        request_headers.push_back(HttpHeader{
            .name = "Content-Type",
            .value = body_.content_type,
        });
    }

    std::unique_ptr<curl_slist, CurlSlistDeleter> header_list_guard{nullptr};
    curl_slist* raw_header_list = nullptr;
    for (const auto& header : request_headers) {
        std::string line = header.name + ": " + header.value;
        curl_slist* appended = curl_slist_append(raw_header_list, line.c_str());
        if (appended == nullptr) {
            if (raw_header_list != nullptr) {
                curl_slist_free_all(raw_header_list);
            }
            return std::unexpected(std::string(operation) + ": failed to allocate request header list");
        }
        raw_header_list = appended;
    }
    header_list_guard.reset(raw_header_list);
    if (raw_header_list != nullptr) {
        if (auto status = setopt(CURLOPT_HTTPHEADER, raw_header_list, "headers"); !status) {
            return std::unexpected(status.error());
        }
    }

    std::vector<std::uint8_t> body_bytes;
    std::unique_ptr<curl_mime, CurlMimeDeleter> mime_guard{nullptr};

    if (body_.kind == BodyKind::Bytes) {
        body_bytes = body_.bytes;
    } else if (body_.kind == BodyKind::File) {
        auto read_bytes = read_file_body(body_.file_path, operation);
        if (!read_bytes) return std::unexpected(read_bytes.error());
        body_bytes = std::move(*read_bytes);
    } else if (body_.kind == BodyKind::Form) {
        auto encoded_form = encode_form_fields(easy_, body_.form_fields, operation);
        if (!encoded_form) return std::unexpected(encoded_form.error());
        body_bytes = std::move(*encoded_form);
    } else if (body_.kind == BodyKind::Multipart) {
        curl_mime* mime = curl_mime_init(easy_);
        if (mime == nullptr) {
            return std::unexpected(std::string(operation) + ": failed to allocate multipart body");
        }
        mime_guard.reset(mime);

        for (const auto& part : body_.multipart_parts) {
            curl_mimepart* mime_part = curl_mime_addpart(mime);
            if (mime_part == nullptr) {
                return std::unexpected(std::string(operation) + ": failed to append multipart part");
            }
            if (curl_mime_name(mime_part, part.name.c_str()) != CURLE_OK) {
                return std::unexpected(std::string(operation) + ": failed to set multipart part name");
            }
            if (!part.filename.empty() && curl_mime_filename(mime_part, part.filename.c_str()) != CURLE_OK) {
                return std::unexpected(std::string(operation) + ": failed to set multipart filename");
            }
            if (!part.content_type.empty() && curl_mime_type(mime_part, part.content_type.c_str()) != CURLE_OK) {
                return std::unexpected(std::string(operation) + ": failed to set multipart content-type");
            }
            const char* data_ptr = part.data.empty()
                ? ""
                : reinterpret_cast<const char*>(part.data.data());
            if (curl_mime_data(mime_part, data_ptr, part.data.size()) != CURLE_OK) {
                return std::unexpected(std::string(operation) + ": failed to set multipart data");
            }
        }

        if (auto status = setopt(CURLOPT_MIMEPOST, mime_guard.get(), "multipart"); !status) {
            return std::unexpected(status.error());
        }
    }

    if (body_.kind != BodyKind::None && body_.kind != BodyKind::Multipart && method_ != "HEAD") {
        const char* payload_data = body_bytes.empty()
            ? ""
            : reinterpret_cast<const char*>(body_bytes.data());
        if (auto status = setopt(CURLOPT_POSTFIELDS, payload_data, "postfields"); !status) {
            return std::unexpected(status.error());
        }
        if (auto status = setopt(
                CURLOPT_POSTFIELDSIZE_LARGE,
                static_cast<curl_off_t>(body_bytes.size()),
                "postfields-size");
            !status) {
            return std::unexpected(status.error());
        }
    }

    if (use_multi) {
        auto run_status = perform_with_multi(easy_, operation, error_buffer_);
        if (!run_status) {
            if (!write_error.empty()) return std::unexpected(write_error);
            return std::unexpected(run_status.error());
        }
    } else {
        const CURLcode perform_status = curl_easy_perform(easy_);
        if (perform_status != CURLE_OK) {
            if (!write_error.empty()) return std::unexpected(write_error);
            return std::unexpected(curl_error_text(operation, perform_status, error_buffer_));
        }
    }

    long status_code = 0;
    if (curl_easy_getinfo(easy_, CURLINFO_RESPONSE_CODE, &status_code) == CURLE_OK) {
        response.status = status_code;
    }

    char* effective_url = nullptr;
    if (curl_easy_getinfo(easy_, CURLINFO_EFFECTIVE_URL, &effective_url) == CURLE_OK
        && effective_url != nullptr) {
        response.effective_url = effective_url;
    }

    return response;
}

} // namespace eta::http_sidecar
