project(eta_core_test LANGUAGES CXX)

set(SOURCES
        src/parser_tests.cpp
        src/lexer_tests.cpp
        src/heap_tests.cpp
        src/factory_tests.cpp
        src/gc_tests.cpp
        src/hazard_ptr_tests.cpp
        src/intern_table_tests.cpp
        src/nanbox_tests.cpp
        src/module_linker_tests.cpp
        src/eta_test.cpp)

add_executable(${PROJECT_NAME} ${SOURCES})

# Base linking - only eta_core unconditionally
target_link_libraries(${PROJECT_NAME} PRIVATE eta_core)
target_link_libraries(${PROJECT_NAME} PRIVATE Boost::unit_test_framework)


# Platform-specific libraries
if(NOT MSVC)
    target_link_libraries(${PROJECT_NAME} PRIVATE stdc++ m)

    # Add SSE2 flags - required by Boost concurrent containers
    target_compile_options(${PROJECT_NAME} PRIVATE "-msse2")

    # Check for PEXT support
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
        }" PEXT_SUPPORTED)

    set(CMAKE_CXX_SCAN_FOR_MODULES ${CMAKE_CXX_SCAN_FOR_MODULES_OLD})

    if(PEXT_SUPPORTED)
        message(STATUS "PEXT instruction is supported.")
        target_compile_definitions(${PROJECT_NAME} PRIVATE USE_PEXT)
        target_compile_options(${PROJECT_NAME} PRIVATE "-mbmi2")
        target_link_options(${PROJECT_NAME} PRIVATE "-mbmi2")
    else()
        message(STATUS "PEXT instruction is not supported.")
    endif()
endif()


if(WIN32)
    if(NOT DEFINED BOOST_ROOT)
        if(DEFINED ENV{BOOST_ROOT})
            set(BOOST_ROOT "$ENV{BOOST_ROOT}")
        else()
            message(FATAL_ERROR "BOOST_ROOT not set. Please specify BOOST_ROOT in CMake or in your environment.")
        endif()
    endif()

    if(CMAKE_BUILD_TYPE STREQUAL "Debug")
        set(BOOST_DLL_PATH "${BOOST_ROOT}/debug/bin")
    else()
        set(BOOST_DLL_PATH "${BOOST_ROOT}/bin")
    endif()
    message(STATUS "Looking for Boost DLLs in: ${BOOST_DLL_PATH}")
    file(GLOB BOOST_TEST_DLL "${BOOST_DLL_PATH}/boost_unit_test_framework*.dll")

    foreach(dll IN LISTS BOOST_TEST_DLL)
        add_custom_command(TARGET eta_core_test POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${dll}"
                $<TARGET_FILE_DIR:eta_core_test>
                COMMENT "Copying Boost.Test DLL to output directory"
        )
    endforeach()
endif()

# Register with CTest
enable_testing()
add_test(NAME ${PROJECT_NAME} COMMAND ${PROJECT_NAME})