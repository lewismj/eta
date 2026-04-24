cmake_minimum_required(VERSION 3.28)

project(eta LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
# The project does not use C++20 modules; disable dependency scanning globally.
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)

enable_testing()

add_subdirectory(eta)

# Install stdlib alongside binaries.
# Layout: <prefix>/bin/* and <prefix>/stdlib/*.eta
install(DIRECTORY stdlib/ DESTINATION stdlib
    FILES_MATCHING PATTERN "*.eta")

add_custom_target(eta_all)
foreach(_t IN ITEMS
    eta_core
    eta_torch
    eta_nng
    eta_stats
    etai
    eta_repl
    etac
    eta_lsp
    eta_dap
    eta_core_test
    eta_test
    eta_test_runner
    eta_qp_bench
)
    if(TARGET ${_t})
        add_dependencies(eta_all ${_t})
    endif()
endforeach()
