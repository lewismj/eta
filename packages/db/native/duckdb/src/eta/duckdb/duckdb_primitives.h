#pragma once

/**
 * @file duckdb_primitives.h
 * @brief Primitive registration scaffold for the DuckDB native sidecar.
 */

#include "eta/native/sdk.h"

namespace eta::duckdb_sidecar {

/**
 * @brief Register DuckDB sidecar primitives through the Eta sidecar C ABI.
 */
int register_duckdb_primitives(const EtaNativeApiV1* api);

} // namespace eta::duckdb_sidecar
