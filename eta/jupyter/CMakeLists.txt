cmake_minimum_required(VERSION 3.28)
project(eta_jupyter LANGUAGES CXX)

add_executable(eta_jupyter
    src/eta/jupyter/main_jupyter.cpp
    src/eta/jupyter/eta_interpreter.cpp
    src/eta/jupyter/kernel_config.cpp
    src/eta/jupyter/display.cpp
    src/eta/jupyter/magics.cpp
    src/eta/jupyter/comm/heap_comm.cpp
    src/eta/jupyter/comm/disasm_comm.cpp
    src/eta/jupyter/comm/actors_comm.cpp
    src/eta/jupyter/comm/dag_comm.cpp
    src/eta/jupyter/comm/tensor_comm.cpp
    ../interpreter/src/eta/interpreter/repl_wrap.cpp
)

target_include_directories(eta_jupyter
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../interpreter/src
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../session/src
)

target_link_libraries(eta_jupyter
    PRIVATE eta_core eta_torch eta_nng eta_stats
            xeus xeus-zmq nlohmann_json::nlohmann_json
)

target_compile_definitions(eta_jupyter PRIVATE
    ETA_HAS_TORCH=1 ETA_HAS_NNG=1 ETA_HAS_STATS=1
    ETA_JUPYTER_SOURCE_RESOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/resources"
)

eta_copy_torch_dlls(eta_jupyter)
eta_copy_nng_dlls(eta_jupyter)

# xeus, xeus-zmq, and libzmq are shared in the FetchContent build and must be
# available next to eta_jupyter on Windows.
if(WIN32)
    foreach(_dep IN ITEMS xeus xeus-zmq libzmq)
        if(TARGET ${_dep})
            add_custom_command(TARGET eta_jupyter POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    $<TARGET_FILE:${_dep}>
                    $<TARGET_FILE_DIR:eta_jupyter>
                COMMENT "Copying ${_dep} DLL to eta_jupyter output dir"
            )
        endif()
    endforeach()
endif()

if(NOT MSVC)
    target_link_libraries(eta_jupyter PRIVATE stdc++ m)
endif()

install(TARGETS eta_jupyter RUNTIME DESTINATION bin)
install(FILES resources/kernel.json.in DESTINATION share/jupyter/kernels/eta)

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/resources/logo-32x32.png")
    install(FILES resources/logo-32x32.png DESTINATION share/jupyter/kernels/eta)
endif()
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/resources/logo-64x64.png")
    install(FILES resources/logo-64x64.png DESTINATION share/jupyter/kernels/eta)
endif()
