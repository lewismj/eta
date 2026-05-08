cmake_minimum_required(VERSION 3.28)
project(eta_prof_tool LANGUAGES CXX)

add_executable(eta_prof
    src/eta/prof/main_eta_prof.cpp
)

set_target_properties(eta_prof PROPERTIES CXX_SCAN_FOR_MODULES OFF)

target_include_directories(eta_prof
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(eta_prof PRIVATE eta_core)

if(NOT MSVC)
    target_link_libraries(eta_prof PRIVATE stdc++ m)
endif()

install(TARGETS eta_prof RUNTIME DESTINATION bin)

