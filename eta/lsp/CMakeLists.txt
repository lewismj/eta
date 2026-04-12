cmake_minimum_required(VERSION 3.28)
project(eta_lsp LANGUAGES CXX)

add_executable(eta_lsp
    src/eta/lsp/main_lsp.cpp
    src/eta/lsp/lsp_server.cpp
)

target_include_directories(eta_lsp
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(eta_lsp PRIVATE eta_core)

# Torch integration (when available — provides completion/hover for torch builtins)
if(ETA_BUILD_TORCH)
    target_link_libraries(eta_lsp PRIVATE eta_torch)
    target_compile_definitions(eta_lsp PRIVATE ETA_HAS_TORCH=1)
    eta_copy_torch_dlls(eta_lsp)
endif()

# nng integration (when available — provides completion/hover for nng builtins)
if(ETA_BUILD_NNG)
    target_link_libraries(eta_lsp PRIVATE eta_nng)
    target_compile_definitions(eta_lsp PRIVATE ETA_HAS_NNG=1)
    eta_copy_nng_dlls(eta_lsp)
endif()

# Platform-specific libraries
if(NOT MSVC)
    target_link_libraries(eta_lsp PRIVATE stdc++ m)

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
        }" PEXT_SUPPORTED_LSP)

    set(CMAKE_CXX_SCAN_FOR_MODULES ${CMAKE_CXX_SCAN_FOR_MODULES_OLD})

    if(PEXT_SUPPORTED_LSP)
        target_compile_definitions(eta_lsp PRIVATE USE_PEXT)
        target_compile_options(eta_lsp PRIVATE "-mbmi2")
        target_link_options(eta_lsp PRIVATE "-mbmi2")
    endif()
endif()

install(TARGETS eta_lsp RUNTIME DESTINATION bin)

