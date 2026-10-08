@echo off
rem OLAS 1.1 launcher.
rem
rem Looks for olas_win.exe next to this file (release layout) or in build\
rem (source layout). No -m is passed, so the app picks the model from
rem olas-model.txt and the arch defaults: Medium English + Small Spanish
rem (Normal mode) or Small for both (Potato mode).
setlocal
cd /d "%~dp0"

set "EXE=%~dp0olas_win.exe"
if not exist "%EXE%" set "EXE=%~dp0build\olas_win.exe"
if not exist "%EXE%" (
    echo.
    echo OLAS: olas_win.exe not found.
    echo   Looked in this folder and in build\.
    echo   Build it first:  setup.ps1 then cmake, per README.md
    echo.
    pause
    exit /b 1
)

start "" "%EXE%" -l en,es
endlocal
