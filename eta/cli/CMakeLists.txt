cmake_minimum_required(VERSION 3.28)
project(eta_cli LANGUAGES CXX)

add_executable(eta
    src/eta/cli/main_eta.cpp
)

set_target_properties(eta PROPERTIES CXX_SCAN_FOR_MODULES OFF)

target_include_directories(eta
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(eta PRIVATE eta_core)
target_compile_definitions(eta PRIVATE
    ETA_ETAI_PATH="$<TARGET_FILE:etai>"
    ETA_ETAC_PATH="$<TARGET_FILE:etac>"
    ETA_ETA_TEST_PATH="$<TARGET_FILE:eta_test>"
)

install(TARGETS eta RUNTIME DESTINATION bin)
