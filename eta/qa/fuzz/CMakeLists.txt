cmake_minimum_required(VERSION 3.28)
project(eta_core_fuzz LANGUAGES CXX)

# Only build fuzzers with Clang (LibFuzzer requirement)
if(NOT CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    message(STATUS "Fuzzing targets require Clang, skipping qa/fuzz directory")
    return()
endif()

message(STATUS "Building fuzzing targets")

# Prefer the sanitized core when available; fall back to normal core
if(TARGET eta_core_sanitized)
    set(ETA_CORE_FOR_FUZZ eta_core_sanitized)
else()
    set(ETA_CORE_FOR_FUZZ eta_core)
endif()


# Common fuzzer flags - use only fuzzer and address sanitizers
# UBSan causes linking issues with C++ modules and standard library
set(FUZZ_COMPILE_FLAGS
        -fsanitize=fuzzer,address
        -fno-omit-frame-pointer
        -g
        -O1
)

set(FUZZ_LINK_FLAGS
        -fsanitize=fuzzer,address
)

# Helper function to create fuzz targets with standardized corpus directories
function(add_fuzz_target TARGET_NAME SOURCE_FILE CORPUS_NAME)

    add_executable(${TARGET_NAME} ${SOURCE_FILE})

    target_compile_options(${TARGET_NAME} PRIVATE ${FUZZ_COMPILE_FLAGS})
    target_link_options(${TARGET_NAME} PRIVATE ${FUZZ_LINK_FLAGS})

    # Link against the selected core library (sanitized if available)
    target_link_libraries(${TARGET_NAME} PRIVATE ${ETA_CORE_FOR_FUZZ})

    # Ensure the core library is built before this fuzzer
    add_dependencies(${TARGET_NAME} ${ETA_CORE_FOR_FUZZ})

    # Create standardized corpus directory (e.g., corpus/nanbox, corpus/intern_table)
    set(CORPUS_DIR ${CMAKE_CURRENT_SOURCE_DIR}/corpus/${CORPUS_NAME})
    file(MAKE_DIRECTORY ${CORPUS_DIR})

    # Copy sample seeds if they exist (place your seed files under qa/fuzz/samples/<CORPUS_NAME>)
    set(SAMPLE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/samples/${CORPUS_NAME})
    if(EXISTS ${SAMPLE_DIR})
        file(GLOB SAMPLE_SEEDS ${SAMPLE_DIR}/*)
        if(SAMPLE_SEEDS)
            file(COPY ${SAMPLE_SEEDS} DESTINATION ${CORPUS_DIR})
            message(STATUS "Seeded corpus for ${CORPUS_NAME} with samples from ${SAMPLE_DIR}")
        endif()
    else()
        message(STATUS "No sample seeds found at ${SAMPLE_DIR}; corpus will start empty")
    endif()

    message(STATUS "Added fuzz target: ${TARGET_NAME} -> corpus/${CORPUS_NAME}")
endfunction()

# Find threading library for multi-threaded fuzzers
find_package(Threads REQUIRED)

# Define all fuzz targets
# Syntax: add_fuzz_target(<executable_name> <source_file> <corpus_directory_name>)
add_fuzz_target(eta_fuzz_nanbox src/fuzz_nanbox.cpp nanbox)
add_fuzz_target(eta_fuzz_intern_table src/fuzz_intern_table.cpp intern_table)
target_link_libraries(eta_fuzz_intern_table PRIVATE Threads::Threads)
add_fuzz_target(eta_fuzz_heap src/fuzz_heap.cpp heap)
