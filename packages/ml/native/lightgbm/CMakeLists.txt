cmake_minimum_required(VERSION 3.28)

project(eta_lightgbm LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)

get_filename_component(ETA_REPO_ROOT_DIR "${CMAKE_CURRENT_LIST_DIR}/../../../.." ABSOLUTE)
set(ETA_CORE_SRC_DIR "${ETA_REPO_ROOT_DIR}/eta/core/src")
set(ETA_CORE_LIBRARY "" CACHE FILEPATH
    "Path to prebuilt eta_core static library for standalone sidecar builds.")
set(ETA_STDLIB_DIR "${ETA_REPO_ROOT_DIR}/stdlib" CACHE PATH
    "Path to the Eta stdlib source directory.")
set(ETA_ETAI_EXECUTABLE "" CACHE FILEPATH
    "Path to etai executable used for package-local Eta smoke tests.")

option(ETA_LIGHTGBM_FETCH_UPSTREAM
    "Fetch pinned LightGBM upstream sources for package development."
    OFF)
option(ETA_LIGHTGBM_ENABLE_TESTS "Build package-local tests." ON)
option(ETA_LIGHTGBM_ENABLE_ETA_TESTS "Run package-local Eta smoke tests." ON)

if(POLICY CMP0167)
    cmake_policy(SET CMP0167 OLD)
endif()

if(ETA_LIGHTGBM_FETCH_UPSTREAM)
    include(cmake/FetchLightGBM.cmake)
    eta_lightgbm_fetch()
endif()

find_package(Boost 1.88 REQUIRED)

function(eta_lightgbm_resolve_core_target out_target)
    if(TARGET eta_core)
        set(${out_target} eta_core PARENT_SCOPE)
        return()
    endif()

    set(_eta_core_candidates "")
    if(NOT "${ETA_CORE_LIBRARY}" STREQUAL "")
        list(APPEND _eta_core_candidates "${ETA_CORE_LIBRARY}")
    endif()
    if(WIN32)
        list(APPEND _eta_core_candidates
            "${ETA_REPO_ROOT_DIR}/out/msvc-release/eta/core/eta_core.lib"
            "${ETA_REPO_ROOT_DIR}/out/release/eta/core/eta_core.lib"
        )
    else()
        list(APPEND _eta_core_candidates
            "${ETA_REPO_ROOT_DIR}/out/release/eta/core/libeta_core.a"
            "${ETA_REPO_ROOT_DIR}/out/msvc-release/eta/core/libeta_core.a"
        )
    endif()

    foreach(_candidate IN LISTS _eta_core_candidates)
        if(EXISTS "${_candidate}")
            if(NOT TARGET eta_core_imported)
                add_library(eta_core_imported UNKNOWN IMPORTED)
                set_target_properties(eta_core_imported PROPERTIES
                    IMPORTED_LOCATION "${_candidate}"
                )
            endif()
            set(${out_target} eta_core_imported PARENT_SCOPE)
            return()
        endif()
    endforeach()

    message(FATAL_ERROR
        "Could not resolve eta_core for eta_lightgbm. "
        "Build eta_core first or pass -DETA_CORE_LIBRARY=/abs/path/to/eta_core.lib")
endfunction()

eta_lightgbm_resolve_core_target(ETA_LIGHTGBM_CORE_TARGET)

set(ETA_LIGHTGBM_LIBRARY_TARGET "")
set(ETA_LIGHTGBM_INCLUDE_DIR "")

if(TARGET _lightgbm)
    set(ETA_LIGHTGBM_LIBRARY_TARGET _lightgbm)
    if(DEFINED ETA_LIGHTGBM_SOURCE_DIR AND NOT "${ETA_LIGHTGBM_SOURCE_DIR}" STREQUAL "")
        set(ETA_LIGHTGBM_INCLUDE_DIR "${ETA_LIGHTGBM_SOURCE_DIR}/include")
    endif()
else()
    find_path(ETA_LIGHTGBM_INCLUDE_DIR
        NAMES LightGBM/c_api.h
    )
    find_library(ETA_LIGHTGBM_LIBRARY_PATH
        NAMES lightgbm lib_lightgbm _lightgbm
    )
    if(ETA_LIGHTGBM_INCLUDE_DIR AND ETA_LIGHTGBM_LIBRARY_PATH)
        add_library(eta_lightgbm_external UNKNOWN IMPORTED)
        set_target_properties(eta_lightgbm_external PROPERTIES
            IMPORTED_LOCATION "${ETA_LIGHTGBM_LIBRARY_PATH}"
        )
        set(ETA_LIGHTGBM_LIBRARY_TARGET eta_lightgbm_external)
    endif()
endif()

if("${ETA_LIGHTGBM_LIBRARY_TARGET}" STREQUAL "")
    message(FATAL_ERROR
        "Could not find LightGBM C API library. "
        "Set ETA_LIGHTGBM_FETCH_UPSTREAM=ON or install LightGBM and provide headers/libs via CMake search paths.")
endif()

if("${ETA_LIGHTGBM_INCLUDE_DIR}" STREQUAL "")
    message(FATAL_ERROR
        "Could not find LightGBM headers (LightGBM/c_api.h).")
endif()

add_library(eta_lightgbm MODULE
    src/eta/lightgbm/lightgbm_extension.cpp
    src/eta/lightgbm/lightgbm_primitives.cpp
    src/eta/lightgbm/lightgbm_model.cpp
)

