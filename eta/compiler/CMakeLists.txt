cmake_minimum_required(VERSION 3.28)
project(eta_compiler LANGUAGES CXX)

# ── etac — ahead-of-time bytecode compiler ─────────────────────────
add_executable(etac
    src/eta/compiler/main_etac.cpp
)

target_include_directories(etac
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../interpreter/src
)

target_link_libraries(etac PRIVATE eta_core eta_torch eta_nng eta_stats)
target_compile_definitions(etac PRIVATE ETA_HAS_TORCH=1 ETA_HAS_NNG=1 ETA_HAS_STATS=1)
eta_copy_torch_dlls(etac)
eta_copy_nng_dlls(etac)

# Platform-specific libraries
if(NOT MSVC)
    target_link_libraries(etac PRIVATE stdc++ m)

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
        }" PEXT_SUPPORTED_COMPILER)

    set(CMAKE_CXX_SCAN_FOR_MODULES ${CMAKE_CXX_SCAN_FOR_MODULES_OLD})

    if(PEXT_SUPPORTED_COMPILER)
        target_compile_definitions(etac PRIVATE USE_PEXT)
        target_compile_options(etac PRIVATE "-mbmi2")
        target_link_options(etac PRIVATE "-mbmi2")
    endif()
endif()

# ── Install ───────────────────────────────────────────────────────────
install(TARGETS etac RUNTIME DESTINATION bin)
