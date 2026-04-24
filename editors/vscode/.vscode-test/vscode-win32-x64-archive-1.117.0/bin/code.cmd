@echo off
setlocal
set VSCODE_DEV=
set ELECTRON_RUN_AS_NODE=1
"%~dp0..\Code.exe" "%~dp0..\10c8e557c8\resources\app\out\cli.js" %*
IF %ERRORLEVEL% NEQ 0 EXIT /b %ERRORLEVEL%
endlocal
