# ──────────────────────────────────────────────────────────────────────────
# FetchEigen.cmake — Fetch Eigen 3.4.0 (header-only) via FetchContent
#
# Called by eta/CMakeLists.txt when ETA_BUILD_STATS is ON.
# Downloads Eigen from GitLab and makes the `Eigen3::Eigen` target available.
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

