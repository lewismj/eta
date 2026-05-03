project(eta_pkg_test LANGUAGES CXX)

add_executable(${PROJECT_NAME}
    src/eta_pkg_test.cpp
)

set_target_properties(${PROJECT_NAME} PROPERTIES CXX_SCAN_FOR_MODULES OFF)

target_link_libraries(${PROJECT_NAME} PRIVATE eta_core Boost::unit_test_framework)

if(WIN32)
    target_compile_definitions(${PROJECT_NAME} PRIVATE BOOST_TEST_DYN_LINK)

    if(NOT DEFINED BOOST_ROOT)
        if(DEFINED ENV{BOOST_ROOT})
            set(BOOST_ROOT "$ENV{BOOST_ROOT}")
        elseif(Boost_INCLUDE_DIRS)
            list(GET Boost_INCLUDE_DIRS 0 _boost_inc)
            get_filename_component(BOOST_ROOT "${_boost_inc}" DIRECTORY)
        else()
            message(FATAL_ERROR "BOOST_ROOT not set.")
        endif()
    endif()

    if(CMAKE_BUILD_TYPE STREQUAL "Debug")
        set(BOOST_DLL_PATH "${BOOST_ROOT}/debug/bin")
    else()
        set(BOOST_DLL_PATH "${BOOST_ROOT}/bin")
    endif()
    file(GLOB BOOST_TEST_DLL "${BOOST_DLL_PATH}/boost_unit_test_framework*.dll")
    foreach(dll IN LISTS BOOST_TEST_DLL)
        add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${dll}"
                $<TARGET_FILE_DIR:${PROJECT_NAME}>
            COMMENT "Copying Boost.Test DLL to output directory"
        )
    endforeach()
endif()

enable_testing()
add_test(NAME ${PROJECT_NAME} COMMAND ${PROJECT_NAME})
