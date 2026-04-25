include(FetchContent)

# Version pins for the xeus dependency stack.
set(ETA_XEUS_TAG          "5.1.1"  CACHE STRING "xeus version")
set(ETA_XEUS_ZMQ_TAG      "3.0.0"  CACHE STRING "xeus-zmq version")
set(ETA_LIBZMQ_TAG        "v4.3.5" CACHE STRING "libzmq version")
set(ETA_CPPZMQ_TAG        "v4.10.0" CACHE STRING "cppzmq version")
set(ETA_NLOHMANN_JSON_TAG "v3.11.3" CACHE STRING "nlohmann_json version")

# Prefer package-manager binaries when requested (recommended on Windows).
if(ETA_USE_VCPKG)
    find_package(nlohmann_json CONFIG REQUIRED)
    find_package(xeus CONFIG REQUIRED)
    find_package(xeus-zmq CONFIG REQUIRED)
    message(STATUS "Using vcpkg-provided xeus dependencies")
    return()
endif()

# Third-party tests/examples are not required for Eta builds.
set(ZMQ_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(ENABLE_CURVE    OFF CACHE BOOL "" FORCE)
set(WITH_DOCS       OFF CACHE BOOL "" FORCE)
set(CPPZMQ_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(XEUS_BUILD_TESTS   OFF CACHE BOOL "" FORCE)
set(XEUS_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(XEUS_ZMQ_BUILD_TESTS OFF CACHE BOOL "" FORCE)

FetchContent_Declare(
    libzmq
    GIT_REPOSITORY https://github.com/zeromq/libzmq.git
    GIT_TAG        ${ETA_LIBZMQ_TAG}
    EXCLUDE_FROM_ALL
    SYSTEM
)

FetchContent_Declare(
    cppzmq
    GIT_REPOSITORY https://github.com/zeromq/cppzmq.git
    GIT_TAG        ${ETA_CPPZMQ_TAG}
    EXCLUDE_FROM_ALL
    SYSTEM
)

FetchContent_Declare(
    nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG        ${ETA_NLOHMANN_JSON_TAG}
    EXCLUDE_FROM_ALL
    SYSTEM
)

FetchContent_Declare(
    xeus
    GIT_REPOSITORY https://github.com/jupyter-xeus/xeus.git
    GIT_TAG        ${ETA_XEUS_TAG}
    EXCLUDE_FROM_ALL
    SYSTEM
)

FetchContent_Declare(
    xeus_zmq
    GIT_REPOSITORY https://github.com/jupyter-xeus/xeus-zmq.git
    GIT_TAG        ${ETA_XEUS_ZMQ_TAG}
    EXCLUDE_FROM_ALL
    SYSTEM
)

# On Windows prefer shared libzmq to avoid import/export mismatches.
if(WIN32)
    if(DEFINED BUILD_SHARED_LIBS)
        set(_eta_had_BUILD_SHARED_LIBS TRUE)
        set(_eta_prev_BUILD_SHARED_LIBS "${BUILD_SHARED_LIBS}")
    else()
        set(_eta_had_BUILD_SHARED_LIBS FALSE)
    endif()
    set(BUILD_SHARED_LIBS ON)
endif()

# Order matters: libzmq -> cppzmq -> json -> xeus -> xeus-zmq.
FetchContent_MakeAvailable(libzmq cppzmq nlohmann_json xeus xeus_zmq)

if(WIN32)
    if(_eta_had_BUILD_SHARED_LIBS)
        set(BUILD_SHARED_LIBS "${_eta_prev_BUILD_SHARED_LIBS}")
    else()
        unset(BUILD_SHARED_LIBS)
    endif()
    unset(_eta_had_BUILD_SHARED_LIBS)
    unset(_eta_prev_BUILD_SHARED_LIBS)
endif()

if(NOT TARGET xeus)
    message(FATAL_ERROR "xeus fetch failed: xeus target missing")
endif()
if(NOT TARGET xeus-zmq)
    message(FATAL_ERROR "xeus-zmq fetch failed: xeus-zmq target missing")
endif()
if(NOT TARGET nlohmann_json::nlohmann_json)
    message(FATAL_ERROR "nlohmann_json fetch failed: nlohmann_json::nlohmann_json target missing")
endif()

message(STATUS "xeus dependency stack fetched")
