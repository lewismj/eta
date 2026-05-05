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

add_custom_target(eta_all)
foreach(_t IN ITEMS
    eta_core
    eta_torch
    eta_nng
    eta_stats
    eta_log
    eta
    etai
    eta_repl
    etac
    eta_lsp
    eta_dap
    eta_jupyter
    eta_stdlib_etac
    eta_core_test
    eta_pkg_test
    eta_cli_test
    eta_test
    eta_test_runner
    eta_qp_bench
)
    if(TARGET ${_t})
        add_dependencies(eta_all ${_t})
    endif()
endforeach()
