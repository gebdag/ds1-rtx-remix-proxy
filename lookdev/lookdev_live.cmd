@echo off
:: Opens the Remix window that follows the running lookdev game frame by frame.
:: Run it again to restart the host (e.g. after a proxy build).
:: In the Remix window: F4 proxy menu, Alt+X Remix menu.
python "%~dp0lookdev.py" host --live
if errorlevel 1 pause
