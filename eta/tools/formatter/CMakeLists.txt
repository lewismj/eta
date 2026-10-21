add_library(eta_format_lib STATIC
    src/eta/format/doc.cpp
    src/eta/format/format_source.cpp
    src/eta/format/forms.cpp
    src/eta/format/layout.cpp)
target_link_libraries(eta_format_lib PUBLIC eta_cst)
target_include_directories(eta_format_lib PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
set_target_properties(eta_format_lib PROPERTIES CXX_SCAN_FOR_MODULES OFF)


add_executable(eta_formatter
    src/eta/format/main_eta_fmt.cpp
    src/eta/format/cli/args.cpp)
set_target_properties(eta_formatter PROPERTIES OUTPUT_NAME eta_fmt CXX_SCAN_FOR_MODULES OFF)
target_link_libraries(eta_formatter PRIVATE eta_format_lib eta_tooling_files)
if(NOT MSVC)
    target_link_libraries(eta_formatter PRIVATE stdc++ m)
endif()
target_compile_definitions(eta_formatter PRIVATE ETA_FMT_VERSION="${CMAKE_PROJECT_VERSION}")
eta_copy_log_dlls(eta_formatter)
install(TARGETS eta_formatter RUNTIME DESTINATION bin)
add_test(NAME eta_formatter_cli
    COMMAND ${Python3_EXECUTABLE} ${ETA_REPO_ROOT_DIR}/scripts/tests/test_formatter_cli.py
        $<TARGET_FILE:eta_formatter> $<TARGET_FILE:etai> ${ETA_REPO_ROOT_DIR})
