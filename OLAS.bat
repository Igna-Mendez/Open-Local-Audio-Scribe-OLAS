@echo off
rem OLAS 1.1 launcher.
rem
rem Windows cannot attach an icon to a .bat file -- the shell draws batch files
rem with the generic prompt glyph. To get the OLAS wave on the desktop and in
rem the taskbar, this script creates a .lnk shortcut pointing at olas_win.exe
rem with OLAS.ico, once, and uses it from then on. The shortcut is what you
rem pin or launch; this file stays as the thing you can double-click straight
rem out of the zip.
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

rem ---- self-iconising shortcut -------------------------------------------
rem Written once, next to the app, and launched instead of the exe so the
rem taskbar and alt-tab entry carry the icon. Best-effort: if the shortcut
rem cannot be made, fall back to running the exe directly.
set "LNK=%~dp0OLAS.lnk"
set "ICO=%~dp0OLAS.ico"

if not exist "%LNK%" if exist "%ICO%" (
    set "TMPVBS=%TEMP%\olas_mklnk.vbs"
    > "%TMPVBS%" echo Set s = CreateObject("WScript.Shell")
    >>"%TMPVBS%" echo Set l = s.CreateShortcut("%LNK%")
    >>"%TMPVBS%" echo l.TargetPath = "%EXE%"
    >>"%TMPVBS%" echo l.WorkingDirectory = "%~dp0"
    >>"%TMPVBS%" echo l.IconLocation = "%ICO%"
    >>"%TMPVBS%" echo l.Arguments = "-l en,es"
    >>"%TMPVBS%" echo l.Save
    cscript //nologo "%TMPVBS%" >nul 2>&1
    del "%TMPVBS%" >nul 2>&1
)

if exist "%LNK%" (
    start "" "%LNK%"
) else (
    start "" "%EXE%" -l en,es
)
endlocal
