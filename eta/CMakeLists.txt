project(eta)

set(ETA_CMAKE_MODULE_DIR "${ETA_CMAKE_DIR}")
set(ETA_LAYOUT_ROOT_DIR "${ETA_SOURCE_DIR}")

set(ETA_CORE_DIR "${ETA_LAYOUT_ROOT_DIR}/core")
set(ETA_STDLIB_NATIVE_DIR "${ETA_REPO_ROOT_DIR}/packages/stdlib/native")
set(ETA_STDLIB_NATIVE_TORCH_SRC_DIR "${ETA_STDLIB_NATIVE_DIR}/torch/src")
set(ETA_STDLIB_NATIVE_NNG_SRC_DIR "${ETA_STDLIB_NATIVE_DIR}/nng/src")
set(ETA_STDLIB_NATIVE_STATS_SRC_DIR "${ETA_STDLIB_NATIVE_DIR}/stats/src")
set(ETA_STDLIB_NATIVE_LOG_SRC_DIR "${ETA_STDLIB_NATIVE_DIR}/log/src")
set(ETA_INTERPRETER_DIR "${ETA_LAYOUT_ROOT_DIR}/tools/interpreter")
set(ETA_COMPILER_DIR "${ETA_LAYOUT_ROOT_DIR}/tools/compiler")
set(ETA_CLI_DIR "${ETA_LAYOUT_ROOT_DIR}/cli")
set(ETA_LSP_DIR "${ETA_LAYOUT_ROOT_DIR}/tools/lsp")
set(ETA_DAP_DIR "${ETA_LAYOUT_ROOT_DIR}/tools/dap")
set(ETA_JUPYTER_DIR "${ETA_LAYOUT_ROOT_DIR}/tools/jupyter")
set(ETA_PROF_DIR "${ETA_LAYOUT_ROOT_DIR}/tools/prof")
set(ETA_TEST_DIR "${ETA_LAYOUT_ROOT_DIR}/qa/test")
set(ETA_PKG_TEST_DIR "${ETA_LAYOUT_ROOT_DIR}/qa/pkg_test")
set(ETA_CLI_TEST_DIR "${ETA_LAYOUT_ROOT_DIR}/qa/cli_test")
set(ETA_TEST_RUNNER_DIR "${ETA_LAYOUT_ROOT_DIR}/tools/test_runner")
set(ETA_BENCH_DIR "${ETA_LAYOUT_ROOT_DIR}/qa/bench")
set(ETA_FUZZ_DIR "${ETA_LAYOUT_ROOT_DIR}/qa/fuzz")

set(ETA_SESSION_SRC_DIR "${ETA_LAYOUT_ROOT_DIR}/session/src")
set(ETA_INTERPRETER_SRC_DIR "${ETA_INTERPRETER_DIR}/src")
set(ETA_DAP_SRC_DIR "${ETA_DAP_DIR}/src")
set(ETA_LSP_SRC_DIR "${ETA_LSP_DIR}/src")
set(ETA_JUPYTER_SRC_DIR "${ETA_JUPYTER_DIR}/src")
set(ETA_STDLIB_RUNTIME_DIR "${CMAKE_BINARY_DIR}/stdlib")
set(ETA_STDLIB_BUILD_SCRIPT "${ETA_REPO_ROOT_DIR}/scripts/build_stdlib_etac.py")
set(ETA_STDLIB_DOC_BUILD_SCRIPT "${ETA_REPO_ROOT_DIR}/scripts/build_stdlib_docs.py")
set(ETA_GENERATED_INCLUDE_DIR "${CMAKE_BINARY_DIR}/generated")
set(ETA_STDLIB_DOC_REGISTRY_INC
    "${ETA_GENERATED_INCLUDE_DIR}/eta/docs/generated/stdlib_docs.inc")

# Clean legacy test output path from pre-qa/test layout so stale binaries
# cannot shadow the active eta_core_test target.
file(REMOVE_RECURSE "${CMAKE_BINARY_DIR}/eta/test")


# Specify dependencies.
if(POLICY CMP0167)
    # Boost config/bjam/vcpkg is generally a mess,
    # until Boost gets rid of bjam, set this policy
    # to OLD.
    cmake_policy(SET CMP0167 OLD)
endif()

# stacktrace_windbg gives rich symbols on Windows; stacktrace_basic is the
# portable fallback on Linux/macOS (addresses only unless addr2line is present).
find_package(Boost 1.88 REQUIRED COMPONENTS unit_test_framework)
if(NOT Boost_FOUND)
    message(FATAL_ERROR "Boost not found.")
endif()
find_package(Python3 REQUIRED COMPONENTS Interpreter)

