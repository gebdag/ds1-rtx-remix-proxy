@echo off
setlocal
:: Fresh-install shader caches for Dark Souls (Steam app 211420), to reproduce first-boot hitches.
::
::   cold_cache.cmd on      move the caches aside: the next launch compiles everything like a new install
::                          (run it again before each further cold launch: it clears what the last one built)
::   cold_cache.cmd off     delete what the cold launches built and put the original caches back
::   cold_cache.cmd status  show which state it is in
::
:: The caches are Steam's shader cache folders for the game: DXVK_state_cache (the DXVK state cache and the
:: runtime's persistent pipeline cache) and nvidiav1 (the NVIDIA driver's cache). "on" renames them to
:: *.bak-coldtest and leaves an empty DXVK_state_cache. A cache folder is only ever deleted while its
:: *.bak-coldtest original exists beside it, so the originals cannot be deleted by this script.
:: Refuses while DARKSOULS.exe is running.

set "S=<steam dir>\steamapps\shadercache\211420"
set "BAK=.bak-coldtest"
:: Windows' own tools by full path: a shell with Git or MSYS first on PATH has another find.exe.
set "TASKLIST=%SystemRoot%\System32\tasklist.exe"
set "FIND=%SystemRoot%\System32\find.exe"

if not exist "%S%\" goto :no_folder
if /i "%~1"=="on" goto :on
if /i "%~1"=="off" goto :off
if /i "%~1"=="status" goto :status
echo Usage: %~nx0 on ^| off ^| status
exit /b 1

:running_check
"%TASKLIST%" /FI "IMAGENAME eq DARKSOULS.exe" /NH 2>nul | "%FIND%" /i "DARKSOULS.exe" >nul
if errorlevel 1 exit /b 0
echo DARKSOULS.exe is running. Close the game first. Nothing changed.
exit /b 1

:on
call :running_check || exit /b 1
if exist "%S%\DXVK_state_cache%BAK%\" goto :on_again
if not exist "%S%\DXVK_state_cache\" mkdir "%S%\DXVK_state_cache"
ren "%S%\DXVK_state_cache" "DXVK_state_cache%BAK%" || goto :on_failed
if not exist "%S%\nvidiav1\" goto :on_done
ren "%S%\nvidiav1" "nvidiav1%BAK%" && goto :on_done
:: The driver cache could not be moved: put the first one back, so nothing is half done.
ren "%S%\DXVK_state_cache%BAK%" "DXVK_state_cache"
goto :on_failed

:on_done
mkdir "%S%\DXVK_state_cache"
echo Cold: original caches kept as *%BAK%. The next launch is a fresh install.
exit /b 0

:on_again
:: Already cold: clear what the last cold launch built, so the next one is cold again.
call :clear_test_caches || goto :failed
mkdir "%S%\DXVK_state_cache"
echo Cold again: the last cold launch's caches deleted. Originals still kept as *%BAK%.
exit /b 0

:off
call :running_check || exit /b 1
if not exist "%S%\DXVK_state_cache%BAK%\" goto :not_cold
call :clear_test_caches || goto :failed
ren "%S%\DXVK_state_cache%BAK%" "DXVK_state_cache" || goto :failed
if exist "%S%\nvidiav1%BAK%\" ren "%S%\nvidiav1%BAK%" "nvidiav1" || goto :failed
echo Warm: original caches restored.
exit /b 0

:clear_test_caches
:: Each folder only while its original is kept beside it: then the folder is one a cold launch built.
if not exist "%S%\DXVK_state_cache%BAK%\" goto :clear_nvidia
if exist "%S%\DXVK_state_cache\" rd /s /q "%S%\DXVK_state_cache"
if exist "%S%\DXVK_state_cache\" exit /b 1
:clear_nvidia
if not exist "%S%\nvidiav1%BAK%\" exit /b 0
if exist "%S%\nvidiav1\" rd /s /q "%S%\nvidiav1"
if exist "%S%\nvidiav1\" exit /b 1
exit /b 0

:status
if exist "%S%\DXVK_state_cache%BAK%\" (echo State: COLD - originals kept as *%BAK%) else (echo State: WARM - original caches in place)
dir /b "%S%"
exit /b 0

:not_cold
echo Not cold: no *%BAK% caches to restore. Nothing changed.
exit /b 0

:no_folder
echo Steam shader cache folder not found: "%S%"
exit /b 1

:on_failed
echo Could not move the caches aside (a file in use?). Nothing changed.
exit /b 1

:failed
echo Failed part way. Check "%S%": the originals are the *%BAK% folders, if any are there.
exit /b 1
