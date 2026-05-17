#include "eta/http/http_metadata.h"
#include "eta/http/http_primitives.h"

#include <curl/curl.h>

#include <atomic>
#include <mutex>

namespace {

std::once_flag g_curl_init_once;
std::atomic<int> g_curl_global_status{ETA_NATIVE_STATUS_ERROR};

void ensure_curl_global_init() {
    const CURLcode status = curl_global_init(CURL_GLOBAL_DEFAULT);
    g_curl_global_status.store(
        status == CURLE_OK ? ETA_NATIVE_STATUS_OK : ETA_NATIVE_STATUS_ERROR,
        std::memory_order_release);
}

struct CurlGlobalCleanup {
    ~CurlGlobalCleanup() {
        if (g_curl_global_status.load(std::memory_order_acquire) == ETA_NATIVE_STATUS_OK) {
            curl_global_cleanup();
        }
    }
};

CurlGlobalCleanup g_curl_cleanup;

void fill_extension_info(EtaExtensionInfoV1* out_info) {
    if (out_info == nullptr) return;
    out_info->struct_size = sizeof(EtaExtensionInfoV1);
    out_info->abi_id = eta::http_sidecar::kNativeAbi;
    out_info->extension_id = eta::http_sidecar::kExtensionId;
    out_info->extension_version = eta::http_sidecar::kExtensionVersion;
}

} // namespace

extern "C" ETA_NATIVE_EXPORT int eta_register_http_extension_v1(const EtaNativeApiV1* api,
                                                                 EtaExtensionInfoV1* out_info) {
    fill_extension_info(out_info);

    std::call_once(g_curl_init_once, ensure_curl_global_init);
    if (g_curl_global_status.load(std::memory_order_acquire) != ETA_NATIVE_STATUS_OK) {
        if (api != nullptr && api->report_error != nullptr) {
            api->report_error(api->user_data, "http sidecar failed to initialize libcurl");
        }
        return ETA_NATIVE_STATUS_ERROR;
    }

    return eta::http_sidecar::register_http_primitives(api);
}
