cmake_minimum_required(VERSION 3.28)
project(eta_bench LANGUAGES CXX)

add_executable(eta_qp_bench
    src/qp_benchmark.cpp
)

set_target_properties(eta_qp_bench PROPERTIES CXX_SCAN_FOR_MODULES OFF)

target_link_libraries(eta_qp_bench PRIVATE eta_core)

if(NOT MSVC)
    target_link_libraries(eta_qp_bench PRIVATE stdc++ m)
endif()

install(TARGETS eta_qp_bench RUNTIME DESTINATION bin)
