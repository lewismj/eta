#pragma once

/**
 * @file sdk.h
 * @brief C ABI surface for Eta native sidecar extensions.
 */

#include <stdint.h>
#include <stdio.h>

#if defined(_WIN32)
#if defined(ETA_NATIVE_EXPORT_SYMBOLS)
#define ETA_NATIVE_EXPORT __declspec(dllexport)
#else
#define ETA_NATIVE_EXPORT __declspec(dllimport)
#endif
#else
#define ETA_NATIVE_EXPORT __attribute__((visibility("default")))
#endif

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief ABI identifier expected by Eta native sidecar loader v1.
 */
#define ETA_NATIVE_ABI_ID_V1 "eta-native-v1"

/**
 * @brief Success status for native sidecar callbacks.
 */
#define ETA_NATIVE_STATUS_OK 0

/**
 * @brief Generic error status for native sidecar callbacks.
 */
#define ETA_NATIVE_STATUS_ERROR 1

/**
 * @brief Runtime callback used by sidecars to register primitive metadata.
 *
 * @param user_data Opaque runtime context pointer.
 * @param name Primitive symbol name.
 * @param arity Fixed arity for the primitive.
 * @param has_rest Non-zero when the primitive accepts a rest argument.
 * @param callable Opaque callable pointer owned by the sidecar.
 * @return ETA_NATIVE_STATUS_OK on success.
 */
typedef int (*EtaRegisterPrimitiveFnV1)(void* user_data,
                                        const char* name,
                                        uint32_t arity,
                                        uint8_t has_rest,
                                        void* callable);

/**
 * @brief Runtime callback used by sidecars to report detailed error text.
 *
 * @param user_data Opaque runtime context pointer.
 * @param message UTF-8 diagnostic message.
 */
typedef void (*EtaReportErrorFnV1)(void* user_data, const char* message);

/**
 * @brief VTable contract for sidecar-managed native heap payloads.
 */
typedef struct EtaNativeObjectVTable {
    /**
     * Human-readable type name used by diagnostics/inspection surfaces.
     */
    const char* type_name;

    /**
     * Called when the wrapper heap object is destroyed.
     */
    void (*destroy)(void* user_data);

    /**
     * Optional trace callback reserved for GC integration.
     */
    void (*trace)(void* user_data, void* ctx, void (*trace_fn)(void* ctx, uint64_t val));

    /**
     * Optional display callback reserved for inspector rendering.
     */
    void (*display)(void* user_data, FILE* out);
} EtaNativeObjectVTable;

/**
 * @brief Runtime API table passed to sidecar entrypoints.
 */
typedef struct EtaNativeApiV1 {
    uint32_t struct_size;
    const char* abi_id;
    void* user_data;
    /**
     * Optional runtime binding pointer supplied by the host runtime.
     */
    void* runtime_context;
    EtaRegisterPrimitiveFnV1 register_primitive;
    EtaReportErrorFnV1 report_error;
} EtaNativeApiV1;

/**
 * @brief Sidecar metadata returned by native extension entrypoints.
 */
typedef struct EtaExtensionInfoV1 {
    uint32_t struct_size;
    const char* abi_id;
    const char* extension_id;
    const char* extension_version;
} EtaExtensionInfoV1;

/**
 * @brief Native sidecar entrypoint function signature.
 */
typedef int (*EtaRegisterExtensionFnV1)(const EtaNativeApiV1* api,
                                        EtaExtensionInfoV1* out_info);

/**
 * @brief Default symbol name expected by Eta sidecar loader for ABI v1.
 */
ETA_NATIVE_EXPORT int eta_register_extension_v1(const EtaNativeApiV1* api,
                                                EtaExtensionInfoV1* out_info);

#ifdef __cplusplus
} // extern "C"
#endif
