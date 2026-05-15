cmake_minimum_required(VERSION 3.28)

project(eta_duckdb LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)

get_filename_component(ETA_REPO_ROOT_DIR "${CMAKE_CURRENT_LIST_DIR}/../../../.." ABSOLUTE)
set(ETA_CORE_SRC_DIR "${ETA_REPO_ROOT_DIR}/eta/core/src")
set(ETA_STDLIB_DIR "${ETA_REPO_ROOT_DIR}/stdlib" CACHE PATH
    "Path to the Eta stdlib source directory.")
set(ETA_ETA_EXECUTABLE "" CACHE FILEPATH
    "Path to eta CLI executable used by package-local metadata tests.")
set(ETA_ETAI_EXECUTABLE "" CACHE FILEPATH
    "Path to etai executable used by package-local Eta smoke tests.")

option(ETA_DUCKDB_FETCH_UPSTREAM
    "Fetch pinned DuckDB upstream sources for package development."
    OFF)
option(ETA_DUCKDB_ENABLE_TESTS "Build package-local tests." ON)
option(ETA_DUCKDB_ENABLE_ETA_TESTS "Run package-local Eta smoke tests." ON)

if(POLICY CMP0167)
    cmake_policy(SET CMP0167 OLD)
endif()

find_package(Boost 1.88 REQUIRED)

if(ETA_DUCKDB_FETCH_UPSTREAM)
    include(cmake/FetchDuckDB.cmake)
    eta_duckdb_fetch()
endif()

set(ETA_DUCKDB_LIBRARY_TARGET "")
set(ETA_DUCKDB_INCLUDE_DIR "")
set(ETA_DUCKDB_NEEDS_STATIC_DEFINE OFF)
set(ETA_DUCKDB_NEEDS_RUNTIME_DLL_COPY OFF)

if(WIN32 AND TARGET duckdb)
    set(ETA_DUCKDB_LIBRARY_TARGET duckdb)
    set(ETA_DUCKDB_NEEDS_RUNTIME_DLL_COPY ON)
elseif(TARGET duckdb_static)
    set(ETA_DUCKDB_LIBRARY_TARGET duckdb_static)
    set(ETA_DUCKDB_NEEDS_STATIC_DEFINE ON)
elseif(TARGET duckdb)
    set(ETA_DUCKDB_LIBRARY_TARGET duckdb)
    if(WIN32)
        set(ETA_DUCKDB_NEEDS_RUNTIME_DLL_COPY ON)
    endif()
endif()

if("${ETA_DUCKDB_LIBRARY_TARGET}" STREQUAL "")
    find_package(DuckDB QUIET CONFIG)
    if(TARGET DuckDB::duckdb)
        set(ETA_DUCKDB_LIBRARY_TARGET DuckDB::duckdb)
    elseif(TARGET DuckDB::duckdb_static)
        set(ETA_DUCKDB_LIBRARY_TARGET DuckDB::duckdb_static)
        set(ETA_DUCKDB_NEEDS_STATIC_DEFINE ON)
    endif()
endif()

if(DEFINED ETA_DUCKDB_SOURCE_DIR AND NOT "${ETA_DUCKDB_SOURCE_DIR}" STREQUAL "")
    set(ETA_DUCKDB_INCLUDE_DIR "${ETA_DUCKDB_SOURCE_DIR}/src/include")
endif()

if("${ETA_DUCKDB_LIBRARY_TARGET}" STREQUAL "")
    find_path(ETA_DUCKDB_INCLUDE_DIR
        NAMES duckdb.h
    )
    find_library(ETA_DUCKDB_LIBRARY_PATH
        NAMES duckdb duckdb_static
    )
    if(ETA_DUCKDB_LIBRARY_PATH)
        add_library(eta_duckdb_external UNKNOWN IMPORTED)
        set_target_properties(eta_duckdb_external PROPERTIES
            IMPORTED_LOCATION "${ETA_DUCKDB_LIBRARY_PATH}"
        )
        set(ETA_DUCKDB_LIBRARY_TARGET eta_duckdb_external)
    endif()
