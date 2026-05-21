#include "eta/http/http_multi.h"

#include <memory>
#include <sstream>

namespace eta::http_sidecar {

namespace {

struct CurlMultiDeleter {
    void operator()(CURLM* value) const noexcept {
        if (value != nullptr) {
            curl_multi_cleanup(value);
        }
    }
};

[[nodiscard]] std::string curl_easy_error_text(std::string_view operation,
                                               const CURLcode code,
                                               const std::array<char, CURL_ERROR_SIZE>& buffer) {
    std::ostringstream out;
    out << operation << ": " << static_cast<int>(code);
    if (buffer[0] != '\0') {
        out << " (" << buffer.data() << ")";
    } else {
        out << " (" << curl_easy_strerror(code) << ")";
    }
    return out.str();
}

[[nodiscard]] std::string curl_multi_error_text(std::string_view operation,
                                                const CURLMcode code) {
    std::ostringstream out;
    out << operation << ": curl_multi " << static_cast<int>(code)
        << " (" << curl_multi_strerror(code) << ")";
    return out.str();
}

} // namespace

std::expected<void, std::string> perform_with_multi(
    CURL* easy,
    const std::string_view operation,
    const std::array<char, CURL_ERROR_SIZE>& error_buffer) {
    if (easy == nullptr) {
        return std::unexpected(std::string(operation) + ": easy handle is null");
    }

    std::unique_ptr<CURLM, CurlMultiDeleter> multi(curl_multi_init());
    if (!multi) {
        return std::unexpected(std::string(operation) + ": failed to allocate CURL multi handle");
    }

    CURLMcode multi_status = curl_multi_add_handle(multi.get(), easy);
    if (multi_status != CURLM_OK) {
        return std::unexpected(curl_multi_error_text(operation, multi_status));
    }

    bool handle_added = true;
    auto remove_handle = [&]() {
        if (handle_added) {
            curl_multi_remove_handle(multi.get(), easy);
            handle_added = false;
        }
    };

    int still_running = 0;
    multi_status = curl_multi_perform(multi.get(), &still_running);
    if (multi_status != CURLM_OK) {
        remove_handle();
        return std::unexpected(curl_multi_error_text(operation, multi_status));
    }

    while (still_running > 0) {
        int num_fds = 0;
        multi_status = curl_multi_poll(multi.get(), nullptr, 0, 1000, &num_fds);
        if (multi_status != CURLM_OK) {
            remove_handle();
            return std::unexpected(curl_multi_error_text(operation, multi_status));
        }

        multi_status = curl_multi_perform(multi.get(), &still_running);
        if (multi_status != CURLM_OK) {
            remove_handle();
            return std::unexpected(curl_multi_error_text(operation, multi_status));
        }
    }

    CURLcode transfer_status = CURLE_OK;
    bool has_done_message = false;
    int message_count = 0;
    while (CURLMsg* message = curl_multi_info_read(multi.get(), &message_count)) {
        if (message->msg != CURLMSG_DONE || message->easy_handle != easy) continue;
        transfer_status = message->data.result;
        has_done_message = true;
        break;
    }

    remove_handle();

    if (!has_done_message) {
        return std::unexpected(std::string(operation) + ": transfer completed without CURLMSG_DONE");
    }
    if (transfer_status != CURLE_OK) {
        return std::unexpected(curl_easy_error_text(operation, transfer_status, error_buffer));
    }

    return {};
}

} // namespace eta::http_sidecar
