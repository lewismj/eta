cmake_minimum_required(VERSION 3.28)

foreach(required_var
        PACKAGE_SOURCE_ROOT
        FIXTURE_ROOT
        SIDECAR_BINARY
        ETA_ETAI_EXECUTABLE
        ETA_STDLIB_DIR
        HOST_TARGET_TRIPLE)
    if(NOT DEFINED ${required_var} OR "${${required_var}}" STREQUAL "")
        message(FATAL_ERROR "Missing required variable: ${required_var}")
    endif()
endforeach()

if(NOT EXISTS "${SIDECAR_BINARY}")
    message(FATAL_ERROR "Sidecar binary does not exist: ${SIDECAR_BINARY}")
endif()
if(NOT EXISTS "${ETA_ETAI_EXECUTABLE}")
    message(FATAL_ERROR "etai executable does not exist: ${ETA_ETAI_EXECUTABLE}")
endif()

set(lightgbm_root "${FIXTURE_ROOT}/lightgbm")
set(app_root "${FIXTURE_ROOT}/app")
set(app_tests_root "${app_root}/tests")
set(lightgbm_src_root "${lightgbm_root}/src")

if("${HOST_TARGET_TRIPLE}" MATCHES "^x86_64-")
    set(host_arch "amd64")
elseif("${HOST_TARGET_TRIPLE}" MATCHES "^aarch64-")
    set(host_arch "arm64")
else()
    message(FATAL_ERROR
        "Unsupported HOST_TARGET_TRIPLE architecture for test fixture: ${HOST_TARGET_TRIPLE}")
endif()

set(lightgbm_libs_root "${lightgbm_root}/libs/${host_arch}")

file(REMOVE_RECURSE "${FIXTURE_ROOT}")
file(MAKE_DIRECTORY
    "${lightgbm_src_root}/ml"
    "${lightgbm_libs_root}"
    "${app_tests_root}")

get_filename_component(sidecar_name "${SIDECAR_BINARY}" NAME)
set(staged_sidecar "${lightgbm_libs_root}/${sidecar_name}")

file(COPY_FILE "${PACKAGE_SOURCE_ROOT}/src/ml/lightgbm.eta"
               "${lightgbm_src_root}/ml/lightgbm.eta")
file(COPY_FILE "${PACKAGE_SOURCE_ROOT}/tests/eta/lightgbm_smoke.test.eta"
               "${app_tests_root}/lightgbm_smoke.test.eta")
file(COPY_FILE "${SIDECAR_BINARY}" "${staged_sidecar}" ONLY_IF_DIFFERENT)
file(SHA256 "${staged_sidecar}" sidecar_sha256)

file(WRITE "${lightgbm_root}/eta.toml"
"[package]
name = \"eta-lightgbm\"
version = \"0.1.0\"
license = \"MIT\"

[compatibility]
eta = \">=0.6, <0.8\"

[native]
kind = \"sidecar\"
abi = \"eta-native-v1\"
id = \"eta.lgbm.sidecar\"
entry = \"eta_register_lgbm_extension_v1\"

[[native.targets]]
triple = \"${HOST_TARGET_TRIPLE}\"
artifact = \"libs/${host_arch}/${sidecar_name}\"
sha256 = \"${sidecar_sha256}\"
")

file(WRITE "${app_root}/eta.toml"
"[package]
name = \"lightgbm_app\"
version = \"0.1.0\"
license = \"MIT\"

[compatibility]
eta = \">=0.6, <0.8\"

[dependencies]
eta-lightgbm = { path = \"../lightgbm\" }
")

file(WRITE "${app_root}/eta.lock"
"version = 1

[[package]]
name = \"lightgbm_app\"
version = \"0.1.0\"
source = \"root\"
dependencies = [\"eta-lightgbm@0.1.0\"]

[[package]]
name = \"eta-lightgbm\"
version = \"0.1.0\"
source = \"path+../lightgbm\"
native_id = \"eta.lgbm.sidecar\"
native_abi = \"eta-native-v1\"
native_entry = \"eta_register_lgbm_extension_v1\"
native_target_triple = \"${HOST_TARGET_TRIPLE}\"
native_artifact_relpath = \"libs/${host_arch}/${sidecar_name}\"
native_sha256 = \"${sidecar_sha256}\"
dependencies = []
")

if(WIN32)
    set(path_sep ";")
else()
    set(path_sep ":")
endif()

set(module_path "${ETA_STDLIB_DIR}${path_sep}${lightgbm_src_root}")
if(WIN32)
    string(REPLACE ";" "\\;" module_path "${module_path}")
endif()

execute_process(
    COMMAND "${CMAKE_COMMAND}" -E env "ETA_MODULE_PATH=${module_path}" "${ETA_ETAI_EXECUTABLE}"
            "${app_tests_root}/lightgbm_smoke.test.eta"
    WORKING_DIRECTORY "${app_root}"
    RESULT_VARIABLE etai_status
    OUTPUT_VARIABLE etai_stdout
    ERROR_VARIABLE etai_stderr
)

if(NOT etai_status EQUAL 0)
    message(FATAL_ERROR
        "LightGBM eta smoke tests failed.\n"
        "STDOUT:\n${etai_stdout}\n"
        "STDERR:\n${etai_stderr}\n")
endif()
