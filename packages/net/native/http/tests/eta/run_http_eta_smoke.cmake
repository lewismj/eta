cmake_minimum_required(VERSION 3.28)

foreach(required_var
        PACKAGE_SOURCE_ROOT
        FIXTURE_ROOT
        SIDECAR_BINARY
        ETA_ETAI_EXECUTABLE
        ETA_STDLIB_DIR
        HOST_TARGET_TRIPLE
        PYTHON_EXECUTABLE
        LOOPBACK_SERVER_SCRIPT)
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
if(NOT IS_DIRECTORY "${ETA_STDLIB_DIR}")
    message(FATAL_ERROR "ETA_STDLIB_DIR does not exist: ${ETA_STDLIB_DIR}")
endif()
if(NOT EXISTS "${PYTHON_EXECUTABLE}")
    message(FATAL_ERROR "python executable does not exist: ${PYTHON_EXECUTABLE}")
endif()
if(NOT EXISTS "${LOOPBACK_SERVER_SCRIPT}")
    message(FATAL_ERROR "loopback fixture script does not exist: ${LOOPBACK_SERVER_SCRIPT}")
endif()

set(http_root "${FIXTURE_ROOT}/http")
set(app_root "${FIXTURE_ROOT}/app")
set(app_tests_root "${app_root}/tests")
set(http_src_root "${http_root}/src")
set(cookbook_net_root "${http_src_root}/cookbook/net")
get_filename_component(repo_root "${PACKAGE_SOURCE_ROOT}/../../../.." ABSOLUTE)
set(repo_cookbook_net_root "${repo_root}/cookbook/net")

if("${HOST_TARGET_TRIPLE}" MATCHES "^x86_64-")
    set(host_arch "amd64")
elseif("${HOST_TARGET_TRIPLE}" MATCHES "^aarch64-")
    set(host_arch "arm64")
else()
    message(FATAL_ERROR
        "Unsupported HOST_TARGET_TRIPLE architecture for test fixture: ${HOST_TARGET_TRIPLE}")
endif()

set(http_libs_root "${http_root}/libs/${host_arch}")

file(REMOVE_RECURSE "${FIXTURE_ROOT}")
file(MAKE_DIRECTORY
    "${http_src_root}/net"
    "${http_libs_root}"
    "${app_tests_root}"
    "${cookbook_net_root}")

get_filename_component(sidecar_name "${SIDECAR_BINARY}" NAME)
set(staged_sidecar "${http_libs_root}/${sidecar_name}")

file(COPY_FILE "${PACKAGE_SOURCE_ROOT}/src/net/http.eta"
               "${http_src_root}/net/http.eta")
file(COPY_FILE "${PACKAGE_SOURCE_ROOT}/tests/eta/http_smoke.test.eta"
               "${app_tests_root}/http_smoke.test.eta")
file(COPY_FILE "${repo_cookbook_net_root}/http-quickstart.eta"
               "${cookbook_net_root}/http-quickstart.eta")
file(COPY_FILE "${repo_cookbook_net_root}/rest-client.eta"
               "${cookbook_net_root}/rest-client.eta")
file(COPY_FILE "${repo_cookbook_net_root}/download-large-file.eta"
               "${cookbook_net_root}/download-large-file.eta")
file(COPY_FILE "${SIDECAR_BINARY}" "${staged_sidecar}" ONLY_IF_DIFFERENT)
if(WIN32)
    get_filename_component(sidecar_dir "${SIDECAR_BINARY}" DIRECTORY)
    file(GLOB runtime_dlls "${sidecar_dir}/*.dll")
    foreach(runtime_dll IN LISTS runtime_dlls)
        get_filename_component(runtime_name "${runtime_dll}" NAME)
        if(NOT runtime_name STREQUAL "${sidecar_name}")
            file(COPY_FILE "${runtime_dll}" "${http_libs_root}/${runtime_name}" ONLY_IF_DIFFERENT)
        endif()
    endforeach()
endif()
file(SHA256 "${staged_sidecar}" sidecar_sha256)

file(WRITE "${http_root}/eta.toml"
"[package]
name = \"eta-http\"
version = \"0.1.0\"
license = \"MIT\"

[compatibility]
eta = \">=0.6, <0.8\"

[native]
kind = \"sidecar\"
abi = \"eta-native-v1\"
id = \"eta.http.sidecar\"
entry = \"eta_register_http_extension_v1\"

[[native.targets]]
triple = \"${HOST_TARGET_TRIPLE}\"
artifact = \"libs/${host_arch}/${sidecar_name}\"
sha256 = \"${sidecar_sha256}\"
")

file(WRITE "${app_root}/eta.toml"
"[package]
name = \"http_app\"
version = \"0.1.0\"
license = \"MIT\"

[compatibility]
eta = \">=0.6, <0.8\"

[dependencies]
eta-http = { path = \"../http\" }
")

file(WRITE "${app_root}/eta.lock"
"version = 1

[[package]]
name = \"http_app\"
version = \"0.1.0\"
source = \"root\"
dependencies = [\"eta-http@0.1.0\"]

[[package]]
name = \"eta-http\"
version = \"0.1.0\"
source = \"path+../http\"
native_id = \"eta.http.sidecar\"
native_abi = \"eta-native-v1\"
native_entry = \"eta_register_http_extension_v1\"
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

set(module_path "${ETA_STDLIB_DIR}${path_sep}${http_src_root}")
if(WIN32)
    string(REPLACE ";" "\\;" module_path "${module_path}")
endif()

execute_process(
    COMMAND "${PYTHON_EXECUTABLE}" "${LOOPBACK_SERVER_SCRIPT}" --run
            "${CMAKE_COMMAND}" -E env "ETA_MODULE_PATH=${module_path}" "${ETA_ETAI_EXECUTABLE}"
            "${app_tests_root}/http_smoke.test.eta"
    WORKING_DIRECTORY "${app_root}"
    RESULT_VARIABLE etai_status
    OUTPUT_VARIABLE etai_stdout
    ERROR_VARIABLE etai_stderr
)

if(NOT etai_status EQUAL 0)
    message(FATAL_ERROR
        "HTTP Eta smoke test failed.\n"
        "STDOUT:\n${etai_stdout}\n"
        "STDERR:\n${etai_stderr}\n")
endif()
