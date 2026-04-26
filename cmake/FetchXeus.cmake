include(FetchContent)

# CMake 4.x removes compatibility with projects declaring very old policy
# baselines. libzmq's pinned tag still advertises an old minimum, so pre-set
# the compatibility floor expected by CMake when evaluating third-party code.
if(NOT DEFINED CMAKE_POLICY_VERSION_MINIMUM)
    set(CMAKE_POLICY_VERSION_MINIMUM "3.5" CACHE STRING "" FORCE)
endif()
if(POLICY CMP0169)
    cmake_policy(SET CMP0169 OLD)
endif()

# Version pins for the xeus dependency stack.
set(ETA_XEUS_TAG          "5.1.1"  CACHE STRING "xeus version")
set(ETA_XEUS_ZMQ_TAG      "3.0.0"  CACHE STRING "xeus-zmq version")
set(ETA_LIBZMQ_TAG        "v4.3.5" CACHE STRING "libzmq version")
set(ETA_CPPZMQ_TAG        "v4.10.0" CACHE STRING "cppzmq version")
set(ETA_NLOHMANN_JSON_TAG "v3.11.3" CACHE STRING "nlohmann_json version")

# Prefer preinstalled packages when the full stack is available.
find_package(nlohmann_json CONFIG QUIET)
find_package(xeus CONFIG QUIET)
find_package(xeus-zmq CONFIG QUIET)
if(TARGET nlohmann_json::nlohmann_json AND TARGET xeus AND TARGET xeus-zmq)
    message(STATUS "Using preinstalled xeus dependency stack")
    return()
endif()

