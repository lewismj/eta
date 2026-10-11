@echo off
rem Eta Release Builder
rem Thin wrapper that launches build-release.ps1 with -ExecutionPolicy
rem Bypass so the script runs regardless of the system execution policy.
rem
rem Usage:
rem   build-release.cmd
rem   build-release.cmd -Version v0.3.0
rem   build-release.cmd -VcpkgRoot C:\src\vcpkg
rem   build-release.cmd "C:\eta-release" -VcpkgRoot C:\src\vcpkg
rem   build-release.cmd -EnableTorch
rem   set ETA_INCLUDE_HTTP=1 && build-release.cmd
rem
rem VcpkgRoot can also be set via the VCPKG_ROOT environment variable.
rem -EnableTorch builds with libtorch bindings (CPU or CUDA).
setlocal EnableExtensions EnableDelayedExpansion
set "SCRIPT_DIR=%~dp0"
set "_PS_ARGS="

:next_arg
if "%~1"=="" goto run_ps

set "_ARG=%~1"
if "!_ARG:~0,1!"=="-" (
	set "_KEY="
	set "_VAL="
	for /f "tokens=1* delims==" %%A in ("!_ARG!") do (
		set "_KEY=%%~A"
		set "_VAL=%%~B"
	)
	if defined _VAL (
		set "_PS_ARGS=!_PS_ARGS! "!_KEY!" "!_VAL!""
	) else (
		set "_PS_ARGS=!_PS_ARGS! "%~1""
	)
) else (
	set "_PS_ARGS=!_PS_ARGS! "%~1""
)
shift
goto next_arg

:run_ps
call powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%SCRIPT_DIR%build-release.ps1" %_PS_ARGS%
exit /b %ERRORLEVEL%
