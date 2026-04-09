cmake_minimum_required(VERSION 3.28)
project(eta_torch LANGUAGES CXX)

# Torch was already found by the parent CMakeLists (eta/CMakeLists.txt)

add_library(${PROJECT_NAME} INTERFACE)

target_include_directories(${PROJECT_NAME} INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${TORCH_INCLUDE_DIRS}
)

target_link_libraries(${PROJECT_NAME} INTERFACE
    eta_core
    ${TORCH_LIBRARIES}
)

# Propagate torch CXX flags (e.g. -D_GLIBCXX_USE_CXX11_ABI)
target_compile_options(${PROJECT_NAME} INTERFACE ${TORCH_CXX_FLAGS})