# Route the sidecar MODULE directly into the source-tree libs/ location that
# eta.toml declares, so cmake --install and cp -a pick it up without a
# separate staging copy step.
if(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64")
    set(_ETA_LIGHTGBM_ARCH_DIR "arm64")
else()
    set(_ETA_LIGHTGBM_ARCH_DIR "amd64")
endif()
set_target_properties(eta_lightgbm PROPERTIES
    OUTPUT_NAME eta_lightgbm
    CXX_SCAN_FOR_MODULES OFF
    LIBRARY_OUTPUT_DIRECTORY          "${CMAKE_CURRENT_SOURCE_DIR}/libs/${_ETA_LIGHTGBM_ARCH_DIR}"
    RUNTIME_OUTPUT_DIRECTORY          "${CMAKE_CURRENT_SOURCE_DIR}/libs/${_ETA_LIGHTGBM_ARCH_DIR}"
    LIBRARY_OUTPUT_DIRECTORY_RELEASE  "${CMAKE_CURRENT_SOURCE_DIR}/libs/${_ETA_LIGHTGBM_ARCH_DIR}"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE  "${CMAKE_CURRENT_SOURCE_DIR}/libs/${_ETA_LIGHTGBM_ARCH_DIR}"
    LIBRARY_OUTPUT_DIRECTORY_DEBUG    "${CMAKE_CURRENT_SOURCE_DIR}/libs/${_ETA_LIGHTGBM_ARCH_DIR}"
    RUNTIME_OUTPUT_DIRECTORY_DEBUG    "${CMAKE_CURRENT_SOURCE_DIR}/libs/${_ETA_LIGHTGBM_ARCH_DIR}"
)
target_compile_definitions(eta_lightgbm PRIVATE
    ETA_NATIVE_EXPORT_SYMBOLS=1
)
target_include_directories(eta_lightgbm PRIVATE
    ${ETA_CORE_SRC_DIR}
    ${ETA_LIGHTGBM_INCLUDE_DIR}
    ${Boost_INCLUDE_DIRS}
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_link_libraries(eta_lightgbm PRIVATE
    ${ETA_LIGHTGBM_LIBRARY_TARGET}
    ${ETA_LIGHTGBM_CORE_TARGET}
)

if(ETA_LIGHTGBM_ENABLE_TESTS)
    enable_testing()

    add_executable(eta_lightgbm_unit_tests
        tests/unit/lightgbm_model_tests.cpp
        src/eta/lightgbm/lightgbm_model.cpp
    )
    set_target_properties(eta_lightgbm_unit_tests PROPERTIES
        CXX_SCAN_FOR_MODULES OFF
    )
    target_include_directories(eta_lightgbm_unit_tests PRIVATE
        ${ETA_CORE_SRC_DIR}
        ${ETA_LIGHTGBM_INCLUDE_DIR}
        ${Boost_INCLUDE_DIRS}
        ${CMAKE_CURRENT_SOURCE_DIR}/src
    )
    target_link_libraries(eta_lightgbm_unit_tests PRIVATE
        ${ETA_LIGHTGBM_LIBRARY_TARGET}
    )
    add_test(NAME eta_lightgbm_unit_tests COMMAND eta_lightgbm_unit_tests)

    if(ETA_LIGHTGBM_ENABLE_ETA_TESTS)
        if(ETA_ETAI_EXECUTABLE STREQUAL "")
            message(FATAL_ERROR
                "ETA_ETAI_EXECUTABLE must be set when ETA tests are enabled.")
        endif()
        if(NOT EXISTS "${ETA_ETAI_EXECUTABLE}")
            message(FATAL_ERROR
                "ETA_ETAI_EXECUTABLE does not exist: ${ETA_ETAI_EXECUTABLE}")
        endif()
        if(NOT IS_DIRECTORY "${ETA_STDLIB_DIR}")
            message(FATAL_ERROR "ETA_STDLIB_DIR does not exist: ${ETA_STDLIB_DIR}")
        endif()

        if(WIN32)
            if(CMAKE_SYSTEM_PROCESSOR MATCHES "^[Aa][Rr][Mm]64|aarch64")
                set(ETA_LIGHTGBM_HOST_TRIPLE "aarch64-pc-windows-msvc")
            else()
                set(ETA_LIGHTGBM_HOST_TRIPLE "x86_64-pc-windows-msvc")
            endif()
        elseif(APPLE)
            if(CMAKE_SYSTEM_PROCESSOR MATCHES "arm64|aarch64")
                set(ETA_LIGHTGBM_HOST_TRIPLE "aarch64-apple-darwin")
            else()
                set(ETA_LIGHTGBM_HOST_TRIPLE "x86_64-apple-darwin")
            endif()
        elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64")
            set(ETA_LIGHTGBM_HOST_TRIPLE "aarch64-unknown-linux-gnu")
        else()
            set(ETA_LIGHTGBM_HOST_TRIPLE "x86_64-unknown-linux-gnu")
        endif()

        add_test(
            NAME eta_lightgbm_eta_smoke
            COMMAND ${CMAKE_COMMAND}
                -DPACKAGE_SOURCE_ROOT=${CMAKE_CURRENT_SOURCE_DIR}
                -DFIXTURE_ROOT=${CMAKE_CURRENT_BINARY_DIR}/eta_fixture
                -DSIDECAR_BINARY=$<TARGET_FILE:eta_lightgbm>
                -DETA_ETAI_EXECUTABLE=${ETA_ETAI_EXECUTABLE}
                -DETA_STDLIB_DIR=${ETA_STDLIB_DIR}
                -DHOST_TARGET_TRIPLE=${ETA_LIGHTGBM_HOST_TRIPLE}
                -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/eta/run_lightgbm_eta_smoke.cmake
        )
    endif()
endif()
