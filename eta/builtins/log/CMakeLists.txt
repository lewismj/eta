cmake_minimum_required(VERSION 3.28)
project(eta_log LANGUAGES CXX)

add_library(${PROJECT_NAME} INTERFACE)

target_include_directories(${PROJECT_NAME} INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(${PROJECT_NAME} INTERFACE
    eta_core
    spdlog::spdlog
)
