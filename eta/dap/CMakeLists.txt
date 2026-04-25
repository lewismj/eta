cmake_minimum_required(VERSION 3.28)
project(eta_dap LANGUAGES CXX)

add_executable(eta_dap
    src/eta/dap/main_dap.cpp
    src/eta/dap/dap_io.cpp
    src/eta/dap/dap_trace.cpp
    src/eta/dap/dap_server.cpp
)

target_include_directories(eta_dap
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
    # Driver is header-only; make session headers visible to the DAP server.
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../session/src
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../interpreter/src
)

target_link_libraries(eta_dap PRIVATE eta_core eta_torch eta_nng eta_stats)
target_compile_definitions(eta_dap PRIVATE ETA_HAS_TORCH=1 ETA_HAS_NNG=1 ETA_HAS_STATS=1)
eta_copy_torch_dlls(eta_dap)
eta_copy_nng_dlls(eta_dap)

# Platform-specific libraries
if(NOT MSVC)
    target_link_libraries(eta_dap PRIVATE stdc++ m)

    # Check for PEXT support (mirrors core/interpreter setup)
    include(CheckCXXSourceCompiles)
    set(CMAKE_REQUIRED_FLAGS "-mbmi2")
    set(CMAKE_CXX_SCAN_FOR_MODULES_OLD ${CMAKE_CXX_SCAN_FOR_MODULES})
    set(CMAKE_CXX_SCAN_FOR_MODULES OFF)

    check_cxx_source_compiles("
        #include <immintrin.h>
        int main() {
            unsigned long long src = 0b10101010, mask = 0b1111;
            unsigned long long res = _pext_u64(src, mask);
            (void)res;
            return 0;
        }" PEXT_SUPPORTED_DAP)

    set(CMAKE_CXX_SCAN_FOR_MODULES ${CMAKE_CXX_SCAN_FOR_MODULES_OLD})

    if(PEXT_SUPPORTED_DAP)
        target_compile_definitions(eta_dap PRIVATE USE_PEXT)
        target_compile_options(eta_dap PRIVATE "-mbmi2")
        target_link_options(eta_dap PRIVATE "-mbmi2")
    endif()
endif()

install(TARGETS eta_dap RUNTIME DESTINATION bin)
