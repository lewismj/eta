cmake_minimum_required(VERSION 3.28)

if(NOT DEFINED DEST_DIR OR "${DEST_DIR}" STREQUAL "")
    message(FATAL_ERROR "DEST_DIR is required")
endif()

foreach(dll IN LISTS DLLS)
    if(EXISTS "${dll}")
        execute_process(
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${dll}" "${DEST_DIR}"
            COMMAND_ERROR_IS_FATAL ANY
        )
    endif()
endforeach()
