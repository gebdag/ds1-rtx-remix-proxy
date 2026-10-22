@echo off
:: Opens a saved capture in the Remix host (one frame, replayed through the proxy and Remix).
setlocal
set "NAME=%~1"
if not "%NAME%"=="" goto run
python "%~dp0lookdev.py" list
echo.
set /p "NAME=Capture to replay in Remix (Enter = newest): "
:run
python "%~dp0lookdev.py" host %NAME%
if errorlevel 1 pause
