@echo off
rem ── Eta Release Builder ────────────────────────────────────────────
rem Thin wrapper that launches build-release.ps1 with -ExecutionPolicy
rem Bypass so the script runs regardless of the system execution policy.
rem
rem Usage:
rem   build-release.cmd
rem   build-release.cmd -Version v0.3.0
rem   build-release.cmd -VcpkgRoot C:\src\vcpkg
rem   build-release.cmd "C:\eta-release" -VcpkgRoot C:\src\vcpkg
rem   build-release.cmd -EnableTorch
rem
rem VcpkgRoot can also be set via the VCPKG_ROOT environment variable.
rem -EnableTorch builds with libtorch bindings (CPU or CUDA).
rem ────────────────────────────────────────────────────────────────────

setlocal
set "SCRIPT_DIR=%~dp0"
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%SCRIPT_DIR%build-release.ps1" %*
