#pragma once

/**
 * @file http_multi.h
 * @brief Thin libcurl multi-handle runner used by streaming HTTP paths.
 */

#include <curl/curl.h>

#include <array>
#include <expected>
#include <string>
#include <string_view>

namespace eta::http_sidecar {

/**
 * @brief Drive one configured easy handle to completion through a multi handle.
 */
[[nodiscard]] std::expected<void, std::string> perform_with_multi(
    CURL* easy,
    std::string_view operation,
    const std::array<char, CURL_ERROR_SIZE>& error_buffer);

} // namespace eta::http_sidecar
