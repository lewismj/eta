cmake_minimum_required(VERSION 3.28)
project(eta_interpreter LANGUAGES CXX)

# ── etai — file interpreter executable ────────────────────────────────
add_executable(etai
    src/eta/interpreter/main_etai.cpp
)

target_include_directories(etai
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(etai PRIVATE eta_core)

# Torch integration (when available)
if(ETA_BUILD_TORCH)
    target_link_libraries(etai PRIVATE eta_torch)
    target_compile_definitions(etai PRIVATE ETA_HAS_TORCH=1)
endif()

# Platform-specific libraries
if(NOT MSVC)
    target_link_libraries(etai PRIVATE stdc++ m)

    # Check for PEXT support (mirrors core/test setup)
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
        }" PEXT_SUPPORTED_INTERP)

    set(CMAKE_CXX_SCAN_FOR_MODULES ${CMAKE_CXX_SCAN_FOR_MODULES_OLD})

    if(PEXT_SUPPORTED_INTERP)
        target_compile_definitions(etai PRIVATE USE_PEXT)
        target_compile_options(etai PRIVATE "-mbmi2")
        target_link_options(etai PRIVATE "-mbmi2")
    endif()
endif()

# ── eta-repl — interactive REPL executable ────────────────────────────
add_executable(eta_repl
    src/eta/interpreter/main_repl.cpp
)

target_include_directories(eta_repl
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(eta_repl PRIVATE eta_core)

# Torch integration (when available)
if(ETA_BUILD_TORCH)
    target_link_libraries(eta_repl PRIVATE eta_torch)
    target_compile_definitions(eta_repl PRIVATE ETA_HAS_TORCH=1)
endif()

# Platform-specific libraries
if(NOT MSVC)
    target_link_libraries(eta_repl PRIVATE stdc++ m)

    if(PEXT_SUPPORTED_INTERP)
        target_compile_definitions(eta_repl PRIVATE USE_PEXT)
        target_compile_options(eta_repl PRIVATE "-mbmi2")
        target_link_options(eta_repl PRIVATE "-mbmi2")
    endif()
endif()

# ── Install ───────────────────────────────────────────────────────────
install(TARGETS etai eta_repl RUNTIME DESTINATION bin)