endif()

if("${ETA_DUCKDB_LIBRARY_TARGET}" STREQUAL "")
    message(FATAL_ERROR
        "Could not find DuckDB C API library. "
        "Set ETA_DUCKDB_FETCH_UPSTREAM=ON or install DuckDB and provide headers/libs via CMake search paths.")
endif()

if("${ETA_DUCKDB_INCLUDE_DIR}" STREQUAL ""
   AND "${ETA_DUCKDB_LIBRARY_TARGET}" STREQUAL "eta_duckdb_external")
    message(FATAL_ERROR
        "Could not find DuckDB headers (duckdb.h).")
endif()

add_library(eta_duckdb MODULE
    src/eta/duckdb/duckdb_extension.cpp
    src/eta/duckdb/duckdb_primitives.cpp
)

# Route the sidecar MODULE directly into the source-tree libs/ location that
# eta.toml declares, so cmake --install and cp -a pick it up without a
# separate staging copy step.
if(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64")
    set(_ETA_DUCKDB_ARCH_DIR "arm64")
else()
    set(_ETA_DUCKDB_ARCH_DIR "amd64")
endif()
set_target_properties(eta_duckdb PROPERTIES
    OUTPUT_NAME eta_duckdb
    CXX_SCAN_FOR_MODULES OFF
    LIBRARY_OUTPUT_DIRECTORY          "${CMAKE_CURRENT_SOURCE_DIR}/libs/${_ETA_DUCKDB_ARCH_DIR}"
    RUNTIME_OUTPUT_DIRECTORY          "${CMAKE_CURRENT_SOURCE_DIR}/libs/${_ETA_DUCKDB_ARCH_DIR}"
    LIBRARY_OUTPUT_DIRECTORY_RELEASE  "${CMAKE_CURRENT_SOURCE_DIR}/libs/${_ETA_DUCKDB_ARCH_DIR}"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE  "${CMAKE_CURRENT_SOURCE_DIR}/libs/${_ETA_DUCKDB_ARCH_DIR}"
    LIBRARY_OUTPUT_DIRECTORY_DEBUG    "${CMAKE_CURRENT_SOURCE_DIR}/libs/${_ETA_DUCKDB_ARCH_DIR}"
    RUNTIME_OUTPUT_DIRECTORY_DEBUG    "${CMAKE_CURRENT_SOURCE_DIR}/libs/${_ETA_DUCKDB_ARCH_DIR}"
)
target_compile_definitions(eta_duckdb PRIVATE
    ETA_NATIVE_EXPORT_SYMBOLS=1
)
if(ETA_DUCKDB_NEEDS_STATIC_DEFINE)
    target_compile_definitions(eta_duckdb PRIVATE
        DUCKDB_STATIC_BUILD=1
    )
endif()
target_include_directories(eta_duckdb PRIVATE
    ${ETA_CORE_SRC_DIR}
    ${ETA_DUCKDB_INCLUDE_DIR}
    ${Boost_INCLUDE_DIRS}
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_link_libraries(eta_duckdb PRIVATE
    ${ETA_DUCKDB_LIBRARY_TARGET}
)
if(TARGET eta_core)
    target_link_libraries(eta_duckdb PRIVATE eta_core)
endif()
if(ETA_DUCKDB_NEEDS_RUNTIME_DLL_COPY)
    add_custom_command(TARGET eta_duckdb POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_FILE:${ETA_DUCKDB_LIBRARY_TARGET}>
            $<TARGET_FILE_DIR:eta_duckdb>
        VERBATIM
    )
endif()

if(ETA_DUCKDB_ENABLE_TESTS)
    enable_testing()

    add_executable(eta_duckdb_unit_tests
        tests/unit/duckdb_metadata_tests.cpp
        src/eta/duckdb/duckdb_extension.cpp
        src/eta/duckdb/duckdb_primitives.cpp
    )
    set_target_properties(eta_duckdb_unit_tests PROPERTIES
        CXX_SCAN_FOR_MODULES OFF
    )
    target_compile_definitions(eta_duckdb_unit_tests PRIVATE
        ETA_NATIVE_EXPORT_SYMBOLS=1
    )
    if(ETA_DUCKDB_NEEDS_STATIC_DEFINE)
        target_compile_definitions(eta_duckdb_unit_tests PRIVATE
            DUCKDB_STATIC_BUILD=1
        )
    endif()
    target_include_directories(eta_duckdb_unit_tests PRIVATE
        ${ETA_CORE_SRC_DIR}
        ${ETA_DUCKDB_INCLUDE_DIR}
        ${Boost_INCLUDE_DIRS}
        ${CMAKE_CURRENT_SOURCE_DIR}/src
    )
    target_link_libraries(eta_duckdb_unit_tests PRIVATE
        ${ETA_DUCKDB_LIBRARY_TARGET}
    )
    if(TARGET eta_core)
        target_link_libraries(eta_duckdb_unit_tests PRIVATE eta_core)
    endif()
    if(ETA_DUCKDB_NEEDS_RUNTIME_DLL_COPY)
        add_custom_command(TARGET eta_duckdb_unit_tests POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_FILE:${ETA_DUCKDB_LIBRARY_TARGET}>
                $<TARGET_FILE_DIR:eta_duckdb_unit_tests>
            VERBATIM
        )
    endif()
    add_test(NAME eta_duckdb_unit_tests COMMAND eta_duckdb_unit_tests)

    if(ETA_DUCKDB_ENABLE_ETA_TESTS)
        if(ETA_ETA_EXECUTABLE STREQUAL "")
            message(FATAL_ERROR
                "ETA_ETA_EXECUTABLE must be set when ETA tests are enabled.")
        endif()
        if(NOT EXISTS "${ETA_ETA_EXECUTABLE}")
            message(FATAL_ERROR
                "ETA_ETA_EXECUTABLE does not exist: ${ETA_ETA_EXECUTABLE}")
        endif()

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
                set(ETA_DUCKDB_HOST_TRIPLE "aarch64-pc-windows-msvc")
            else()
                set(ETA_DUCKDB_HOST_TRIPLE "x86_64-pc-windows-msvc")
            endif()
        elseif(APPLE)
            if(CMAKE_SYSTEM_PROCESSOR MATCHES "arm64|aarch64")
                set(ETA_DUCKDB_HOST_TRIPLE "aarch64-apple-darwin")
            else()
                set(ETA_DUCKDB_HOST_TRIPLE "x86_64-apple-darwin")
            endif()
        elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64")
            set(ETA_DUCKDB_HOST_TRIPLE "aarch64-unknown-linux-gnu")
        else()
            set(ETA_DUCKDB_HOST_TRIPLE "x86_64-unknown-linux-gnu")
        endif()

        add_test(
            NAME eta_duckdb_eta_smoke
            COMMAND ${CMAKE_COMMAND}
                -DPACKAGE_SOURCE_ROOT=${CMAKE_CURRENT_SOURCE_DIR}
                -DFIXTURE_ROOT=${CMAKE_CURRENT_BINARY_DIR}/eta_fixture
                -DSIDECAR_BINARY=$<TARGET_FILE:eta_duckdb>
                -DETA_ETA_EXECUTABLE=${ETA_ETA_EXECUTABLE}
                -DETA_ETAI_EXECUTABLE=${ETA_ETAI_EXECUTABLE}
                -DETA_STDLIB_DIR=${ETA_STDLIB_DIR}
                -DHOST_TARGET_TRIPLE=${ETA_DUCKDB_HOST_TRIPLE}
                -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/eta/run_duckdb_eta_smoke.cmake
        )
    endif()
endif()
