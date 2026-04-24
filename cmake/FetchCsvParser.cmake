include(FetchContent)

FetchContent_Declare(
    csv_parser
    GIT_REPOSITORY https://github.com/vincentlaucsb/csv-parser.git
    GIT_TAG        2.3.0
    GIT_SHALLOW    TRUE
    EXCLUDE_FROM_ALL
    SYSTEM
)

if(POLICY CMP0169)
    cmake_policy(PUSH)
    cmake_policy(SET CMP0169 OLD)
endif()

FetchContent_GetProperties(csv_parser)
if(NOT csv_parser_POPULATED)
    FetchContent_Populate(csv_parser)
endif()

if(POLICY CMP0169)
    cmake_policy(POP)
endif()

add_library(eta_csv_parser INTERFACE)
target_include_directories(eta_csv_parser SYSTEM INTERFACE
    ${csv_parser_SOURCE_DIR}/single_include
)

add_library(csv::parser ALIAS eta_csv_parser)

message(STATUS "csv-parser 2.3.0 fetched - csv::parser target available")
