@echo off
:: Captures the game's next frame to DATA\rtx_comp\lookdev\<name>.ds1cap.
:: Same as F9 in game, but lets you name the capture.
setlocal
set "NAME=%~1"
if "%NAME%"=="" set /p "NAME=Capture name (Enter = timestamp): "
python "%~dp0lookdev.py" capture %NAME%
pause
