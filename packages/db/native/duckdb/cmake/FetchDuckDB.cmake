include(FetchContent)

function(eta_duckdb_fetch)
    set(ETA_DUCKDB_TAG "v1.5.1")

    FetchContent_Declare(
        duckdb_upstream
        GIT_REPOSITORY https://github.com/duckdb/duckdb.git
        GIT_TAG ${ETA_DUCKDB_TAG}
        GIT_SHALLOW TRUE
    )

    FetchContent_GetProperties(duckdb_upstream)
    if(NOT duckdb_upstream_POPULATED)
        FetchContent_Populate(duckdb_upstream)
    endif()

    set(ETA_DUCKDB_SOURCE_DIR "${duckdb_upstream_SOURCE_DIR}" PARENT_SCOPE)
    message(STATUS "DuckDB source fetched at ${duckdb_upstream_SOURCE_DIR} (tag ${ETA_DUCKDB_TAG})")
endfunction()