option(ETA_PROF_PPROF "Enable experimental pprof export support for profiling" OFF)
if(ETA_PROF_PPROF)
    add_compile_definitions(ETA_PROF_PPROF=1)
endif()

# ── libtorch (always required) ────────────────────────────────────────────
set(ETA_TORCH_BACKEND "cpu" CACHE STRING
    "libtorch backend for auto-download: cpu, cu126, cu128, or cu130")
set(ETA_LIBTORCH_VER "2.11.0" CACHE STRING
    "libtorch version for auto-download fallback")

find_package(Torch QUIET)
if(Torch_FOUND)
    message(STATUS "libtorch found — enabling native sidecar runtime support")
else()
    message(STATUS "libtorch not found locally — attempting auto-download (backend=${ETA_TORCH_BACKEND})")
    include("${ETA_CMAKE_MODULE_DIR}/FetchLibtorch.cmake")
    find_package(Torch REQUIRED)
    message(STATUS "libtorch auto-downloaded — enabling native sidecar runtime support (backend=${ETA_TORCH_BACKEND})")
endif()

# ── Utility: copy libtorch DLLs next to a target (Windows only) ──────
# Defined here (parent scope) so all subdirectories can call it.
# Usage: eta_copy_torch_dlls(<target>)
function(eta_copy_torch_dlls target)
    if(WIN32 AND TORCH_INSTALL_PREFIX)
        file(GLOB _torch_dlls "${TORCH_INSTALL_PREFIX}/lib/*.dll")
        foreach(dll IN LISTS _torch_dlls)
            add_custom_command(TARGET ${target} POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${dll}"
                    $<TARGET_FILE_DIR:${target}>
            )
        endforeach()
        list(LENGTH _torch_dlls _n)
        if(_n GREATER 0)
            message(STATUS "Will copy ${_n} libtorch DLL(s) to ${target} output dir")
        endif()
    endif()
endfunction()

# ── nng (always required) ─────────────────────────────────────────────────
message(STATUS "Fetching nng (nanomsg-next-gen) v1.11 ...")
include("${ETA_CMAKE_MODULE_DIR}/FetchNng.cmake")
message(STATUS "Fetching spdlog 1.17.0 ...")
include("${ETA_CMAKE_MODULE_DIR}/FetchSpdlog.cmake")
if(NOT TARGET spdlog::spdlog)
    message(FATAL_ERROR "spdlog fetch FAILED - spdlog::spdlog target missing")
endif()
if(NOT TARGET nng)
    message(FATAL_ERROR "nng fetch FAILED — nng is a required dependency")
endif()

# ── Eigen (always required for stats primitives) ──────────────────────────
message(STATUS "Fetching Eigen 5.0.0 (header-only) ...")
include("${ETA_CMAKE_MODULE_DIR}/FetchEigen.cmake")
message(STATUS "Fetching csv-parser 4.0.0 ...")
include("${ETA_CMAKE_MODULE_DIR}/FetchCsvParser.cmake")
if(NOT TARGET csv::parser)
    message(FATAL_ERROR "csv-parser fetch FAILED - csv::parser target missing")
endif()
if(NOT TARGET Eigen3::Eigen)
    message(FATAL_ERROR "Eigen fetch FAILED — Eigen is a required dependency")
endif()

# ── Utility: copy nng DLL next to a target (Windows only) ────────────
# On Windows, nng is built as a shared library; the DLL must sit beside
# each executable.  On Linux/macOS nng is static — this is a no-op.
# Usage: eta_copy_nng_dlls(<target>)
function(eta_copy_nng_dlls target)
    if(WIN32 AND TARGET nng)
        add_custom_command(TARGET ${target} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_FILE:nng>
                $<TARGET_FILE_DIR:${target}>
            COMMENT "Copying nng DLL to ${target} output dir"
        )
    endif()
endfunction()

function(eta_spdlog_target out_var)
    if(TARGET spdlog)
        set(${out_var} spdlog PARENT_SCOPE)
    elseif(TARGET spdlog::spdlog)
        set(${out_var} spdlog::spdlog PARENT_SCOPE)
    else()
        set(${out_var} "" PARENT_SCOPE)
    endif()
endfunction()

function(eta_copy_log_dlls target)
    eta_spdlog_target(_eta_spdlog_target)
    if(WIN32 AND _eta_spdlog_target)
        add_custom_command(TARGET ${target} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_FILE:${_eta_spdlog_target}>
                $<TARGET_FILE_DIR:${target}>
            COMMENT "Copying spdlog DLL to ${target} output dir"
        )
    endif()
endfunction()

