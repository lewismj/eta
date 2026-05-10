include(FetchContent)

set(ETA_LIGHTGBM_UPSTREAM_TAG "v4.6.0" CACHE STRING
    "Pinned LightGBM upstream tag used by the LightGBM sidecar scaffold.")
option(ETA_LIGHTGBM_ENABLE_OPENMP
    "Enable OpenMP acceleration when building fetched LightGBM sources."
    ON)

function(eta_lightgbm_fetch)
    set(BUILD_CLI OFF CACHE BOOL "Disable LightGBM CLI binary for sidecar builds." FORCE)
    set(BUILD_CPP_TEST OFF CACHE BOOL "Disable LightGBM upstream C++ tests." FORCE)
    set(BUILD_STATIC_LIB ON CACHE BOOL "Build LightGBM as static library for sidecar linkage." FORCE)
    set(INSTALL_HEADERS OFF CACHE BOOL "Disable LightGBM header install step in sidecar builds." FORCE)

    if(ETA_LIGHTGBM_ENABLE_OPENMP)
        find_package(OpenMP QUIET)
        if(OpenMP_C_FOUND AND OpenMP_CXX_FOUND)
            set(USE_OPENMP ON CACHE BOOL "Enable OpenMP" FORCE)
        else()
            message(STATUS
                "OpenMP toolchain not fully available (OpenMP_C_FOUND=${OpenMP_C_FOUND}, "
                "OpenMP_CXX_FOUND=${OpenMP_CXX_FOUND}); building LightGBM without OpenMP.")
            set(USE_OPENMP OFF CACHE BOOL "Enable OpenMP" FORCE)
        endif()
    else()
        set(USE_OPENMP OFF CACHE BOOL "Enable OpenMP" FORCE)
    endif()

    FetchContent_Declare(
        lightgbm
        GIT_REPOSITORY https://github.com/microsoft/LightGBM.git
        GIT_TAG        ${ETA_LIGHTGBM_UPSTREAM_TAG}
        GIT_SHALLOW    ON
    )

    # Upstream LightGBM hard-codes install destinations using
    # `${CMAKE_INSTALL_PREFIX}` in install() DESTINATION paths, so `cmake --install --prefix`
    # cannot relocate those entries. Route those upstream install destinations into
    # the build tree to avoid polluting system prefixes in Eta super-build installs.
    set(_eta_lightgbm_install_prefix "${CMAKE_INSTALL_PREFIX}")
    set(CMAKE_INSTALL_PREFIX "${CMAKE_BINARY_DIR}/_deps/lightgbm-install" CACHE PATH
        "Install path prefix, prepended onto install directories."
        FORCE)
    FetchContent_MakeAvailable(lightgbm)
    set(CMAKE_INSTALL_PREFIX "${_eta_lightgbm_install_prefix}" CACHE PATH
        "Install path prefix, prepended onto install directories."
        FORCE)

    # Eta builds a shared sidecar module and links it against the fetched static
    # LightGBM archive, so ensure the upstream object/static targets use PIC.
    if(TARGET lightgbm_objs)
        set_target_properties(lightgbm_objs PROPERTIES POSITION_INDEPENDENT_CODE ON)
    endif()
    if(TARGET lightgbm_capi_objs)
        set_target_properties(lightgbm_capi_objs PROPERTIES POSITION_INDEPENDENT_CODE ON)
    endif()
    if(TARGET _lightgbm)
        set_target_properties(_lightgbm PROPERTIES POSITION_INDEPENDENT_CODE ON)
    endif()

    set(ETA_LIGHTGBM_SOURCE_DIR "${lightgbm_SOURCE_DIR}" PARENT_SCOPE)
    set(ETA_LIGHTGBM_BINARY_DIR "${lightgbm_BINARY_DIR}" PARENT_SCOPE)
endfunction()
