add_library(eta_tooling_files STATIC src/eta/tooling/files.cpp)
target_include_directories(eta_tooling_files PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_features(eta_tooling_files PUBLIC cxx_std_23)
set_target_properties(eta_tooling_files PROPERTIES CXX_SCAN_FOR_MODULES OFF)
if(WIN32)
    target_compile_definitions(eta_tooling_files PUBLIC NOMINMAX WIN32_LEAN_AND_MEAN)
    target_link_libraries(eta_tooling_files PRIVATE advapi32)
endif()

# Content-Length message framing shared by the LSP and DAP servers.
add_library(eta_tooling_framing STATIC src/eta/tooling/framing.cpp)
target_include_directories(eta_tooling_framing PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_features(eta_tooling_framing PUBLIC cxx_std_23)
set_target_properties(eta_tooling_framing PROPERTIES CXX_SCAN_FOR_MODULES OFF)
