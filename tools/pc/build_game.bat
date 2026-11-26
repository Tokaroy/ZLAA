@echo off
rem ZLAA Builder: builds ZLAA's game code from your own Link's Awakening dump.
rem Double-click it and choose your dump's folder, or drop that folder onto this file.
title ZLAA Builder
if not exist "%~dp0python\python.exe" (
    echo This folder is missing ZLAA Builder's other files: it was probably started from inside the zip.
    echo Unpack the whole zip first ^(right-click it, Extract All^), then start Build Game.bat in the unpacked folder.
    echo.
    pause
    exit /b 1
)
"%~dp0python\python.exe" "%~dp0builder\zlaa_build.py" %*
echo.
pause