function(eta_link_sidecar_builtin_runtime target)
    target_include_directories(${target} PRIVATE
        ${ETA_SESSION_SRC_DIR}
        ${ETA_STDLIB_NATIVE_TORCH_SRC_DIR}
        ${ETA_STDLIB_NATIVE_NNG_SRC_DIR}
        ${ETA_STDLIB_NATIVE_STATS_SRC_DIR}
        ${ETA_STDLIB_NATIVE_LOG_SRC_DIR}
    )

    target_include_directories(${target} SYSTEM PRIVATE
        ${TORCH_INCLUDE_DIRS}
    )

    target_link_libraries(${target} PRIVATE
        ${TORCH_LIBRARIES}
        nng
        Eigen3::Eigen
    )

    eta_spdlog_target(_eta_spdlog_target)
    if(_eta_spdlog_target)
        target_link_libraries(${target} PRIVATE ${_eta_spdlog_target})
    endif()

    if(TORCH_CXX_FLAGS)
        target_compile_options(${target} PRIVATE ${TORCH_CXX_FLAGS})
    endif()

    if(MSVC)
        target_compile_options(${target} PRIVATE
            /wd4127
            /wd4244
            /wd4267
        )
    endif()

    # Linux sidecars are dlopen'd MODULE libraries that resolve eta_core
    # references from the host process; executable hosts must export symbols.
    if(UNIX AND NOT APPLE)
        get_target_property(_eta_target_type ${target} TYPE)
        if(_eta_target_type STREQUAL "EXECUTABLE")
            target_link_options(${target} PRIVATE -rdynamic)
        endif()
    endif()
endfunction()

# On Linux/macOS, set RPATH so installed binaries find libtorch in ../lib.
# Must be set before add_subdirectory() so it applies to all executable targets.
if(NOT WIN32)
    set(CMAKE_INSTALL_RPATH "$ORIGIN/../lib")
    set(CMAKE_INSTALL_RPATH_USE_LINK_PATH TRUE)
endif()

# Runtime core.
add_subdirectory("${ETA_CORE_DIR}" core)

# User-facing tools and protocol servers.
add_subdirectory("${ETA_INTERPRETER_DIR}")
add_subdirectory("${ETA_COMPILER_DIR}")
add_subdirectory("${ETA_PROF_DIR}")
add_subdirectory("${ETA_CLI_DIR}")
add_subdirectory("${ETA_LSP_DIR}")
add_subdirectory("${ETA_DAP_DIR}")
add_subdirectory("${ETA_JUPYTER_DIR}")

# Quality and performance targets.
add_subdirectory("${ETA_TEST_DIR}")
add_subdirectory("${ETA_PKG_TEST_DIR}")
add_subdirectory("${ETA_CLI_TEST_DIR}")
add_subdirectory("${ETA_TEST_RUNNER_DIR}")
add_subdirectory("${ETA_BENCH_DIR}")
add_subdirectory("${ETA_FUZZ_DIR}")

file(GLOB_RECURSE ETA_STDLIB_ETA_SOURCES CONFIGURE_DEPENDS
    "${ETA_STDLIB_SOURCE_DIR}/*.eta")

add_custom_command(
    OUTPUT "${ETA_STDLIB_DOC_REGISTRY_INC}"
    COMMAND ${CMAKE_COMMAND} -E make_directory
            "${ETA_GENERATED_INCLUDE_DIR}/eta/docs/generated"
    COMMAND ${Python3_EXECUTABLE} "${ETA_STDLIB_DOC_BUILD_SCRIPT}"
            --src-root "${ETA_STDLIB_SOURCE_DIR}"
            --out "${ETA_STDLIB_DOC_REGISTRY_INC}"
    DEPENDS "${ETA_STDLIB_DOC_BUILD_SCRIPT}" ${ETA_STDLIB_ETA_SOURCES}
    VERBATIM
)
add_custom_target(eta_stdlib_doc_registry DEPENDS "${ETA_STDLIB_DOC_REGISTRY_INC}")

set(ETA_STDLIB_BUILD_STAMP "${ETA_STDLIB_RUNTIME_DIR}/.eta_stdlib_etac.stamp")
add_custom_command(
    OUTPUT "${ETA_STDLIB_BUILD_STAMP}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${ETA_STDLIB_RUNTIME_DIR}"
    COMMAND ${Python3_EXECUTABLE} "${ETA_STDLIB_BUILD_SCRIPT}"
            --etac "$<TARGET_FILE:etac>"
            --src-root "${ETA_STDLIB_SOURCE_DIR}"
            --out-root "${ETA_STDLIB_RUNTIME_DIR}"
    COMMAND ${CMAKE_COMMAND} -E touch "${ETA_STDLIB_BUILD_STAMP}"
    DEPENDS etac "${ETA_STDLIB_BUILD_SCRIPT}" ${ETA_STDLIB_ETA_SOURCES}
    VERBATIM
)
add_custom_target(eta_stdlib_etac DEPENDS "${ETA_STDLIB_BUILD_STAMP}")

