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

add_library(eta_duckdb MODULE
    src/eta/duckdb/duckdb_extension.cpp
    src/eta/duckdb/duckdb_primitives.cpp
)
set_target_properties(eta_duckdb PROPERTIES
    OUTPUT_NAME eta_duckdb
    CXX_SCAN_FOR_MODULES OFF
)
target_compile_definitions(eta_duckdb PRIVATE
    ETA_NATIVE_EXPORT_SYMBOLS=1
)
target_include_directories(eta_duckdb PRIVATE
    ${ETA_CORE_SRC_DIR}
    ${Boost_INCLUDE_DIRS}
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

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
    target_include_directories(eta_duckdb_unit_tests PRIVATE
        ${ETA_CORE_SRC_DIR}
        ${Boost_INCLUDE_DIRS}
        ${CMAKE_CURRENT_SOURCE_DIR}/src
    )
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
