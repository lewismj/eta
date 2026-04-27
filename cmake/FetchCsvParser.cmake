include(FetchContent)

# csv-parser 4.x builds a compiled `csv` static library. Keep optional
# artifacts off for Eta integration.
set(BUILD_PYTHON OFF CACHE BOOL "" FORCE)
set(CSV_BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)
set(CSV_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(CSV_BUILD_SINGLE_INCLUDE_TEST OFF CACHE BOOL "" FORCE)
# Keep runtime behavior conservative to match Eta's previous parser profile.
set(CSV_ENABLE_THREADS OFF CACHE BOOL "" FORCE)
set(CSV_NO_SIMD ON CACHE BOOL "" FORCE)
set(CSV_FORCE_AVX2 OFF CACHE BOOL "" FORCE)

FetchContent_Declare(
    csv_parser
    GIT_REPOSITORY https://github.com/vincentlaucsb/csv-parser.git
    GIT_TAG        4.0.0
    GIT_SHALLOW    TRUE
    EXCLUDE_FROM_ALL
    SYSTEM
)

if(POLICY CMP0169)
    cmake_policy(PUSH)
    cmake_policy(SET CMP0169 OLD)
endif()

FetchContent_MakeAvailable(csv_parser)

if(POLICY CMP0169)
    cmake_policy(POP)
endif()

if(TARGET csv)
    if(NOT TARGET csv::parser)
        add_library(csv::parser ALIAS csv)
    endif()
    message(STATUS "csv-parser 4.0.0 fetched - csv::parser target available")
else()
    # Fallback for very old header-only releases.
    add_library(eta_csv_parser INTERFACE)
    if(EXISTS "${csv_parser_SOURCE_DIR}/include/csv.hpp")
        set(_eta_csv_parser_include_dir "${csv_parser_SOURCE_DIR}/include")
    else()
        set(_eta_csv_parser_include_dir "${csv_parser_SOURCE_DIR}/single_include")
    endif()
    target_include_directories(eta_csv_parser SYSTEM INTERFACE
        "${_eta_csv_parser_include_dir}"
    )
    if(NOT TARGET csv::parser)
        add_library(csv::parser ALIAS eta_csv_parser)
    endif()
    message(STATUS "csv-parser fallback mode enabled - using header-only interface target")
endif()
