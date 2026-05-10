#include "eta/duckdb/duckdb_metadata.h"
#include "eta/native/sdk.h"

#include <cstdint>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

namespace {

int expect_true(const bool condition, const char* message) {
    if (condition) return 0;
    std::cerr << "duckdb_metadata_tests: " << message << '\n';
    return 1;
}

int g_register_calls = 0;
struct RegisteredPrimitiveCall {
    std::string name;
    std::uint32_t arity{0};
    std::uint8_t has_rest{0};
};
std::vector<RegisteredPrimitiveCall> g_registered_primitives;

int register_stub(void* user_data,
                  const char* name,
                  const std::uint32_t arity,
                  const std::uint8_t has_rest,
                  void*) {
    auto* calls = static_cast<int*>(user_data);
    if (calls != nullptr) ++(*calls);
    ++g_register_calls;
    g_registered_primitives.push_back(RegisteredPrimitiveCall{
        .name = name == nullptr ? std::string{} : std::string{name},
        .arity = arity,
        .has_rest = has_rest,
    });
    return ETA_NATIVE_STATUS_OK;
}

void report_error_stub(void*, const char*) {
}

} // namespace

extern "C" int eta_register_duckdb_extension_v1(const EtaNativeApiV1* api,
                                                EtaExtensionInfoV1* out_info);

int main() {
    int failures = 0;

    failures += expect_true(
        std::strcmp(eta::duckdb_sidecar::kDuckDbUpstreamTag, "v1.5.1") == 0,
        "duckdb upstream tag should be pinned to v1.5.1");
    failures += expect_true(
        std::strcmp(eta::duckdb_sidecar::kNativeAbi, ETA_NATIVE_ABI_ID_V1) == 0,
        "native ABI should match eta-native-v1");

    EtaExtensionInfoV1 info{};
    int per_call_counter = 0;
    EtaNativeApiV1 api{};
    api.struct_size = sizeof(EtaNativeApiV1);
    api.abi_id = ETA_NATIVE_ABI_ID_V1;
    api.user_data = &per_call_counter;
    api.register_primitive = &register_stub;
    api.report_error = &report_error_stub;

    const int ok_status = eta_register_duckdb_extension_v1(&api, &info);
    failures += expect_true(
        ok_status == ETA_NATIVE_STATUS_OK,
        "extension entry should succeed when register callback is available");
    failures += expect_true(
        std::strcmp(info.extension_id, eta::duckdb_sidecar::kExtensionId) == 0,
        "extension id should match package metadata");
    failures += expect_true(
        std::strcmp(info.extension_version, eta::duckdb_sidecar::kExtensionVersion) == 0,
        "extension version should match package metadata");
    failures += expect_true(
        std::strcmp(info.abi_id, ETA_NATIVE_ABI_ID_V1) == 0,
        "extension ABI should match eta-native-v1");
    failures += expect_true(
        per_call_counter == 5 && g_register_calls == 5,
        "duckdb sidecar should register five MVP primitives");
    failures += expect_true(
        g_registered_primitives.size() == 5u,
        "registration callback should capture five primitive rows");
    if (g_registered_primitives.size() == 5u) {
        failures += expect_true(
            g_registered_primitives[0].name == "%duckdb-open"
                && g_registered_primitives[0].arity == 1u
                && g_registered_primitives[0].has_rest == 0u,
            "first primitive should be %duckdb-open/1");
        failures += expect_true(
            g_registered_primitives[1].name == "%duckdb-close!"
                && g_registered_primitives[1].arity == 1u
                && g_registered_primitives[1].has_rest == 0u,
            "second primitive should be %duckdb-close!/1");
        failures += expect_true(
            g_registered_primitives[2].name == "%duckdb-exec"
                && g_registered_primitives[2].arity == 2u
                && g_registered_primitives[2].has_rest == 0u,
            "third primitive should be %duckdb-exec/2");
        failures += expect_true(
            g_registered_primitives[3].name == "%duckdb-query"
                && g_registered_primitives[3].arity == 2u
                && g_registered_primitives[3].has_rest == 1u,
            "fourth primitive should be %duckdb-query with rest args");
        failures += expect_true(
            g_registered_primitives[4].name == "%duckdb-last-error"
                && g_registered_primitives[4].arity == 1u
                && g_registered_primitives[4].has_rest == 0u,
            "fifth primitive should be %duckdb-last-error/1");
    }

    EtaNativeApiV1 missing_register{};
    missing_register.struct_size = sizeof(EtaNativeApiV1);
    missing_register.abi_id = ETA_NATIVE_ABI_ID_V1;
    missing_register.report_error = &report_error_stub;
    const int err_status = eta_register_duckdb_extension_v1(&missing_register, &info);
    failures += expect_true(
        err_status == ETA_NATIVE_STATUS_ERROR,
        "extension entry should fail without register callback support");

    return failures == 0 ? 0 : 1;
}
