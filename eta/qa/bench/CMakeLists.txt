cmake_minimum_required(VERSION 3.28)
project(eta_bench LANGUAGES CXX)

add_executable(eta_qp_bench
    src/qp_benchmark.cpp
)

add_executable(eta_prof_off_bench
    prof/profiler_off_benchmark.cpp
)

set_target_properties(eta_qp_bench PROPERTIES CXX_SCAN_FOR_MODULES OFF)
set_target_properties(eta_prof_off_bench PROPERTIES CXX_SCAN_FOR_MODULES OFF)

target_link_libraries(eta_qp_bench PRIVATE eta_core)
target_link_libraries(eta_prof_off_bench PRIVATE eta_core)
if(TARGET nng)
    target_link_libraries(eta_qp_bench PRIVATE nng)
    target_link_libraries(eta_prof_off_bench PRIVATE nng)
endif()

if(NOT MSVC)
    target_link_libraries(eta_qp_bench PRIVATE stdc++ m)
    target_link_libraries(eta_prof_off_bench PRIVATE stdc++ m)
endif()

install(TARGETS eta_qp_bench RUNTIME DESTINATION bin)
install(TARGETS eta_prof_off_bench RUNTIME DESTINATION bin)
