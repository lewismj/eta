# ──────────────────────────────────────────────────────────────────────────
# FetchEigen.cmake — Fetch Eigen 3.4.0 (header-only) via FetchContent
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
    GIT_TAG        3.4.0
    GIT_SHALLOW    TRUE
)

# Disable Eigen's own build artifacts — we only need the headers.
set(EIGEN_BUILD_DOC       OFF CACHE BOOL "" FORCE)
set(EIGEN_BUILD_TESTING   OFF CACHE BOOL "" FORCE)
set(BUILD_TESTING         OFF CACHE BOOL "" FORCE)
set(EIGEN_BUILD_PKGCONFIG OFF CACHE BOOL "" FORCE)

FetchContent_MakeAvailable(eigen)

message(STATUS "Eigen 3.4.0 fetched — Eigen3::Eigen target available")

