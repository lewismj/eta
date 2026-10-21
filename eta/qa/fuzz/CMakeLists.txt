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

# Helper function to create fuzz targets with standardized corpus directories.
# An optional fourth argument overrides the core library to link (default ETA_CORE_FOR_FUZZ).
function(add_fuzz_target TARGET_NAME SOURCE_FILE CORPUS_NAME)
    if(ARGC GREATER 3)
        set(CORE_LIB ${ARGV3})
    else()
        set(CORE_LIB ${ETA_CORE_FOR_FUZZ})
    endif()

    add_executable(${TARGET_NAME} ${SOURCE_FILE})

    target_compile_options(${TARGET_NAME} PRIVATE ${FUZZ_COMPILE_FLAGS})
    target_link_options(${TARGET_NAME} PRIVATE ${FUZZ_LINK_FLAGS})

    # Link against the selected core library (sanitized if available)
    target_link_libraries(${TARGET_NAME} PRIVATE ${CORE_LIB})

    # Ensure the core library is built before this fuzzer
    add_dependencies(${TARGET_NAME} ${CORE_LIB})

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
if(TARGET eta_cst_sanitized)
    add_library(eta_format_sanitized STATIC
        ../../tools/formatter/src/eta/format/doc.cpp
        ../../tools/formatter/src/eta/format/format_source.cpp
        ../../tools/formatter/src/eta/format/forms.cpp
        ../../tools/formatter/src/eta/format/layout.cpp)
    target_include_directories(eta_format_sanitized PUBLIC ../../tools/formatter/src)
    target_link_libraries(eta_format_sanitized PUBLIC eta_cst_sanitized)
    target_compile_options(eta_format_sanitized PRIVATE -fsanitize=address -fno-omit-frame-pointer -g -O1)
    set(ETA_FORMAT_FOR_FUZZ eta_format_sanitized)
else()
    set(ETA_FORMAT_FOR_FUZZ eta_format_lib)
endif()
add_fuzz_target(eta_fuzz_formatter src/fuzz_formatter.cpp formatter)
target_link_libraries(eta_fuzz_formatter PRIVATE ${ETA_FORMAT_FOR_FUZZ})
add_fuzz_target(eta_fuzz_cst src/fuzz_cst.cpp cst)
if(TARGET eta_cst_sanitized)
    target_link_libraries(eta_fuzz_cst PRIVATE eta_cst_sanitized)
else()
    target_link_libraries(eta_fuzz_cst PRIVATE eta_cst)
endif()

# Parsers of untrusted input. The parser under test is compiled into the fuzzer
# itself (header-only, or added as a source) so it gets coverage and ASan
# instrumentation; the rest of the runtime comes from the regular eta_core.
set(ETA_CORE_SRC_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../core/src)

add_fuzz_target(eta_fuzz_etac src/fuzz_etac.cpp etac eta_core)
target_sources(eta_fuzz_etac PRIVATE ${ETA_CORE_SRC_DIR}/eta/runtime/vm/bytecode_serializer.cpp)

add_fuzz_target(eta_fuzz_wire_format src/fuzz_wire_format.cpp wire_format eta_core)
target_include_directories(eta_fuzz_wire_format PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../../packages/stdlib/native/nng/src)

add_fuzz_target(eta_fuzz_json src/fuzz_json.cpp json)

add_fuzz_target(eta_fuzz_framing src/fuzz_framing.cpp framing)
target_sources(eta_fuzz_framing PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../tools/shared/src/eta/tooling/framing.cpp)
target_include_directories(eta_fuzz_framing PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../tools/shared/src)

# Every fuzz target, for `cmake --build --target eta_fuzz_all` and the fuzz workflows.
add_custom_target(eta_fuzz_all DEPENDS
    eta_fuzz_nanbox eta_fuzz_intern_table eta_fuzz_heap eta_fuzz_formatter eta_fuzz_cst
    eta_fuzz_etac eta_fuzz_wire_format eta_fuzz_json eta_fuzz_framing)