if(TARGET eta_core)
    add_dependencies(eta_core eta_stdlib_doc_registry)
endif()

foreach(_t IN ITEMS etai eta_repl eta_lsp eta_dap eta_jupyter)
    if(TARGET ${_t})
        add_dependencies(${_t} eta_stdlib_etac)
    endif()
endforeach()

# Keep the common local workflow target aligned with test/runtime dependencies:
# building eta_core_test should also rebuild etai + etac + eta_test_runner.
if(TARGET eta_core_test AND TARGET etai AND TARGET etac AND TARGET eta_test)
    add_dependencies(eta_core_test etai etac eta_test)
endif()

# Convenience target: rebuild core binaries and run the two canonical test suites.
add_custom_target(eta_rebuild_and_test
    COMMAND $<TARGET_FILE:eta_core_test>
    COMMAND $<TARGET_FILE:eta_test>
        --path "${ETA_STDLIB_SOURCE_DIR}"
        "${ETA_STDLIB_SOURCE_DIR}/tests"
    DEPENDS etai etac eta_test eta_core_test
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    COMMENT "Rebuild etai + etac + eta_test + eta_core_test, then run runtime + stdlib tests"
    USES_TERMINAL
)

add_test(
    NAME stdlib_doc_extractor_tests
    COMMAND ${Python3_EXECUTABLE}
        "${ETA_REPO_ROOT_DIR}/scripts/tests/test_build_stdlib_docs.py"
)
set_tests_properties(stdlib_doc_extractor_tests PROPERTIES
    WORKING_DIRECTORY "${ETA_REPO_ROOT_DIR}")

add_test(
    NAME builtin_catalog_docs_guard
    COMMAND ${Python3_EXECUTABLE}
        "${ETA_REPO_ROOT_DIR}/scripts/tests/check_builtin_catalog_docs.py"
        --root "${ETA_REPO_ROOT_DIR}"
)
set_tests_properties(builtin_catalog_docs_guard PROPERTIES
    WORKING_DIRECTORY "${ETA_REPO_ROOT_DIR}")

add_test(
    NAME builtin_catalog_docs_guard_tests
    COMMAND ${Python3_EXECUTABLE}
        "${ETA_REPO_ROOT_DIR}/scripts/tests/test_check_builtin_catalog_docs.py"
)
set_tests_properties(builtin_catalog_docs_guard_tests PROPERTIES
    WORKING_DIRECTORY "${ETA_REPO_ROOT_DIR}")

add_test(
    NAME legacy_builtin_names_symbol_guard
    COMMAND ${Python3_EXECUTABLE}
        "${ETA_REPO_ROOT_DIR}/scripts/tests/check_legacy_builtin_names_symbol.py"
        --root "${ETA_REPO_ROOT_DIR}"
)
set_tests_properties(legacy_builtin_names_symbol_guard PROPERTIES
    WORKING_DIRECTORY "${ETA_REPO_ROOT_DIR}")

add_test(
    NAME legacy_builtin_names_symbol_guard_tests
    COMMAND ${Python3_EXECUTABLE}
        "${ETA_REPO_ROOT_DIR}/scripts/tests/test_check_legacy_builtin_names_symbol.py"
)
set_tests_properties(legacy_builtin_names_symbol_guard_tests PROPERTIES
    WORKING_DIRECTORY "${ETA_REPO_ROOT_DIR}")

# Install libtorch runtime libraries alongside Eta binaries.
if(TORCH_INSTALL_PREFIX)
    if(WIN32)
        file(GLOB _torch_runtime_dlls "${TORCH_INSTALL_PREFIX}/lib/*.dll")
        install(FILES ${_torch_runtime_dlls} DESTINATION bin)
    else()
        file(GLOB _torch_runtime_so
            "${TORCH_INSTALL_PREFIX}/lib/*.so*"
            "${TORCH_INSTALL_PREFIX}/lib/*.dylib*")
        install(FILES ${_torch_runtime_so} DESTINATION lib)
    endif()
endif()

# Install native sidecar package metadata/artifact layout expected by
# runtime bundled-sidecar discovery.
if(EXISTS "${ETA_STDLIB_NATIVE_DIR}")
    install(
        DIRECTORY "${ETA_STDLIB_NATIVE_DIR}/"
        DESTINATION "packages/stdlib/native")
endif()

# Install nng DLL on Windows
if(WIN32 AND TARGET nng)
    install(FILES $<TARGET_FILE:nng> DESTINATION bin)
endif()

eta_spdlog_target(_eta_install_spdlog_target)
if(WIN32 AND _eta_install_spdlog_target)
    install(FILES $<TARGET_FILE:${_eta_install_spdlog_target}> DESTINATION bin)
endif()
