@echo off
:: Opens a saved capture in the plain D3D9 viewer: 1 replay, 2 vanilla, 3 difference.
setlocal
set "NAME=%~1"
if not "%NAME%"=="" goto run
python "%~dp0lookdev.py" list
echo.
set /p "NAME=Capture to view (Enter = newest): "
:run
python "%~dp0lookdev.py" view %NAME%
if errorlevel 1 pause
