cmake_minimum_required(VERSION 3.28)

project(eta VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
# The project does not use C++20 modules; disable dependency scanning globally.
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)

# Root-anchored source paths shared by eta subdirectories.
set(ETA_REPO_ROOT_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
set(ETA_CMAKE_DIR "${ETA_REPO_ROOT_DIR}/cmake")
set(ETA_SOURCE_DIR "${ETA_REPO_ROOT_DIR}/eta")
set(ETA_STDLIB_SOURCE_DIR "${ETA_REPO_ROOT_DIR}/stdlib")
set(ETA_COOKBOOK_SOURCE_DIR "${ETA_REPO_ROOT_DIR}/cookbook")

# GitHub Actions release jobs package from `cmake --install` directly, so
# ensure the MSVC runtime DLLs are installed into bin/ for portable bundles.
if(WIN32)
    set(CMAKE_INSTALL_SYSTEM_RUNTIME_DESTINATION "bin")
    include(InstallRequiredSystemLibraries)
endif()

enable_testing()

include(cmake/FetchXeus.cmake)

add_subdirectory(eta)

# VS Code extension targets self-disable when Node/npm are unavailable.
add_subdirectory(editors/vscode)

set(ETA_LIGHTGBM_PACKAGE_DIR "${ETA_REPO_ROOT_DIR}/packages/ml/native/lightgbm")
set(ETA_DUCKDB_PACKAGE_DIR "${ETA_REPO_ROOT_DIR}/packages/db/native/duckdb")

option(ETA_ENABLE_PACKAGE_NATIVE_SIDECARS
    "Build and stage package-managed native sidecars in the top-level build."
    ON)

if(ETA_ENABLE_PACKAGE_NATIVE_SIDECARS)
    set(ETA_LIGHTGBM_FETCH_UPSTREAM ON CACHE BOOL
        "Fetch pinned LightGBM upstream sources for package development." FORCE)
    set(ETA_LIGHTGBM_ENABLE_TESTS OFF CACHE BOOL
        "Build package-local tests." FORCE)
    set(ETA_DUCKDB_ENABLE_TESTS OFF CACHE BOOL
        "Build package-local tests." FORCE)

    add_subdirectory("${ETA_LIGHTGBM_PACKAGE_DIR}" "packages/ml/native/lightgbm")
    add_subdirectory("${ETA_DUCKDB_PACKAGE_DIR}" "packages/db/native/duckdb")

    if(WIN32)
        if(CMAKE_SYSTEM_PROCESSOR MATCHES "^[Aa][Rr][Mm]64|aarch64")
            set(ETA_PACKAGE_HOST_TRIPLE "aarch64-pc-windows-msvc")
        else()
            set(ETA_PACKAGE_HOST_TRIPLE "x86_64-pc-windows-msvc")
        endif()
    elseif(APPLE)
        if(CMAKE_SYSTEM_PROCESSOR MATCHES "arm64|aarch64")
            set(ETA_PACKAGE_HOST_TRIPLE "aarch64-apple-darwin")
        else()
            set(ETA_PACKAGE_HOST_TRIPLE "x86_64-apple-darwin")
        endif()
    elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64")
        set(ETA_PACKAGE_HOST_TRIPLE "aarch64-unknown-linux-gnu")
    else()
        set(ETA_PACKAGE_HOST_TRIPLE "x86_64-unknown-linux-gnu")
    endif()

    set(ETA_PACKAGE_STAGE_SUPPORTED_TRIPLE FALSE)
    if(ETA_PACKAGE_HOST_TRIPLE STREQUAL "x86_64-pc-windows-msvc"
       OR ETA_PACKAGE_HOST_TRIPLE STREQUAL "x86_64-unknown-linux-gnu"
       OR ETA_PACKAGE_HOST_TRIPLE STREQUAL "x86_64-apple-darwin"
       OR ETA_PACKAGE_HOST_TRIPLE STREQUAL "aarch64-apple-darwin")
        set(ETA_PACKAGE_STAGE_SUPPORTED_TRIPLE TRUE)
    endif()

    if(ETA_PACKAGE_STAGE_SUPPORTED_TRIPLE)
        add_custom_target(eta_stage_native_packages
            COMMAND "${CMAKE_COMMAND}"
                -DPACKAGE_ROOT=${ETA_LIGHTGBM_PACKAGE_DIR}
                -DSIDECAR_BINARY=$<TARGET_FILE:eta_lightgbm>
                -DHOST_TARGET_TRIPLE=${ETA_PACKAGE_HOST_TRIPLE}
                -P "${ETA_LIGHTGBM_PACKAGE_DIR}/cmake/StageLightGBMSidecar.cmake"
            COMMAND "${CMAKE_COMMAND}"
                -DPACKAGE_ROOT=${ETA_DUCKDB_PACKAGE_DIR}
                -DSIDECAR_BINARY=$<TARGET_FILE:eta_duckdb>
                -DHOST_TARGET_TRIPLE=${ETA_PACKAGE_HOST_TRIPLE}
                -P "${ETA_DUCKDB_PACKAGE_DIR}/cmake/StageDuckDBSidecar.cmake"
            DEPENDS eta_lightgbm eta_duckdb
            COMMENT "Stage package sidecars and refresh host sha256 entries"
            VERBATIM
        )
    else()
        message(STATUS
            "Skipping package sidecar staging: unsupported host triple ${ETA_PACKAGE_HOST_TRIPLE}")
    endif()
endif()

# Install stdlib alongside binaries.
install(DIRECTORY stdlib/ DESTINATION stdlib
    FILES_MATCHING PATTERN "*.eta")
install(DIRECTORY "${CMAKE_BINARY_DIR}/stdlib/" DESTINATION stdlib
    OPTIONAL
    FILES_MATCHING PATTERN "*.etac"
)

# Install cookbook programs.
# Include source examples (*.eta), Jupyter notebooks (*.ipynb) and the
# accompanying READMEs/markdown so the release bundle ships a complete
# cookbook tree (the notebooks under cookbook/notebooks/ in particular
# are referenced from README.md / TLDR.md / docs/*).
install(DIRECTORY cookbook/ DESTINATION cookbook
    FILES_MATCHING
        PATTERN "*.eta"
        PATTERN "*.ipynb"
        PATTERN "*.md")

# Install package-managed sidecars and Eta wrapper modules.
install(FILES
    "${ETA_LIGHTGBM_PACKAGE_DIR}/eta.toml"
    "${ETA_LIGHTGBM_PACKAGE_DIR}/README.md"
    DESTINATION "packages/ml/native/lightgbm")
install(DIRECTORY "${ETA_LIGHTGBM_PACKAGE_DIR}/src/"
    DESTINATION "packages/ml/native/lightgbm/src"
    FILES_MATCHING PATTERN "*.eta")
install(DIRECTORY "${ETA_LIGHTGBM_PACKAGE_DIR}/libs/"
    DESTINATION "packages/ml/native/lightgbm/libs"
    OPTIONAL
    FILES_MATCHING
        PATTERN "*.dll"
        PATTERN "*.so"
        PATTERN "*.dylib")

install(FILES
    "${ETA_DUCKDB_PACKAGE_DIR}/eta.toml"
    "${ETA_DUCKDB_PACKAGE_DIR}/README.md"
    DESTINATION "packages/db/native/duckdb")
install(DIRECTORY "${ETA_DUCKDB_PACKAGE_DIR}/src/"
    DESTINATION "packages/db/native/duckdb/src"
    FILES_MATCHING PATTERN "*.eta")
install(DIRECTORY "${ETA_DUCKDB_PACKAGE_DIR}/libs/"
    DESTINATION "packages/db/native/duckdb/libs"
    OPTIONAL
    FILES_MATCHING
        PATTERN "*.dll"
        PATTERN "*.so"
        PATTERN "*.dylib")

add_custom_target(eta_all)
foreach(_t IN ITEMS
    eta_core
    eta
    etai
    eta_repl
    etac
    eta_lsp
    eta_dap
    eta_jupyter
    eta_prof
    eta_stdlib_doc_registry
    eta_stdlib_etac
    eta_core_test
    eta_pkg_test
    eta_cli_test
    eta_test
    eta_test_runner
    eta_qp_bench
    eta_prof_off_bench
    eta_stage_native_packages
)
    if(TARGET ${_t})
        add_dependencies(eta_all ${_t})
    endif()
endforeach()
