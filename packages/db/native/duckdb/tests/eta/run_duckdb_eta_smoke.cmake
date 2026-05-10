cmake_minimum_required(VERSION 3.28)

foreach(required_var
        PACKAGE_SOURCE_ROOT
        FIXTURE_ROOT
        SIDECAR_BINARY
        ETA_ETA_EXECUTABLE
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
if(NOT EXISTS "${ETA_ETA_EXECUTABLE}")
    message(FATAL_ERROR "eta executable does not exist: ${ETA_ETA_EXECUTABLE}")
endif()
if(NOT EXISTS "${ETA_ETAI_EXECUTABLE}")
    message(FATAL_ERROR "etai executable does not exist: ${ETA_ETAI_EXECUTABLE}")
endif()
if(NOT IS_DIRECTORY "${ETA_STDLIB_DIR}")
    message(FATAL_ERROR "ETA_STDLIB_DIR does not exist: ${ETA_STDLIB_DIR}")
endif()

set(duckdb_root "${FIXTURE_ROOT}/duckdb")
set(app_root "${FIXTURE_ROOT}/app")
set(app_tests_root "${app_root}/tests")
set(duckdb_src_root "${duckdb_root}/src")

if("${HOST_TARGET_TRIPLE}" MATCHES "^x86_64-")
    set(host_arch "amd64")
elseif("${HOST_TARGET_TRIPLE}" MATCHES "^aarch64-")
    set(host_arch "arm64")
else()
    message(FATAL_ERROR
        "Unsupported HOST_TARGET_TRIPLE architecture for test fixture: ${HOST_TARGET_TRIPLE}")
endif()

set(duckdb_libs_root "${duckdb_root}/native/${host_arch}/libs")

file(REMOVE_RECURSE "${FIXTURE_ROOT}")
file(MAKE_DIRECTORY
    "${duckdb_src_root}/db"
    "${duckdb_libs_root}"
    "${app_tests_root}")

get_filename_component(sidecar_name "${SIDECAR_BINARY}" NAME)
set(staged_sidecar "${duckdb_libs_root}/${sidecar_name}")

file(COPY_FILE "${PACKAGE_SOURCE_ROOT}/src/db/duckdb.eta"
               "${duckdb_src_root}/db/duckdb.eta")
file(COPY_FILE "${PACKAGE_SOURCE_ROOT}/tests/eta/duckdb_smoke.test.eta"
               "${app_tests_root}/duckdb_smoke.test.eta")
file(COPY_FILE "${SIDECAR_BINARY}" "${staged_sidecar}" ONLY_IF_DIFFERENT)
file(SHA256 "${staged_sidecar}" sidecar_sha256)

file(WRITE "${duckdb_root}/eta.toml"
"[package]
name = \"eta-duckdb\"
version = \"0.1.0\"
license = \"MIT\"

[compatibility]
eta = \">=0.6, <0.8\"

[native]
kind = \"sidecar\"
abi = \"eta-native-v1\"
id = \"eta.duckdb.sidecar\"
entry = \"eta_register_duckdb_extension_v1\"

[[native.targets]]
triple = \"${HOST_TARGET_TRIPLE}\"
artifact = \"native/${host_arch}/libs/${sidecar_name}\"
sha256 = \"${sidecar_sha256}\"
")

execute_process(
    COMMAND "${ETA_ETA_EXECUTABLE}" tree --manifest-path "${duckdb_root}/eta.toml"
    RESULT_VARIABLE sidecar_tree_status
    OUTPUT_VARIABLE sidecar_tree_stdout
    ERROR_VARIABLE sidecar_tree_stderr
)
if(NOT sidecar_tree_status EQUAL 0)
    message(FATAL_ERROR
        "DuckDB sidecar metadata parse failed.\n"
        "STDOUT:\n${sidecar_tree_stdout}\n"
        "STDERR:\n${sidecar_tree_stderr}\n")
endif()
string(FIND "${sidecar_tree_stdout}" "eta-duckdb v0.1.0" sidecar_match)
if(sidecar_match EQUAL -1)
    message(FATAL_ERROR
        "DuckDB sidecar tree output did not contain expected package id.\n"
        "STDOUT:\n${sidecar_tree_stdout}\n")
endif()

file(WRITE "${app_root}/eta.toml"
"[package]
name = \"duckdb_app\"
version = \"0.1.0\"
license = \"MIT\"

[compatibility]
eta = \">=0.6, <0.8\"

[dependencies]
eta-duckdb = { path = \"../duckdb\" }
")

file(WRITE "${app_root}/eta.lock"
"version = 1

[[package]]
name = \"duckdb_app\"
version = \"0.1.0\"
source = \"root\"
dependencies = [\"eta-duckdb@0.1.0\"]

[[package]]
name = \"eta-duckdb\"
version = \"0.1.0\"
source = \"path+../duckdb\"
native_id = \"eta.duckdb.sidecar\"
native_abi = \"eta-native-v1\"
native_entry = \"eta_register_duckdb_extension_v1\"
native_target_triple = \"${HOST_TARGET_TRIPLE}\"
native_artifact_relpath = \"native/${host_arch}/libs/${sidecar_name}\"
native_sha256 = \"${sidecar_sha256}\"
dependencies = []
")

if(WIN32)
    set(path_sep ";")
else()
    set(path_sep ":")
endif()

set(module_path "${ETA_STDLIB_DIR}${path_sep}${duckdb_src_root}")
if(WIN32)
    string(REPLACE ";" "\\;" module_path "${module_path}")
endif()

execute_process(
    COMMAND "${CMAKE_COMMAND}" -E env "ETA_MODULE_PATH=${module_path}" "${ETA_ETAI_EXECUTABLE}"
            "${app_tests_root}/duckdb_smoke.test.eta"
    WORKING_DIRECTORY "${app_root}"
    RESULT_VARIABLE etai_status
    OUTPUT_VARIABLE etai_stdout
    ERROR_VARIABLE etai_stderr
)
if(NOT etai_status EQUAL 0)
    message(FATAL_ERROR
        "DuckDB Eta smoke test failed.\n"
        "STDOUT:\n${etai_stdout}\n"
        "STDERR:\n${etai_stderr}\n")
endif()
