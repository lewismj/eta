cmake_minimum_required(VERSION 3.28)
project(eta_cli LANGUAGES CXX)

set(ETA_CLI_VERSION_STRING "${CMAKE_PROJECT_VERSION}")
find_package(Git QUIET)
if(GIT_FOUND)
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" -C "${CMAKE_SOURCE_DIR}" describe --tags --dirty
        RESULT_VARIABLE ETA_CLI_GIT_DESCRIBE_RESULT
        OUTPUT_VARIABLE ETA_CLI_GIT_DESCRIBE
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
    )
    if(ETA_CLI_GIT_DESCRIBE_RESULT EQUAL 0 AND NOT ETA_CLI_GIT_DESCRIBE STREQUAL "")
        set(ETA_CLI_VERSION_STRING "${ETA_CLI_GIT_DESCRIBE}")
    endif()
endif()

add_executable(eta
    src/eta/cli/main_eta.cpp
)

set_target_properties(eta PROPERTIES CXX_SCAN_FOR_MODULES OFF)

target_include_directories(eta
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(eta PRIVATE eta_core)
target_compile_definitions(eta PRIVATE
    ETA_CLI_VERSION="${ETA_CLI_VERSION_STRING}"
    ETA_ETAI_PATH="$<TARGET_FILE:etai>"
    ETA_ETAC_PATH="$<TARGET_FILE:etac>"
    ETA_ETA_TEST_PATH="$<TARGET_FILE:eta_test>"
    ETA_ETA_PROF_PATH="$<TARGET_FILE:eta_prof>"
)
add_dependencies(eta eta_prof)

if(NOT MSVC)
    target_link_libraries(eta PRIVATE stdc++ m)
endif()

install(TARGETS eta RUNTIME DESTINATION bin)
