cmake_minimum_required(VERSION 3.28)
project(eta_nng LANGUAGES CXX)

add_library(${PROJECT_NAME} INTERFACE)

target_include_directories(${PROJECT_NAME} INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(${PROJECT_NAME} INTERFACE
    eta_core
    nng
)

# Suppress nng header warnings on MSVC (C4244 narrowing, C4267 size_t→int)
if(MSVC)
    target_compile_options(${PROJECT_NAME} INTERFACE /wd4244 /wd4267)
endif()

