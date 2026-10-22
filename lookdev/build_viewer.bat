@echo off
setlocal enabledelayedexpansion

:: Builds the x64 lookdev viewer: lookdev\bin\ds1_lookdev_viewer.exe
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
call "%VCVARS%" x64 >nul 2>&1
if errorlevel 1 ( echo ERROR: vcvarsall.bat failed & exit /b 1 )

mkdir "%ROOT%bin" 2>nul
mkdir "%ROOT%obj" 2>nul

cl /nologo /std:c++latest /W4 /WX /O2 /MT /EHsc /Zi ^
    /DUNICODE /D_UNICODE /D_CRT_SECURE_NO_WARNINGS ^
    /Fo"%ROOT%obj\\" /Fd"%ROOT%obj\\" ^
    "%ROOT%viewer\main.cpp" ^
    /Fe"%ROOT%bin\ds1_lookdev_viewer.exe" ^
    /link /DEBUG /PDB:"%ROOT%bin\ds1_lookdev_viewer.pdb" user32.lib d3d9.lib
if errorlevel 1 (
    echo === Viewer build FAILED ===
    exit /b 1
)
echo === Viewer build succeeded: %ROOT%bin\ds1_lookdev_viewer.exe ===
exit /b 0
