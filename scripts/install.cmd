@echo off
rem ── Eta Windows Installer ──────────────────────────────────────────
rem Thin wrapper that launches install.ps1 with -ExecutionPolicy Bypass
rem so users don't have to change their system execution policy.
rem
rem Usage:
rem   install.cmd                        (use bundle dir)
rem   install.cmd "C:\Program Files\Eta" (install to prefix)
rem ────────────────────────────────────────────────────────────────────

setlocal
set "SCRIPT_DIR=%~dp0"
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%SCRIPT_DIR%install.ps1" %*

