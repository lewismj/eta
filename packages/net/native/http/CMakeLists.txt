cmake_minimum_required(VERSION 3.28)

project(eta_http LANGUAGES CXX)

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
    "Path to etai executable used by package-local Eta smoke tests.")

option(ETA_HTTP_ENABLE_TESTS "Build package-local tests." ON)
option(ETA_HTTP_ENABLE_ETA_TESTS "Run package-local Eta smoke tests." ON)
option(ETA_HTTP_FETCH_UPSTREAM
    "Fetch pinned libcurl sources for package development."
    OFF)

if(POLICY CMP0167)
    cmake_policy(SET CMP0167 OLD)
endif()

find_package(Boost 1.88 REQUIRED)

function(eta_http_resolve_core_target out_target)
    if(TARGET eta_core)
        set(${out_target} eta_core PARENT_SCOPE)
        return()
    endif()

    if(NOT "${ETA_CORE_LIBRARY}" STREQUAL "" AND EXISTS "${ETA_CORE_LIBRARY}")
        if(NOT TARGET eta_core_imported)
            add_library(eta_core_imported UNKNOWN IMPORTED)
            set_target_properties(eta_core_imported PROPERTIES
                IMPORTED_LOCATION "${ETA_CORE_LIBRARY}"
            )
        endif()
        set(${out_target} eta_core_imported PARENT_SCOPE)
        return()
    endif()

    message(FATAL_ERROR
        "Could not resolve eta_core for eta_http. "
        "Build eta_core first or pass -DETA_CORE_LIBRARY=/abs/path/to/libeta_core.{a,lib}")
endfunction()

eta_http_resolve_core_target(ETA_HTTP_CORE_TARGET)

set(ETA_HTTP_EXTRA_RUNTIME_DLLS "")

if(ETA_HTTP_FETCH_UPSTREAM)
    include(cmake/FetchLibcurl.cmake)
    eta_http_fetch_libcurl()
else()
    find_package(CURL 8.0 QUIET)
endif()
if(NOT TARGET CURL::libcurl)
    message(FATAL_ERROR
        "libcurl not found. Re-configure with -DETA_HTTP_FETCH_UPSTREAM=ON "
        "or install libcurl >=8.0 and ensure CURLConfig.cmake is on CMAKE_PREFIX_PATH.")
endif()

if(ETA_HTTP_FETCH_UPSTREAM)
    get_target_property(_eta_http_curl_target CURL::libcurl ALIASED_TARGET)
    if(NOT _eta_http_curl_target)
        set(_eta_http_curl_target CURL::libcurl)
    endif()

    get_target_property(_eta_http_curl_type "${_eta_http_curl_target}" TYPE)
    if(_eta_http_curl_type STREQUAL "STATIC_LIBRARY")
        get_target_property(_eta_http_curl_pic
            "${_eta_http_curl_target}" POSITION_INDEPENDENT_CODE)
        if(NOT _eta_http_curl_pic)
            message(FATAL_ERROR
                "Fetched libcurl must be built with POSITION_INDEPENDENT_CODE=ON "
                "because eta_http is linked as a MODULE shared library.")
        endif()
    endif()
    unset(_eta_http_curl_target)
    unset(_eta_http_curl_type)
    unset(_eta_http_curl_pic)
endif()

if(WIN32)
    find_package(ZLIB QUIET)
    if(ZLIB_FOUND)
        set(_eta_http_zlib_lib "")
        if(DEFINED ZLIB_LIBRARY_RELEASE AND NOT "${ZLIB_LIBRARY_RELEASE}" STREQUAL "")
            set(_eta_http_zlib_lib "${ZLIB_LIBRARY_RELEASE}")
        elseif(DEFINED ZLIB_LIBRARY AND NOT "${ZLIB_LIBRARY}" STREQUAL "")
            set(_eta_http_zlib_lib "${ZLIB_LIBRARY}")
        endif()

        if(NOT "${_eta_http_zlib_lib}" STREQUAL "")
            get_filename_component(_eta_http_zlib_libdir "${_eta_http_zlib_lib}" DIRECTORY)
            get_filename_component(_eta_http_zlib_prefix "${_eta_http_zlib_libdir}" DIRECTORY)
            foreach(_eta_http_zlib_name IN ITEMS zlib1.dll zlibd1.dll)
                set(_eta_http_zlib_candidate
                    "${_eta_http_zlib_prefix}/bin/${_eta_http_zlib_name}")
                if(EXISTS "${_eta_http_zlib_candidate}")
                    list(APPEND ETA_HTTP_EXTRA_RUNTIME_DLLS
                        "${_eta_http_zlib_candidate}")
                endif()
            endforeach()
        endif()
    endif()
endif()

add_library(eta_http MODULE
    src/eta/http/http_extension.cpp
    src/eta/http/http_multi.cpp
    src/eta/http/http_primitives.cpp
    src/eta/http/http_session.cpp
)

