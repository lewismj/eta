cmake_minimum_required(VERSION 3.28)
project(eta_core LANGUAGES CXX)

set(CMAKE_CXX_SCAN_FOR_MODULES OFF)

add_library(${PROJECT_NAME} STATIC)

target_include_directories(${PROJECT_NAME}
        PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src
        PUBLIC ${Boost_INCLUDE_DIRS}
        PRIVATE ${PROJECT_SOURCE_DIR}
)

target_sources(${PROJECT_NAME}
        PUBLIC
        src/eta/runtime/memory/intern_table.cpp
        src/eta/runtime/memory/heap.cpp
        src/eta/runtime/memory/mark_sweep_gc.cpp
        src/eta/reader/lexer.cpp
        src/eta/reader/parser.cpp
        src/eta/reader/expander.cpp
        src/eta/reader/module_linker.cpp
        src/eta/semantics/semantic_analyzer.cpp
        src/eta/semantics/emitter.cpp
        src/eta/runtime/vm/vm.cpp
        src/eta/runtime/vm/bytecode_serializer.cpp
)

# BMI2/PEXT support check
if(NOT MSVC)
	include(CheckCXXSourceCompiles)

    set(CMAKE_REQUIRED_FLAGS "-mbmi2")
    set(CMAKE_CXX_SCAN_FOR_MODULES_OLD ${CMAKE_CXX_SCAN_FOR_MODULES})
    set(CMAKE_CXX_SCAN_FOR_MODULES OFF)

    check_cxx_source_compiles("
        #include <immintrin.h>
        int main() {
            unsigned long long src = 0b10101010, mask = 0b1111;
            unsigned long long res = _pext_u64(src, mask);
            (void)res;
            return 0;
        }" PEXT_SUPPORTED)

    set(CMAKE_CXX_SCAN_FOR_MODULES ${CMAKE_CXX_SCAN_FOR_MODULES_OLD})

    if(PEXT_SUPPORTED)
        message(STATUS "PEXT instruction is supported.")
        target_compile_definitions(${PROJECT_NAME} PRIVATE USE_PEXT)
        target_compile_options(${PROJECT_NAME} PRIVATE "-mbmi2")
        target_link_options(${PROJECT_NAME} PRIVATE "-mbmi2")
    else()
        message(STATUS "PEXT instruction is not supported.")
    endif()
endif()


if(CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    message(STATUS "Clang C++ compiler is being used.")
elseif(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    message(STATUS "GCC C++ compiler is being used.")
elseif(CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
    message(STATUS "MSVC C++ compiler is being used.")
else()
    message(STATUS "Unknown C++ compiler is being used: ${CMAKE_CXX_COMPILER_ID}")
endif()

# Prevent <windows.h> from injecting min/max macros that break std::min/std::max.
# PUBLIC so every target that links eta_core inherits these definitions.
if(MSVC)
    target_compile_definitions(${PROJECT_NAME} PUBLIC
        NOMINMAX
        WIN32_LEAN_AND_MEAN
    )
endif()

if(CMAKE_BUILD_TYPE MATCHES "^[Rr]elease")
    if(MSVC)
        message(STATUS "MSVC Release build: using /O2")
        target_compile_options(${PROJECT_NAME} PRIVATE /O2)
    else()
        message(STATUS "GCC/Clang Release build: using -O3")
        target_compile_options(${PROJECT_NAME} PRIVATE -O3)
    endif()
endif()

# Enforce exhaustiveness on enum switches for Clang/GCC
if (NOT MSVC)
    target_compile_options(${PROJECT_NAME} PRIVATE -Wswitch-enum)
endif()

install(TARGETS ${PROJECT_NAME}
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        PUBLIC_HEADER DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})


# --- Fuzzing support (Clang/LLVM only, not Windows) ---
if (CMAKE_CXX_COMPILER_ID STREQUAL "Clang" AND NOT MSVC)
    add_library(${PROJECT_NAME}_sanitized)

    target_sources(${PROJECT_NAME}_sanitized
            PUBLIC
            src/eta/reader/expander.cpp
            src/eta/reader/parser.cpp
            src/eta/reader/lexer.cpp
            src/eta/runtime/memory/mark_sweep_gc.cpp
            src/eta/runtime/memory/intern_table.cpp
            src/eta/runtime/memory/heap.cpp
    )

    target_include_directories(${PROJECT_NAME}_sanitized
            PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src
    )

    target_include_directories(${PROJECT_NAME}_sanitized PRIVATE ${PROJECT_SOURCE_DIR} ${PROJECT_SOURCE_DIR}/src)

    target_compile_definitions(${PROJECT_NAME}_sanitized PUBLIC _POSIX_C_SOURCE=200112L)

    target_compile_options(${PROJECT_NAME}_sanitized PRIVATE
            -fsanitize=address
            -fno-omit-frame-pointer
            -g -O1
    )

    target_compile_options(${PROJECT_NAME}_sanitized PRIVATE -Wswitch-enum)

    target_link_options(${PROJECT_NAME}_sanitized PRIVATE -fsanitize=address)
endif()


