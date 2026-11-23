@echo off
rem ZLAA Builder: builds ZLAA's game code from your own Link's Awakening dump.
rem Double-click it and choose your dump's folder, or drop that folder onto this file.
title ZLAA Builder
"%~dp0python\python.exe" "%~dp0builder\zlaa_build.py" %*
echo.
pause
