#include "eta/duckdb/duckdb_primitives.h"

namespace eta::duckdb_sidecar {

int register_duckdb_primitives(const EtaNativeApiV1* api) {
    if (api == nullptr || api->register_primitive == nullptr) {
        if (api != nullptr && api->report_error != nullptr) {
            api->report_error(
                api->user_data,
                "duckdb sidecar requires register_primitive callback support");
        }
        return ETA_NATIVE_STATUS_ERROR;
    }

    return ETA_NATIVE_STATUS_OK;
}

} // namespace eta::duckdb_sidecar
