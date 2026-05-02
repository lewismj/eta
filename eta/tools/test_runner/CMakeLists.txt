cmake_minimum_required(VERSION 3.28)
project(eta_test_runner LANGUAGES CXX)

add_executable(eta_test
    src/main_test_runner.cpp
)

set_target_properties(eta_test PROPERTIES CXX_SCAN_FOR_MODULES OFF)

target_include_directories(eta_test PRIVATE
    ${ETA_INTERPRETER_SRC_DIR}
    ${ETA_SESSION_SRC_DIR}
)

target_link_libraries(eta_test PRIVATE eta_core eta_torch eta_nng eta_stats eta_log)
target_compile_definitions(eta_test PRIVATE
    ETA_HAS_TORCH=1 ETA_HAS_NNG=1 ETA_HAS_STATS=1
    ETA_STDLIB_DIR="${ETA_STDLIB_SOURCE_DIR}"
)

eta_copy_torch_dlls(eta_test)
eta_copy_nng_dlls(eta_test)
eta_copy_log_dlls(eta_test)

if(NOT MSVC)
    target_link_libraries(eta_test PRIVATE stdc++ m)
    target_compile_options(eta_test PRIVATE "-msse2")

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
        }" PEXT_SUPPORTED_TR)

    set(CMAKE_CXX_SCAN_FOR_MODULES ${CMAKE_CXX_SCAN_FOR_MODULES_OLD})

    if(PEXT_SUPPORTED_TR)
        target_compile_definitions(eta_test PRIVATE USE_PEXT)
        target_compile_options(eta_test PRIVATE "-mbmi2")
        target_link_options(eta_test PRIVATE "-mbmi2")
    endif()
endif()

# Register stdlib tests as a CTest entry
enable_testing()
add_test(
    NAME eta_stdlib_tests
    COMMAND eta_test
        --path "${ETA_STDLIB_SOURCE_DIR}"
        "${ETA_STDLIB_SOURCE_DIR}/tests"
)

install(TARGETS eta_test RUNTIME DESTINATION bin)

