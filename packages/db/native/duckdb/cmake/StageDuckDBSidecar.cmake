cmake_minimum_required(VERSION 3.28)

foreach(required_var
        PACKAGE_ROOT
        SIDECAR_BINARY
        HOST_TARGET_TRIPLE)
    if(NOT DEFINED ${required_var} OR "${${required_var}}" STREQUAL "")
        message(FATAL_ERROR "Missing required variable: ${required_var}")
    endif()
endforeach()

if(NOT EXISTS "${SIDECAR_BINARY}")
    message(FATAL_ERROR "Sidecar binary does not exist: ${SIDECAR_BINARY}")
endif()

set(manifest_path "${PACKAGE_ROOT}/eta.toml")
if(NOT EXISTS "${manifest_path}")
    message(FATAL_ERROR "Package manifest does not exist: ${manifest_path}")
endif()

if("${HOST_TARGET_TRIPLE}" STREQUAL "x86_64-pc-windows-msvc")
    set(artifact_relpath "libs/amd64/eta_duckdb.dll")
elseif("${HOST_TARGET_TRIPLE}" STREQUAL "x86_64-unknown-linux-gnu")
    set(artifact_relpath "libs/amd64/libeta_duckdb.so")
elseif("${HOST_TARGET_TRIPLE}" STREQUAL "x86_64-apple-darwin")
    set(artifact_relpath "libs/amd64/libeta_duckdb.dylib")
elseif("${HOST_TARGET_TRIPLE}" STREQUAL "aarch64-apple-darwin")
    set(artifact_relpath "libs/arm64/libeta_duckdb.dylib")
else()
    message(FATAL_ERROR
        "Unsupported host target triple for eta-duckdb staging: ${HOST_TARGET_TRIPLE}")
endif()

set(artifact_path "${PACKAGE_ROOT}/${artifact_relpath}")
get_filename_component(artifact_dir "${artifact_path}" DIRECTORY)
file(MAKE_DIRECTORY "${artifact_dir}")

file(COPY_FILE "${SIDECAR_BINARY}" "${artifact_path}" ONLY_IF_DIFFERENT)
file(SHA256 "${artifact_path}" artifact_sha256)

file(STRINGS "${manifest_path}" manifest_lines)
set(updated_manifest "")
set(in_target FALSE)
set(current_triple "")
set(updated_sha FALSE)

foreach(line IN LISTS manifest_lines)
    set(out_line "${line}")

    if("${line}" STREQUAL "[[native.targets]]")
        set(in_target TRUE)
        set(current_triple "")
    elseif("${line}" MATCHES "^\\[.+\\]$")
        set(in_target FALSE)
        set(current_triple "")
    elseif(in_target AND "${line}" MATCHES "^triple = \"([^\"]+)\"$")
        set(current_triple "${CMAKE_MATCH_1}")
    elseif(in_target
           AND "${current_triple}" STREQUAL "${HOST_TARGET_TRIPLE}"
           AND "${line}" MATCHES "^sha256 = \"[0-9a-fA-F]+\"$")
        set(out_line "sha256 = \"${artifact_sha256}\"")
        set(updated_sha TRUE)
    endif()

    string(APPEND updated_manifest "${out_line}\n")
endforeach()

if(NOT updated_sha)
    message(FATAL_ERROR
        "Could not find sha256 row for host triple ${HOST_TARGET_TRIPLE} in ${manifest_path}")
endif()

file(WRITE "${manifest_path}" "${updated_manifest}")

message(STATUS
    "Staged ${artifact_relpath} and updated sha256 for ${HOST_TARGET_TRIPLE}: ${artifact_sha256}")
