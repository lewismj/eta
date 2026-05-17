#pragma once

/**
 * @file http_primitives.h
 * @brief Primitive registration for the libcurl HTTP sidecar package.
 */

#include "eta/native/sdk.h"

namespace eta::http_sidecar {

/**
 * @brief Register `http/` primitives through the Eta sidecar C ABI.
 */
int register_http_primitives(const EtaNativeApiV1* api);

} // namespace eta::http_sidecar
