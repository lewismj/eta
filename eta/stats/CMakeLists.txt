cmake_minimum_required(VERSION 3.28)
project(eta_stats LANGUAGES CXX)

add_library(${PROJECT_NAME} INTERFACE)

target_include_directories(${PROJECT_NAME} INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(${PROJECT_NAME} INTERFACE
    eta_core
    Eigen3::Eigen
)

# Suppress Eigen header warnings on MSVC (C4127 conditional expression is
# constant, C4244 narrowing conversion).
if(MSVC)
    target_compile_options(${PROJECT_NAME} INTERFACE /wd4127 /wd4244)
endif()

