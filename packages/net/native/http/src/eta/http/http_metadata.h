#pragma once

/**
 * @file http_metadata.h
 * @brief Compile-time metadata constants for the libcurl HTTP sidecar package.
 */

#include "eta/native/sdk.h"

namespace eta::http_sidecar {

/**
 * @brief Extension identifier declared by this sidecar package.
 */
inline constexpr const char* kExtensionId = "eta.http.sidecar";

/**
 * @brief Sidecar package version string.
 */
inline constexpr const char* kExtensionVersion = "0.1.0";

/**
 * @brief Upstream libcurl tag pinned for package development.
 */
inline constexpr const char* kLibcurlUpstreamTag = "curl-8_13_0";

/**
 * @brief Native ABI identifier expected by this sidecar package.
 */
inline constexpr const char* kNativeAbi = ETA_NATIVE_ABI_ID_V1;

} // namespace eta::http_sidecar
