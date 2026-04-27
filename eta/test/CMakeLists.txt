project(eta_core_test LANGUAGES CXX)

set(SOURCES
        src/parser_tests.cpp
        src/lexer_tests.cpp
        src/heap_tests.cpp
        src/factory_tests.cpp
        src/gc_tests.cpp
        src/gc_stress_tests.cpp
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
        src/lsp_tests.cpp
        src/module_path_tests.cpp
        src/repl_redefine_tests.cpp
        src/higher_order_tests.cpp
        src/example_runner_tests.cpp
        src/compiled_example_tests.cpp
        src/bytecode_serializer_tests.cpp
        src/disassembler_tests.cpp
        src/optimization_tests.cpp
        src/cons_pool_tests.cpp
        src/stats_tests.cpp
        src/time_primitives_tests.cpp
        src/csv_reader_tests.cpp
        src/csv_writer_tests.cpp
        src/csv_fact_table_tests.cpp
        src/regex_tests.cpp
        src/nng_tests.cpp
        src/clp_linear_tests.cpp
        src/clp_qp_linearize_tests.cpp
        src/clp_qp_matrix_tests.cpp
        src/clp_qp_solver_tests.cpp
        src/clp_fm_tests.cpp
        src/clp_simplex_tests.cpp
        src/builtin_sync_tests.cpp
        src/driver_jupyter_test.cpp
        src/jupyter_kernel_config_test.cpp
        src/jupyter_magics_test.cpp)

# libtorch ships release binaries only (_ITERATOR_DEBUG_LEVEL=0).
# MSVC Debug builds use _ITERATOR_DEBUG_LEVEL=2 which causes ABI mismatch and
# crashes inside libtorch inline template code (ordered_dict.h etc.).
# Skip torch_tests entirely in MSVC Debug; mark torch examples as skippable.
if(NOT (MSVC AND CMAKE_BUILD_TYPE STREQUAL "Debug"))
    list(APPEND SOURCES src/torch_tests.cpp)
endif()

add_executable(${PROJECT_NAME} ${SOURCES})

# Disable C++20 module scanning — this project doesn't use modules and the
# scanner fails to locate standard library headers when Boost headers are
# transitively included (MSVC + CMake 3.28+ scanDependencies bug).
set_target_properties(${PROJECT_NAME} PROPERTIES CXX_SCAN_FOR_MODULES OFF)

# libtorch ships release binaries only (_ITERATOR_DEBUG_LEVEL=0).
# MSVC Debug uses _ITERATOR_DEBUG_LEVEL=2 — ABI mismatch crashes torch inline
# template code. Skip torch_tests (excluded above) and mark torch examples
# as skippable via ETA_TORCH_DEBUG_SKIP.
if(MSVC AND CMAKE_BUILD_TYPE STREQUAL "Debug")
    target_compile_definitions(${PROJECT_NAME} PRIVATE ETA_TORCH_DEBUG_SKIP=1)
endif()

# Inject source tree paths for the example runner integration test.
target_compile_definitions(${PROJECT_NAME} PRIVATE
    ETA_EXAMPLES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../../examples"
    ETA_STDLIB_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../../stdlib"
)

# Inject the path to the etai binary for spawn tests.
target_compile_definitions(${PROJECT_NAME} PRIVATE
    ETA_ETAI_PATH="$<TARGET_FILE:etai>"
    ETA_ETAC_PATH="$<TARGET_FILE:etac>"
)

# Pull in the DAP server implementation (not main_dap.cpp) so dap_tests.cpp
# can link against DapServer with injected streams.
target_sources(${PROJECT_NAME} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../dap/src/eta/dap/dap_io.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../dap/src/eta/dap/dap_server.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../lsp/src/eta/lsp/lsp_server.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../jupyter/src/eta/jupyter/kernel_config.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../jupyter/src/eta/jupyter/magics.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../interpreter/src/eta/interpreter/repl_wrap.cpp
)

# Always link all runtime libraries
target_link_libraries(${PROJECT_NAME} PRIVATE eta_core eta_torch eta_nng eta_stats)
target_link_libraries(${PROJECT_NAME} PRIVATE Boost::unit_test_framework)
target_compile_definitions(${PROJECT_NAME} PRIVATE
    ETA_HAS_TORCH=1 ETA_HAS_NNG=1 ETA_HAS_STATS=1)
eta_copy_torch_dlls(${PROJECT_NAME})
eta_copy_nng_dlls(${PROJECT_NAME})

# Expose session driver, interpreter helpers, DAP, and LSP headers.
target_include_directories(${PROJECT_NAME} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../interpreter/src
    ${CMAKE_CURRENT_SOURCE_DIR}/../session/src
    ${CMAKE_CURRENT_SOURCE_DIR}/../dap/src
    ${CMAKE_CURRENT_SOURCE_DIR}/../jupyter/src
    ${CMAKE_CURRENT_SOURCE_DIR}/../lsp/src
)

# vcpkg x64-windows builds shared Boost; x64-linux builds static.
if(WIN32)
    target_compile_definitions(${PROJECT_NAME} PRIVATE BOOST_TEST_DYN_LINK)
endif()

# Platform-specific libraries
if(NOT MSVC)
    target_link_libraries(${PROJECT_NAME} PRIVATE stdc++ m)

    target_compile_options(${PROJECT_NAME} PRIVATE "-msse2")

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
            list(GET Boost_INCLUDE_DIRS 0 _boost_inc)
            get_filename_component(BOOST_ROOT "${_boost_inc}" DIRECTORY)
        else()
            message(FATAL_ERROR "BOOST_ROOT not set.")
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
