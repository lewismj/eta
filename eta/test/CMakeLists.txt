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
        src/semantics_tests.cpp
        src/vm_tests.cpp
        src/eta_test.cpp
        src/emitter_tests.cpp
        src/port_tests.cpp
        src/expander_tests.cpp
        src/diagnostic_tests.cpp
        src/functional_tests.cpp
        src/debug_tests.cpp
        src/dap_tests.cpp
        src/module_path_tests.cpp
        src/higher_order_tests.cpp
        src/example_runner_tests.cpp
        src/compiled_example_tests.cpp
        src/bytecode_serializer_tests.cpp
        src/disassembler_tests.cpp
        src/optimization_tests.cpp)

add_executable(${PROJECT_NAME} ${SOURCES})

# Inject source tree paths for the example runner integration test.
# These point at the source-tree examples/ and stdlib/ directories so the
# test can find them regardless of the build output directory.
target_compile_definitions(${PROJECT_NAME} PRIVATE
    ETA_EXAMPLES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../../examples"
    ETA_STDLIB_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../../stdlib"
)

# Pull in the DAP server implementation (not main_dap.cpp) so dap_tests.cpp
# can link against DapServer with injected streams.
target_sources(${PROJECT_NAME} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../dap/src/eta/dap/dap_io.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../dap/src/eta/dap/dap_server.cpp
)

# Base linking - only eta_core unconditionally
target_link_libraries(${PROJECT_NAME} PRIVATE eta_core)
target_link_libraries(${PROJECT_NAME} PRIVATE Boost::unit_test_framework)

# Expose interpreter (driver.h) and DAP headers
target_include_directories(${PROJECT_NAME} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../interpreter/src
    ${CMAKE_CURRENT_SOURCE_DIR}/../dap/src
)

# vcpkg x64-windows builds shared Boost; x64-linux builds static.
# BOOST_TEST_DYN_LINK is required only when linking the shared library.
if(WIN32)
    target_compile_definitions(${PROJECT_NAME} PRIVATE BOOST_TEST_DYN_LINK)
endif()


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
        elseif(Boost_INCLUDE_DIRS)
            # Derive from CMake/vcpkg found include path
            # e.g. .../installed/x64-windows/include -> .../installed/x64-windows
            list(GET Boost_INCLUDE_DIRS 0 _boost_inc)
            get_filename_component(BOOST_ROOT "${_boost_inc}" DIRECTORY)
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