# Route the sidecar MODULE directly into the source-tree libs/ location that
# eta.toml declares, so cmake --install and cp -a pick it up without a
# separate staging copy step.
if(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64")
    set(_ETA_HTTP_ARCH_DIR "arm64")
else()
    set(_ETA_HTTP_ARCH_DIR "amd64")
endif()
set_target_properties(eta_http PROPERTIES
    OUTPUT_NAME eta_http
    CXX_SCAN_FOR_MODULES OFF
    LIBRARY_OUTPUT_DIRECTORY          "${CMAKE_CURRENT_SOURCE_DIR}/libs/${_ETA_HTTP_ARCH_DIR}"
    RUNTIME_OUTPUT_DIRECTORY          "${CMAKE_CURRENT_SOURCE_DIR}/libs/${_ETA_HTTP_ARCH_DIR}"
    LIBRARY_OUTPUT_DIRECTORY_RELEASE  "${CMAKE_CURRENT_SOURCE_DIR}/libs/${_ETA_HTTP_ARCH_DIR}"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE  "${CMAKE_CURRENT_SOURCE_DIR}/libs/${_ETA_HTTP_ARCH_DIR}"
    LIBRARY_OUTPUT_DIRECTORY_DEBUG    "${CMAKE_CURRENT_SOURCE_DIR}/libs/${_ETA_HTTP_ARCH_DIR}"
    RUNTIME_OUTPUT_DIRECTORY_DEBUG    "${CMAKE_CURRENT_SOURCE_DIR}/libs/${_ETA_HTTP_ARCH_DIR}"
)
target_compile_definitions(eta_http PRIVATE
    ETA_NATIVE_EXPORT_SYMBOLS=1
    NOMINMAX=1
    WIN32_LEAN_AND_MEAN=1
)
target_include_directories(eta_http PRIVATE
    ${ETA_CORE_SRC_DIR}
    ${Boost_INCLUDE_DIRS}
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_link_libraries(eta_http PRIVATE
    CURL::libcurl
    ${ETA_HTTP_CORE_TARGET}
)
if(WIN32)
    add_custom_command(TARGET eta_http POST_BUILD
        COMMAND ${CMAKE_COMMAND}
            -DDEST_DIR=$<TARGET_FILE_DIR:eta_http>
            -DDLLS=$<TARGET_RUNTIME_DLLS:eta_http>
            -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/CopyRuntimeDeps.cmake
        COMMAND_EXPAND_LISTS
        VERBATIM
    )
    foreach(extra_dll IN LISTS ETA_HTTP_EXTRA_RUNTIME_DLLS)
        add_custom_command(TARGET eta_http POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${extra_dll}"
                $<TARGET_FILE_DIR:eta_http>
            VERBATIM
        )
    endforeach()
endif()

if(ETA_HTTP_ENABLE_TESTS)
    enable_testing()

    add_executable(eta_http_unit_tests
        tests/unit/http_extension_tests.cpp
        src/eta/http/http_extension.cpp
        src/eta/http/http_multi.cpp
        src/eta/http/http_primitives.cpp
        src/eta/http/http_session.cpp
    )
    set_target_properties(eta_http_unit_tests PROPERTIES
        CXX_SCAN_FOR_MODULES OFF
    )
    target_compile_definitions(eta_http_unit_tests PRIVATE
        ETA_NATIVE_EXPORT_SYMBOLS=1
        NOMINMAX=1
        WIN32_LEAN_AND_MEAN=1
    )
    target_include_directories(eta_http_unit_tests PRIVATE
        ${ETA_CORE_SRC_DIR}
        ${Boost_INCLUDE_DIRS}
        ${CMAKE_CURRENT_SOURCE_DIR}/src
    )
    target_link_libraries(eta_http_unit_tests PRIVATE
        CURL::libcurl
        ${ETA_HTTP_CORE_TARGET}
    )
    if(WIN32)
        add_custom_command(TARGET eta_http_unit_tests POST_BUILD
            COMMAND ${CMAKE_COMMAND}
                -DDEST_DIR=$<TARGET_FILE_DIR:eta_http_unit_tests>
                -DDLLS=$<TARGET_RUNTIME_DLLS:eta_http_unit_tests>
                -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/CopyRuntimeDeps.cmake
            COMMAND_EXPAND_LISTS
            VERBATIM
        )
        foreach(extra_dll IN LISTS ETA_HTTP_EXTRA_RUNTIME_DLLS)
            add_custom_command(TARGET eta_http_unit_tests POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${extra_dll}"
                    $<TARGET_FILE_DIR:eta_http_unit_tests>
                VERBATIM
            )
        endforeach()
    endif()
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    add_test(
        NAME eta_http_unit_tests
        COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/loopback_server.py
            --run
            $<TARGET_FILE:eta_http_unit_tests>
    )

    if(ETA_HTTP_ENABLE_ETA_TESTS)
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
                set(ETA_HTTP_HOST_TRIPLE "aarch64-pc-windows-msvc")
            else()
                set(ETA_HTTP_HOST_TRIPLE "x86_64-pc-windows-msvc")
            endif()
        elseif(APPLE)
            if(CMAKE_SYSTEM_PROCESSOR MATCHES "arm64|aarch64")
                set(ETA_HTTP_HOST_TRIPLE "aarch64-apple-darwin")
            else()
                set(ETA_HTTP_HOST_TRIPLE "x86_64-apple-darwin")
            endif()
        elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64")
            set(ETA_HTTP_HOST_TRIPLE "aarch64-unknown-linux-gnu")
        else()
            set(ETA_HTTP_HOST_TRIPLE "x86_64-unknown-linux-gnu")
        endif()

        add_test(
            NAME eta_http_eta_smoke
            COMMAND ${CMAKE_COMMAND}
                -DPACKAGE_SOURCE_ROOT=${CMAKE_CURRENT_SOURCE_DIR}
                -DFIXTURE_ROOT=${CMAKE_CURRENT_BINARY_DIR}/eta_fixture
                -DSIDECAR_BINARY=$<TARGET_FILE:eta_http>
                -DETA_ETAI_EXECUTABLE=${ETA_ETAI_EXECUTABLE}
                -DETA_STDLIB_DIR=${ETA_STDLIB_DIR}
                -DHOST_TARGET_TRIPLE=${ETA_HTTP_HOST_TRIPLE}
                -DPYTHON_EXECUTABLE=${Python3_EXECUTABLE}
                -DLOOPBACK_SERVER_SCRIPT=${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/loopback_server.py
                -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/eta/run_http_eta_smoke.cmake
        )
    endif()
endif()
