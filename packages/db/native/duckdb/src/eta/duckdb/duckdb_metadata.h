#pragma once

/**
 * @file duckdb_metadata.h
 * @brief Compile-time metadata constants for the DuckDB sidecar package.
 */

#include "eta/native/sdk.h"

namespace eta::duckdb_sidecar {

/**
 * @brief Extension identifier declared by this sidecar package.
 */
inline constexpr const char* kExtensionId = "eta.duckdb.sidecar";

/**
 * @brief Sidecar package version string.
 */
inline constexpr const char* kExtensionVersion = "0.1.0";

/**
 * @brief Upstream DuckDB release tag pinned for package development.
 */
inline constexpr const char* kDuckDbUpstreamTag = "v1.5.1";

/**
 * @brief Native ABI identifier expected by this sidecar package.
 */
inline constexpr const char* kNativeAbi = ETA_NATIVE_ABI_ID_V1;

} // namespace eta::duckdb_sidecar
