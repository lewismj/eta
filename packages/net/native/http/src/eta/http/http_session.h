#pragma once

/**
 * @file http_session.h
 * @brief RAII wrappers and option state for libcurl HTTP request execution.
 */

#include <curl/curl.h>

#include <array>
#include <cstdint>
#include <expected>
#include <iosfwd>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace eta::http_sidecar {

/**
 * @brief One response header entry in transmission order.
 */
struct HttpHeader {
    std::string name;
    std::string value;
};

/**
 * @brief Captured HTTP response payload from one completed request.
 */
struct HttpResponse {
    long status{0};
    std::string effective_url;
    std::vector<HttpHeader> headers;
    std::vector<std::uint8_t> body;
};

/**
 * @brief HTTP protocol preference wired to `CURLOPT_HTTP_VERSION`.
 */
enum class HttpVersion {
    Any,
    Http11,
    Http2,
    Http2Tls,
};

/**
 * @brief Resolved per-request option set applied to libcurl handles.
 */
struct HttpOptions {
    bool follow_redirects{true};
    long max_redirects{10L};
    long connect_timeout_ms{30000L};
    long timeout_ms{0L};
    std::string user_agent;
    std::string accept_encoding;
    bool verify_tls{true};
    HttpVersion http_version{HttpVersion::Http2Tls};
    std::string ca_bundle;
    std::string ca_path;
    std::string client_cert;
    std::string client_key;
    std::string username;
    std::string password;
    std::string bearer_token;
    std::string proxy;
    std::string cookie_jar;
    std::string cookie_file;
    bool verbose{false};
    long low_speed_limit_bps{0L};
    long low_speed_time_s{0L};
    std::string unix_socket_path;
};

/**
 * @brief Value carrier for session option reads.
 */
struct HttpOptionValue {
    enum class Kind {
        Boolean,
        Integer,
        String,
        Symbol,
    };

    Kind kind{Kind::Boolean};
    bool bool_value{false};
    long int_value{0L};
    std::string text;
};

/**
 * @brief One multipart part submitted through `curl_mime`.
 */
struct HttpMultipartPart {
    std::string name;
    std::vector<std::uint8_t> data;
    std::string filename;
    std::string content_type;
};

/**
 * @brief Session defaults + shared libcurl state used by request handles.
 */
class HttpSession {
public:
    explicit HttpSession(std::string default_user_agent);
    ~HttpSession();

    HttpSession(const HttpSession&) = delete;
    HttpSession& operator=(const HttpSession&) = delete;

    /**
     * @brief Initialize the underlying `CURLSH` handle and defaults.
     */
    [[nodiscard]] std::expected<void, std::string> initialize();

    /**
     * @brief Return whether initialization completed successfully.
     */
    [[nodiscard]] bool initialized() const noexcept { return initialized_; }

    /**
     * @brief Return the underlying share handle for request reuse.
     */
    [[nodiscard]] CURLSH* share_handle() const noexcept { return share_; }

    /**
     * @brief Set one boolean session option by name.
     */
    [[nodiscard]] std::expected<void, std::string> set_boolean_option(std::string_view name, bool value);

    /**
     * @brief Set one integer session option by name.
     */
    [[nodiscard]] std::expected<void, std::string> set_integer_option(std::string_view name, long value);

    /**
     * @brief Set one string/path session option by name.
     */
    [[nodiscard]] std::expected<void, std::string> set_string_option(
        std::string_view name,
        std::string value);

    /**
     * @brief Set HTTP protocol preference.
     */
    [[nodiscard]] std::expected<void, std::string> set_http_version(std::string_view value);

    /**
     * @brief Read a session option by name.
     */
    [[nodiscard]] std::expected<HttpOptionValue, std::string> get_option(std::string_view name) const;

    /**
     * @brief Return current resolved defaults used by new requests.
     */
    [[nodiscard]] const HttpOptions& options() const noexcept { return options_; }

private:
    static void lock_callback(CURL*, curl_lock_data, curl_lock_access, void* userptr);
    static void unlock_callback(CURL*, curl_lock_data, void* userptr);

    CURLSH* share_{nullptr};
    std::mutex share_mutex_;
    std::string default_user_agent_;
    HttpOptions options_{};
    bool initialized_{false};
};

/**
 * @brief Mutable libcurl request handle bound to a session.
 */
class HttpRequest {
public:
    HttpRequest(std::shared_ptr<HttpSession> session, std::string method);
    ~HttpRequest();

    HttpRequest(const HttpRequest&) = delete;
    HttpRequest& operator=(const HttpRequest&) = delete;

    /**
     * @brief Initialize the underlying `CURL*` handle.
     */
    [[nodiscard]] std::expected<void, std::string> initialize();

