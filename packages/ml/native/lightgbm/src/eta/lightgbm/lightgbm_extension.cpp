#include "eta/lightgbm/lightgbm_primitives.h"

namespace {

void fill_extension_info(EtaExtensionInfoV1* out_info) {
    if (out_info == nullptr) return;
    out_info->struct_size = sizeof(EtaExtensionInfoV1);
    out_info->abi_id = ETA_NATIVE_ABI_ID_V1;
    out_info->extension_id = "eta.lgbm.sidecar";
    out_info->extension_version = "0.1.0";
}

} // namespace

extern "C" ETA_NATIVE_EXPORT int eta_register_lgbm_extension_v1(const EtaNativeApiV1* api,
                                                                 EtaExtensionInfoV1* out_info) {
    fill_extension_info(out_info);
    return eta::lightgbm_sidecar::register_lightgbm_primitives(api);
}
