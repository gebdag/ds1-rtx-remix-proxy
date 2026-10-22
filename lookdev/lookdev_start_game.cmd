@echo off
:: Starts Dark Souls in lookdev mode (vanilla rendering + capture/stream).
:: In game: F8 freezes input, F9 captures a frame.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0lookdev.ps1"
if errorlevel 1 pause
