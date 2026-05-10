#include "eta/duckdb/duckdb_metadata.h"
#include "eta/duckdb/duckdb_primitives.h"

namespace {

void fill_extension_info(EtaExtensionInfoV1* out_info) {
    if (out_info == nullptr) return;
    out_info->struct_size = sizeof(EtaExtensionInfoV1);
    out_info->abi_id = eta::duckdb_sidecar::kNativeAbi;
    out_info->extension_id = eta::duckdb_sidecar::kExtensionId;
    out_info->extension_version = eta::duckdb_sidecar::kExtensionVersion;
}

} // namespace

extern "C" ETA_NATIVE_EXPORT int eta_register_duckdb_extension_v1(const EtaNativeApiV1* api,
                                                                   EtaExtensionInfoV1* out_info) {
    fill_extension_info(out_info);
    return eta::duckdb_sidecar::register_duckdb_primitives(api);
}
