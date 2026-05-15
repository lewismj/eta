include(FetchContent)

set(ETA_DUCKDB_UPSTREAM_TAG "v1.5.1" CACHE STRING
    "Pinned DuckDB upstream tag used by the DuckDB sidecar package.")

function(eta_duckdb_fetch)
    set(BUILD_MAIN_DUCKDB_LIBRARY ON CACHE BOOL
        "Build the main DuckDB library for Eta sidecar linkage."
        FORCE)
    set(BUILD_SHELL OFF CACHE BOOL
        "Disable DuckDB shell binary for Eta sidecar builds."
        FORCE)
    set(BUILD_UNITTESTS OFF CACHE BOOL
        "Disable DuckDB upstream tests for Eta sidecar builds."
        FORCE)
    set(ENABLE_UNITTEST_CPP_TESTS OFF CACHE BOOL
        "Disable DuckDB upstream C++ unit tests for Eta sidecar builds."
        FORCE)
    set(BUILD_BENCHMARKS OFF CACHE BOOL
        "Disable DuckDB benchmarks for Eta sidecar builds."
        FORCE)
    set(OVERRIDE_GIT_DESCRIBE "${ETA_DUCKDB_UPSTREAM_TAG}" CACHE STRING
        "Pinned DuckDB version string used for sidecar builds."
        FORCE)

    FetchContent_Declare(
        duckdb_upstream
        GIT_REPOSITORY https://github.com/duckdb/duckdb.git
        GIT_TAG ${ETA_DUCKDB_UPSTREAM_TAG}
        GIT_SHALLOW TRUE
    )

    # Upstream DuckDB installs to fixed locations from its own CMake config.
    # Route that into the build tree so super-build installs do not pollute
    # the caller's install prefix.
    set(_eta_duckdb_install_prefix "${CMAKE_INSTALL_PREFIX}")
    set(_eta_duckdb_prev_cxx_standard "${CMAKE_CXX_STANDARD}")
    set(CMAKE_INSTALL_PREFIX "${CMAKE_BINARY_DIR}/_deps/duckdb-install" CACHE PATH
        "Install path prefix, prepended onto install directories."
        FORCE)
    # DuckDB v1.5.1 still uses std::uncaught_exception in parts of the codebase.
    # Build DuckDB with a pre-C++20 mode while leaving Eta targets on C++23.
    set(CMAKE_CXX_STANDARD 17)
    set(CMAKE_CXX_STANDARD 17 CACHE STRING
        "C++ standard to enforce"
        FORCE)
    # DuckDB's upstream install tree includes extension-loader artifacts that
    # Eta does not package/use directly; skip generating upstream install rules
    # so super-build installs only include Eta-owned install entries.
    if(DEFINED CMAKE_SKIP_INSTALL_RULES)
        set(_eta_duckdb_prev_skip_install_rules_defined TRUE)
        set(_eta_duckdb_prev_skip_install_rules "${CMAKE_SKIP_INSTALL_RULES}")
    else()
        set(_eta_duckdb_prev_skip_install_rules_defined FALSE)
    endif()
    set(CMAKE_SKIP_INSTALL_RULES ON)
    FetchContent_MakeAvailable(duckdb_upstream)
    if(_eta_duckdb_prev_skip_install_rules_defined)
        set(CMAKE_SKIP_INSTALL_RULES "${_eta_duckdb_prev_skip_install_rules}")
    else()
        unset(CMAKE_SKIP_INSTALL_RULES)
    endif()
    set(CMAKE_CXX_STANDARD "${_eta_duckdb_prev_cxx_standard}")
    set(CMAKE_CXX_STANDARD "${_eta_duckdb_prev_cxx_standard}" CACHE STRING
        "C++ standard to enforce"
        FORCE)
    set(CMAKE_INSTALL_PREFIX "${_eta_duckdb_install_prefix}" CACHE PATH
        "Install path prefix, prepended onto install directories."
        FORCE)

    set(ETA_DUCKDB_SOURCE_DIR "${duckdb_upstream_SOURCE_DIR}" PARENT_SCOPE)
    set(ETA_DUCKDB_BINARY_DIR "${duckdb_upstream_BINARY_DIR}" PARENT_SCOPE)
    message(STATUS
        "DuckDB source fetched at ${duckdb_upstream_SOURCE_DIR} (tag ${ETA_DUCKDB_UPSTREAM_TAG})")
endfunction()
