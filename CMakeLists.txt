cmake_minimum_required(VERSION 3.28)

project(eta VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
# The project does not use C++20 modules; disable dependency scanning globally.
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)

enable_testing()

include(cmake/FetchXeus.cmake)

add_subdirectory(eta)

# VS Code extension targets self-disable when Node/npm are unavailable.
add_subdirectory(editors/vscode)

# Install stdlib alongside binaries.
install(DIRECTORY stdlib/ DESTINATION stdlib
    FILES_MATCHING PATTERN "*.eta")

# Install example programs.
install(DIRECTORY examples/ DESTINATION examples
    FILES_MATCHING PATTERN "*.eta")

add_custom_target(eta_all)
foreach(_t IN ITEMS
    eta_core
    eta_torch
    eta_nng
    eta_stats
    eta_log
    etai
    eta_repl
    etac
    eta_lsp
    eta_dap
    eta_jupyter
    eta_core_test
    eta_test
    eta_test_runner
    eta_qp_bench
)
    if(TARGET ${_t})
        add_dependencies(eta_all ${_t})
    endif()
endforeach()
