# ──────────────────────────────────────────────────────────────────────────
# FetchEigen.cmake — Fetch Eigen 5.0.0 (header-only) via FetchContent
#
# Called unconditionally by eta/CMakeLists.txt.  Eigen is a required
# dependency — eta_core links it PUBLIC so all downstream targets
# (eta_stats, interpreter, tests, etc.) inherit the include paths.
#
# Eigen is header-only — no shared/static library is produced, so no
# DLL-copy or RPATH plumbing is needed.
# ──────────────────────────────────────────────────────────────────────────

include(FetchContent)

FetchContent_Declare(
    eigen
    GIT_REPOSITORY https://gitlab.com/libeigen/eigen.git
    GIT_TAG        5.0.0
    GIT_SHALLOW    TRUE
    # EXCLUDE_FROM_ALL (CMake ≥ 3.28) keeps Eigen out of the default `all`
    # target AND — importantly — disables Eigen's own install() rules so
    # we don't end up with include/, lib/cmake/eigen3/, share/eigen3/cmake/
    # polluting the release bundle.  Eigen is header-only and consumed
    # purely through the imported Eigen3::Eigen target.
    EXCLUDE_FROM_ALL
    SYSTEM
)

# Disable Eigen's own build artifacts — we only need the headers.
set(EIGEN_BUILD_DOC       OFF CACHE BOOL "" FORCE)
set(EIGEN_BUILD_TESTING   OFF CACHE BOOL "" FORCE)
set(BUILD_TESTING         OFF CACHE BOOL "" FORCE)
set(EIGEN_BUILD_PKGCONFIG OFF CACHE BOOL "" FORCE)

# Silence deprecated-policy noise from Eigen's own CMake while fetching.
set(_eta_prev_cmake_warn_deprecated "${CMAKE_WARN_DEPRECATED}")
set(CMAKE_WARN_DEPRECATED OFF)

FetchContent_MakeAvailable(eigen)

set(CMAKE_WARN_DEPRECATED "${_eta_prev_cmake_warn_deprecated}")
unset(_eta_prev_cmake_warn_deprecated)

message(STATUS "Eigen 5.0.0 fetched — Eigen3::Eigen target available")