    /**
     * @brief Parse and set request URL via `curl_url_*`.
     */
    [[nodiscard]] std::expected<void, std::string> set_url(std::string_view url_text);

    /**
     * @brief Add or remove one request header.
     */
    [[nodiscard]] std::expected<void, std::string> set_header(
        std::string name,
        std::optional<std::string> value);

    /**
     * @brief Set body payload from raw bytes.
     */
    [[nodiscard]] std::expected<void, std::string> set_body_bytes(std::vector<std::uint8_t> bytes);

    /**
     * @brief Set body payload from string text + charset metadata.
     */
    [[nodiscard]] std::expected<void, std::string> set_body_string(
        std::string text,
        std::string charset);

    /**
     * @brief Set body payload from file path.
     */
    [[nodiscard]] std::expected<void, std::string> set_body_file(std::string path);

    /**
     * @brief Set body payload from form fields.
     */
    [[nodiscard]] std::expected<void, std::string> set_body_form(
        std::vector<std::pair<std::string, std::string>> fields);

    /**
     * @brief Set body payload from multipart parts.
     */
    [[nodiscard]] std::expected<void, std::string> set_body_multipart(std::vector<HttpMultipartPart> parts);

    /**
     * @brief Set one request-local boolean option override.
     */
    [[nodiscard]] std::expected<void, std::string> set_boolean_option(std::string_view name, bool value);

    /**
     * @brief Set one request-local integer option override.
     */
    [[nodiscard]] std::expected<void, std::string> set_integer_option(std::string_view name, long value);

    /**
     * @brief Set one request-local string/path option override.
     */
    [[nodiscard]] std::expected<void, std::string> set_string_option(
        std::string_view name,
        std::string value);

    /**
     * @brief Set one request-local HTTP protocol override.
     */
    [[nodiscard]] std::expected<void, std::string> set_http_version(std::string_view value);

    /**
     * @brief Execute this request synchronously and return captured response.
     */
    [[nodiscard]] std::expected<HttpResponse, std::string> perform();

    /**
     * @brief Execute this request through a multi-handle transfer loop.
     */
    [[nodiscard]] std::expected<HttpResponse, std::string> perform_stream();

    /**
     * @brief Execute this request and stream body bytes into `path`.
     */
    [[nodiscard]] std::expected<HttpResponse, std::string> download_to_file(std::string path);

private:
    enum class BodyKind {
        None,
        Bytes,
        File,
        Form,
        Multipart,
    };

    struct BodyPayload {
        BodyKind kind{BodyKind::None};
        std::vector<std::uint8_t> bytes;
        std::string file_path;
        std::vector<std::pair<std::string, std::string>> form_fields;
        std::vector<HttpMultipartPart> multipart_parts;
        std::string content_type;
    };

    struct OptionOverrides {
        std::optional<bool> follow_redirects;
        std::optional<long> max_redirects;
        std::optional<long> connect_timeout_ms;
        std::optional<long> timeout_ms;
        std::optional<std::string> user_agent;
        std::optional<std::string> accept_encoding;
        std::optional<bool> verify_tls;
        std::optional<HttpVersion> http_version;
        std::optional<std::string> ca_bundle;
        std::optional<std::string> ca_path;
        std::optional<std::string> client_cert;
        std::optional<std::string> client_key;
        std::optional<std::string> username;
        std::optional<std::string> password;
        std::optional<std::string> bearer_token;
        std::optional<std::string> proxy;
        std::optional<std::string> cookie_jar;
        std::optional<std::string> cookie_file;
        std::optional<bool> verbose;
        std::optional<long> low_speed_limit_bps;
        std::optional<long> low_speed_time_s;
        std::optional<std::string> unix_socket_path;
    };

    [[nodiscard]] bool has_header(std::string_view name) const;
    [[nodiscard]] HttpOptions resolved_options() const;
    [[nodiscard]] std::expected<HttpResponse, std::string> perform_internal(
        std::string_view operation,
        bool use_multi,
        std::ostream* stream_target);

    struct BodyWriteContext {
        HttpResponse* response{nullptr};
        std::ostream* output{nullptr};
        std::string_view operation{};
        std::string* write_error{nullptr};
    };

    static std::size_t body_write_callback(char* ptr, std::size_t size, std::size_t nmemb, void* userdata);
    static std::size_t header_write_callback(char* ptr, std::size_t size, std::size_t nmemb, void* userdata);

    std::shared_ptr<HttpSession> session_;
    std::string method_;
    std::string url_;
    std::vector<HttpHeader> headers_;
    BodyPayload body_;
    OptionOverrides overrides_;
    CURL* easy_{nullptr};
    std::array<char, CURL_ERROR_SIZE> error_buffer_{};
};

} // namespace eta::http_sidecar