# Third-party tests/examples are not required for Eta builds.
set(ZMQ_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(BUILD_STATIC OFF CACHE BOOL "" FORCE)
set(BUILD_SHARED ON CACHE BOOL "" FORCE)
# libzmq's MSVC precompiled-header flags (/Yu precompiled.hpp) break IDE
# compiler-probing that compiles temp source files outside libzmq's src dir.
set(ENABLE_PRECOMPILED OFF CACHE BOOL "" FORCE)
set(ENABLE_CURVE    OFF CACHE BOOL "" FORCE)
set(WITH_DOCS       OFF CACHE BOOL "" FORCE)
set(CPPZMQ_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(XEUS_BUILD_TESTS   OFF CACHE BOOL "" FORCE)
set(XEUS_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(XEUS_BUILD_STATIC_LIBS OFF CACHE BOOL "" FORCE)
set(XEUS_ZMQ_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(XEUS_ZMQ_BUILD_STATIC_LIBS OFF CACHE BOOL "" FORCE)

FetchContent_Declare(
    libzmq
    GIT_REPOSITORY https://github.com/zeromq/libzmq.git
    GIT_TAG        ${ETA_LIBZMQ_TAG}
    EXCLUDE_FROM_ALL
    SYSTEM
)

FetchContent_Declare(
    cppzmq
    GIT_REPOSITORY https://github.com/zeromq/cppzmq.git
    GIT_TAG        ${ETA_CPPZMQ_TAG}
    EXCLUDE_FROM_ALL
    SYSTEM
)

# Prefer an installed nlohmann_json package when available so xeus/xeus-zmq
# export() validation does not fail on non-exported in-tree json targets.
if(WIN32)
    set(_eta_vcpkg_root "C:/Users/lewis/develop/vcpkg/installed/x64-windows")
    if(EXISTS "${_eta_vcpkg_root}/share/nlohmann_json/nlohmann_jsonConfig.cmake")
        find_package(nlohmann_json CONFIG QUIET PATHS "${_eta_vcpkg_root}" NO_DEFAULT_PATH)
    endif()
    unset(_eta_vcpkg_root)
endif()
if(NOT TARGET nlohmann_json::nlohmann_json)
    find_package(nlohmann_json CONFIG QUIET)
endif()
if(NOT TARGET nlohmann_json::nlohmann_json)
    FetchContent_Declare(
        nlohmann_json
        GIT_REPOSITORY https://github.com/nlohmann/json.git
        GIT_TAG        ${ETA_NLOHMANN_JSON_TAG}
        EXCLUDE_FROM_ALL
        SYSTEM
    )

    # Keep json header-only and imported to avoid CMake-4 export-set errors in
    # xeus/xeus-zmq when the dependency is not provided by a package manager.
    FetchContent_GetProperties(nlohmann_json)
    if(NOT nlohmann_json_POPULATED)
        FetchContent_Populate(nlohmann_json)
    endif()

    add_library(nlohmann_json INTERFACE IMPORTED GLOBAL)
    set_target_properties(nlohmann_json PROPERTIES
        INTERFACE_INCLUDE_DIRECTORIES "${nlohmann_json_SOURCE_DIR}/include"
    )
    add_library(nlohmann_json::nlohmann_json ALIAS nlohmann_json)
endif()

FetchContent_Declare(
    xeus
    GIT_REPOSITORY https://github.com/jupyter-xeus/xeus.git
    GIT_TAG        ${ETA_XEUS_TAG}
    EXCLUDE_FROM_ALL
    SYSTEM
)

FetchContent_Declare(
    xeus_zmq
    GIT_REPOSITORY https://github.com/jupyter-xeus/xeus-zmq.git
    GIT_TAG        ${ETA_XEUS_ZMQ_TAG}
    EXCLUDE_FROM_ALL
    SYSTEM
)

# On Windows prefer shared libzmq to avoid import/export mismatches.
if(WIN32)
    if(DEFINED BUILD_SHARED_LIBS)
        set(_eta_had_BUILD_SHARED_LIBS TRUE)
        set(_eta_prev_BUILD_SHARED_LIBS "${BUILD_SHARED_LIBS}")
    else()
        set(_eta_had_BUILD_SHARED_LIBS FALSE)
    endif()
    set(BUILD_SHARED_LIBS ON)
endif()

# xeus / xeus-zmq install-export metadata is not needed when consumed as
# in-tree FetchContent dependencies. Temporarily disable install rule
# generation to avoid export-set validation failures against third-party
# targets (notably nlohmann_json on CMake 4.x).
if(DEFINED CMAKE_SKIP_INSTALL_RULES)
    set(_eta_had_skip_install_rules TRUE)
    set(_eta_prev_skip_install_rules "${CMAKE_SKIP_INSTALL_RULES}")
else()
    set(_eta_had_skip_install_rules FALSE)
endif()
set(CMAKE_SKIP_INSTALL_RULES ON)

function(_eta_prepare_linux_openssl_headers out_include_dir)
    set(_eta_openssl_version "3.3.0")
    set(_eta_openssl_root "${CMAKE_BINARY_DIR}/_deps/eta_openssl")
    set(_eta_openssl_archive "${_eta_openssl_root}/openssl-${_eta_openssl_version}.tar.gz")
    set(_eta_openssl_src "${_eta_openssl_root}/openssl-${_eta_openssl_version}")

    if(NOT EXISTS "${_eta_openssl_src}/include/openssl/opensslv.h")
        file(MAKE_DIRECTORY "${_eta_openssl_root}")

        if(NOT EXISTS "${_eta_openssl_archive}")
            message(STATUS "OpenSSL headers not found: downloading openssl-${_eta_openssl_version}.tar.gz")
            file(DOWNLOAD
                "https://www.openssl.org/source/openssl-${_eta_openssl_version}.tar.gz"
                "${_eta_openssl_archive}"
                SHOW_PROGRESS
                STATUS _eta_openssl_download_status
                TLS_VERIFY ON
            )
            list(GET _eta_openssl_download_status 0 _eta_openssl_download_code)
            list(GET _eta_openssl_download_status 1 _eta_openssl_download_msg)
            if(NOT _eta_openssl_download_code EQUAL 0)
                message(FATAL_ERROR
                    "Failed to download OpenSSL source archive:\n"
                    "  URL: https://www.openssl.org/source/openssl-${_eta_openssl_version}.tar.gz\n"
                    "  Error: ${_eta_openssl_download_msg}")
            endif()
        endif()

        if(EXISTS "${_eta_openssl_src}")
            file(REMOVE_RECURSE "${_eta_openssl_src}")
        endif()

        execute_process(
            COMMAND "${CMAKE_COMMAND}" -E tar xzf "${_eta_openssl_archive}"
            WORKING_DIRECTORY "${_eta_openssl_root}"
            RESULT_VARIABLE _eta_openssl_extract_result
        )
        if(NOT _eta_openssl_extract_result EQUAL 0)
            message(FATAL_ERROR
                "Failed to extract OpenSSL source archive: ${_eta_openssl_archive}")
        endif()

        find_program(_eta_openssl_perl_exe NAMES perl REQUIRED)
        find_program(_eta_openssl_make_exe NAMES make gmake REQUIRED)

        if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|amd64|AMD64)$")
            set(_eta_openssl_target "linux-x86_64")
        elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64)$")
            set(_eta_openssl_target "linux-aarch64")
        else()
            set(_eta_openssl_target "linux-generic64")
        endif()

        execute_process(
            COMMAND "${_eta_openssl_perl_exe}" Configure "${_eta_openssl_target}" no-tests
            WORKING_DIRECTORY "${_eta_openssl_src}"
            RESULT_VARIABLE _eta_openssl_configure_result
        )
        if(NOT _eta_openssl_configure_result EQUAL 0)
            message(FATAL_ERROR
                "Failed to configure local OpenSSL headers in ${_eta_openssl_src}")
        endif()

        execute_process(
            COMMAND "${_eta_openssl_make_exe}" build_generated
            WORKING_DIRECTORY "${_eta_openssl_src}"
            RESULT_VARIABLE _eta_openssl_make_result
        )
        if(NOT _eta_openssl_make_result EQUAL 0)
            message(FATAL_ERROR
                "Failed to generate OpenSSL headers in ${_eta_openssl_src}")
        endif()

        unset(_eta_openssl_perl_exe)
        unset(_eta_openssl_make_exe)
        unset(_eta_openssl_target)
        unset(_eta_openssl_download_status)
        unset(_eta_openssl_download_code)
        unset(_eta_openssl_download_msg)
        unset(_eta_openssl_extract_result)
        unset(_eta_openssl_configure_result)
        unset(_eta_openssl_make_result)
    endif()

    set(${out_include_dir} "${_eta_openssl_src}/include" PARENT_SCOPE)
    unset(_eta_openssl_version)
    unset(_eta_openssl_root)
    unset(_eta_openssl_archive)
    unset(_eta_openssl_src)
endfunction()

# xeus-zmq requires OpenSSL::Crypto. Try system OpenSSL first and then
# bootstrap headers on Linux/WSL when only the runtime library is present.
find_package(OpenSSL QUIET)
if(NOT OpenSSL_FOUND)
    if(WIN32 AND MSVC)
        if(NOT DEFINED OPENSSL_ROOT_DIR)
            set(_eta_vcpkg_openssl_root "C:/Users/lewis/develop/vcpkg/installed/x64-windows")
            if(EXISTS "${_eta_vcpkg_openssl_root}/share/openssl/OpenSSLConfig.cmake")
                set(OPENSSL_ROOT_DIR "${_eta_vcpkg_openssl_root}" CACHE PATH "OpenSSL root directory" FORCE)
            endif()
            unset(_eta_vcpkg_openssl_root)
        endif()
    elseif(UNIX AND NOT APPLE)
        _eta_prepare_linux_openssl_headers(_eta_openssl_include_dir)

        find_library(_eta_openssl_crypto_library
            NAMES crypto libcrypto.so libcrypto.so.3
        )
        if(NOT _eta_openssl_crypto_library)
            message(FATAL_ERROR
                "OpenSSL runtime library (libcrypto) not found. "
                "Install OpenSSL development/runtime packages for this toolchain.")
        endif()

        set(OPENSSL_INCLUDE_DIR "${_eta_openssl_include_dir}" CACHE PATH
            "OpenSSL include directory" FORCE)
        set(OPENSSL_CRYPTO_LIBRARY "${_eta_openssl_crypto_library}" CACHE FILEPATH
            "OpenSSL crypto library" FORCE)

        find_library(_eta_openssl_ssl_library
            NAMES ssl libssl.so libssl.so.3
        )
        if(_eta_openssl_ssl_library)
            set(OPENSSL_SSL_LIBRARY "${_eta_openssl_ssl_library}" CACHE FILEPATH
                "OpenSSL ssl library" FORCE)
        endif()

        unset(_eta_openssl_include_dir)
        unset(_eta_openssl_crypto_library)
        unset(_eta_openssl_ssl_library)
    endif()

    find_package(OpenSSL REQUIRED)
endif()

# Order matters: libzmq -> cppzmq -> json -> xeus -> xeus-zmq.
FetchContent_MakeAvailable(libzmq cppzmq xeus xeus_zmq)

if(_eta_had_skip_install_rules)
    set(CMAKE_SKIP_INSTALL_RULES "${_eta_prev_skip_install_rules}")
else()
    unset(CMAKE_SKIP_INSTALL_RULES)
endif()
unset(_eta_had_skip_install_rules)
unset(_eta_prev_skip_install_rules)

if(WIN32)
    if(_eta_had_BUILD_SHARED_LIBS)
        set(BUILD_SHARED_LIBS "${_eta_prev_BUILD_SHARED_LIBS}")
    else()
        unset(BUILD_SHARED_LIBS)
    endif()
    unset(_eta_had_BUILD_SHARED_LIBS)
    unset(_eta_prev_BUILD_SHARED_LIBS)
endif()

if(NOT TARGET xeus)
    message(FATAL_ERROR "xeus fetch failed: xeus target missing")
endif()
if(NOT TARGET xeus-zmq)
    message(FATAL_ERROR "xeus-zmq fetch failed: xeus-zmq target missing")
endif()
if(NOT TARGET nlohmann_json::nlohmann_json)
    message(FATAL_ERROR "nlohmann_json fetch failed: nlohmann_json::nlohmann_json target missing")
endif()

message(STATUS "xeus dependency stack fetched")
