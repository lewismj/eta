#pragma once

/**
 * @file lightgbm_primitives.h
 * @brief Primitive registration for the LightGBM native sidecar scaffold.
 */

#include "eta/native/sdk.h"

namespace eta::lightgbm_sidecar {

/**
 * @brief Register `lgbm/`-prefixed primitives through the Eta sidecar C ABI.
 */
int register_lightgbm_primitives(const EtaNativeApiV1* api);

} // namespace eta::lightgbm_sidecar
