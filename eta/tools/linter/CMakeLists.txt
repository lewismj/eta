add_library(eta_linter_lib STATIC
    src/eta/linter/lint.cpp
    src/eta/linter/core/registry.cpp
    src/eta/linter/core/config.cpp
    src/eta/linter/rules/rules.cpp
    src/eta/linter/rules/eta104_non_kebab_case.cpp
    src/eta/linter/rules/eta203_duplicate_export.cpp
    src/eta/linter/rules/eta205_import_order.cpp
    src/eta/linter/rules/eta401_comment_marker_style.cpp
    src/eta/linter/rules/eta501_function_too_long.cpp
)
target_link_libraries(eta_linter_lib PUBLIC eta_cst)
target_include_directories(eta_linter_lib PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_features(eta_linter_lib PUBLIC cxx_std_23)
set_target_properties(eta_linter_lib PROPERTIES CXX_SCAN_FOR_MODULES OFF POSITION_INDEPENDENT_CODE ON)

add_executable(eta_linter
    src/eta/linter/main_eta_lint.cpp
    src/eta/linter/cli/args.cpp
    src/eta/linter/cli/reporter.cpp
)
set_target_properties(eta_linter PROPERTIES
    OUTPUT_NAME eta_lint
    CXX_SCAN_FOR_MODULES OFF
)
target_link_libraries(eta_linter PRIVATE eta_linter_lib eta_tooling_files)
target_compile_definitions(eta_linter PRIVATE ETA_LINT_VERSION="${CMAKE_PROJECT_VERSION}")
eta_copy_log_dlls(eta_linter)
install(TARGETS eta_linter RUNTIME DESTINATION bin)

add_test(NAME eta_linter_cli
    COMMAND ${Python3_EXECUTABLE} ${ETA_REPO_ROOT_DIR}/scripts/tests/test_linter_cli.py
        $<TARGET_FILE:eta_linter>
)
add_test(NAME eta_linter_fixtures
    COMMAND ${Python3_EXECUTABLE} ${ETA_REPO_ROOT_DIR}/scripts/tests/test_linter_fixtures.py
        $<TARGET_FILE:eta_linter>
        ${CMAKE_CURRENT_SOURCE_DIR}/rules
        ${ETA_REPO_ROOT_DIR}/eta/qa/test/fixtures/linter
)
