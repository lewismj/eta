include(FetchContent)

set(ETA_SPDLOG_TAG "v1.17.0" CACHE STRING "spdlog version")

# Prefer an already-installed package when available.
find_package(spdlog CONFIG QUIET)
if(TARGET spdlog::spdlog)
    message(STATUS "Using preinstalled spdlog")
    return()
endif()

# Third-party extras are not needed for Eta builds.
set(SPDLOG_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(SPDLOG_BUILD_EXAMPLE OFF CACHE BOOL "" FORCE)
set(SPDLOG_BUILD_EXAMPLE_HO OFF CACHE BOOL "" FORCE)
set(SPDLOG_BUILD_BENCH OFF CACHE BOOL "" FORCE)
set(SPDLOG_INSTALL OFF CACHE BOOL "" FORCE)
set(SPDLOG_FMT_EXTERNAL OFF CACHE BOOL "" FORCE)

# On Windows, build shared so the runtime DLL can be copied/installed beside executables.
if(WIN32)
    set(SPDLOG_BUILD_SHARED ON CACHE BOOL "" FORCE)
endif()

FetchContent_Declare(
    spdlog
    GIT_REPOSITORY https://github.com/gabime/spdlog.git
    GIT_TAG        ${ETA_SPDLOG_TAG}
    EXCLUDE_FROM_ALL
    SYSTEM
)

FetchContent_MakeAvailable(spdlog)

if(NOT TARGET spdlog::spdlog)
    message(FATAL_ERROR "spdlog fetch failed: spdlog::spdlog target missing")
endif()

message(STATUS "spdlog fetched")
