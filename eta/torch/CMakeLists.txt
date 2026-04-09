cmake_minimum_required(VERSION 3.28)
project(eta_torch LANGUAGES CXX)

# Torch was already found by the parent CMakeLists (eta/CMakeLists.txt)

add_library(${PROJECT_NAME} INTERFACE)

target_include_directories(${PROJECT_NAME} INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

# Mark libtorch headers as SYSTEM so the compiler suppresses warnings
# from third-party code (e.g. MSVC C4267 inside ATen headers).
target_include_directories(${PROJECT_NAME} SYSTEM INTERFACE
    ${TORCH_INCLUDE_DIRS}
)

# On MSVC, libtorch template instantiations trigger C4244 (narrowing
# conversion) and C4267 (size_t → int) warnings deep inside the MSVC STL
# headers (<optional>, <xutility>, <vector>, <utility>).  Because the
# warnings are emitted in STL code — not in the libtorch headers themselves
# — the /external:I + /external:W0 mechanism cannot suppress them.
# Disable these two warnings for every TU that consumes eta_torch.
if(MSVC)
    target_compile_options(${PROJECT_NAME} INTERFACE /wd4244 /wd4267)
endif()

target_link_libraries(${PROJECT_NAME} INTERFACE
    eta_core
    ${TORCH_LIBRARIES}
)

# Propagate torch CXX flags (e.g. -D_GLIBCXX_USE_CXX11_ABI)
target_compile_options(${PROJECT_NAME} INTERFACE ${TORCH_CXX_FLAGS})

# ── Install libtorch runtime libraries alongside Eta binaries ────────
# On Windows, DLLs must sit next to the executables (bin/).
# On Linux/macOS, shared libraries go to lib/ (RPATH is set in the
# parent eta/CMakeLists.txt before targets are created).
if(TORCH_INSTALL_PREFIX)
    if(WIN32)
        file(GLOB _torch_runtime_dlls "${TORCH_INSTALL_PREFIX}/lib/*.dll")
        install(FILES ${_torch_runtime_dlls} DESTINATION bin)
    else()
        file(GLOB _torch_runtime_so "${TORCH_INSTALL_PREFIX}/lib/*.so*"
                                     "${TORCH_INSTALL_PREFIX}/lib/*.dylib*")
        install(FILES ${_torch_runtime_so} DESTINATION lib)
    endif()
endif()


