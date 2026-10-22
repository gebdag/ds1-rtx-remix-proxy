@echo off
setlocal enabledelayedexpansion

:: Builds the lookdev tools into lookdev\bin:
::   ds1_lookdev_viewer.exe  x64  plain-D3D9 replay + pixel comparison
::   ds1_lookdev_host.exe    x86  replay through the proxy and RTX Remix
:: Independent of build.bat; never touches the game folder.

set "ROOT=%~dp0"

set "VCVARS="
for %%p in (
    "%ProgramFiles%\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvarsall.bat"
    "%ProgramFiles%\Microsoft Visual Studio\2022\Professional\VC\Auxiliary\Build\vcvarsall.bat"
    "%ProgramFiles%\Microsoft Visual Studio\2022\Enterprise\VC\Auxiliary\Build\vcvarsall.bat"
    "%ProgramFiles%\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvarsall.bat"
) do (
    if exist %%p set "VCVARS=%%~p"
)
if "%VCVARS%"=="" (
    echo ERROR: Visual Studio 2022 not found
    exit /b 1
)

mkdir "%ROOT%bin" 2>nul
set "CF=/nologo /std:c++latest /W4 /WX /O2 /MT /EHsc /Zi /DUNICODE /D_UNICODE /D_CRT_SECURE_NO_WARNINGS"

:: Each architecture in its own environment (vcvarsall cannot be switched in place).
call :build x64 viewer ds1_lookdev_viewer "user32.lib d3d9.lib"
if errorlevel 1 goto :fail
call :build x86 host ds1_lookdev_host "user32.lib"
if errorlevel 1 goto :fail

echo === Lookdev build succeeded: %ROOT%bin ===
exit /b 0

:build
setlocal
call "%VCVARS%" %1 >nul 2>&1
if errorlevel 1 ( echo ERROR: vcvarsall %1 failed & exit /b 1 )
mkdir "%ROOT%obj\%2" 2>nul
echo [%1] %3
cl %CF% /Fo"%ROOT%obj\%2\\" /Fd"%ROOT%obj\%2\\" "%ROOT%%2\main.cpp" /Fe"%ROOT%bin\%3.exe" ^
    /link /DEBUG /PDB:"%ROOT%bin\%3.pdb" %~4
if errorlevel 1 exit /b 1
endlocal
exit /b 0

:fail
echo === Lookdev build FAILED ===
exit /b 1
