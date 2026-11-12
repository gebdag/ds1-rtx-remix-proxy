@echo off
:: Builds test_game_build.exe (x86, like the proxy) into %TEMP%\test_game_build.
setlocal
set "HERE=%~dp0"
set "OUT=%TEMP%\test_game_build"
if not exist "%OUT%" mkdir "%OUT%"
for %%v in (
    "%ProgramFiles%\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvarsall.bat"
    "%ProgramFiles%\Microsoft Visual Studio\2022\Professional\VC\Auxiliary\Build\vcvarsall.bat"
    "%ProgramFiles%\Microsoft Visual Studio\2022\Enterprise\VC\Auxiliary\Build\vcvarsall.bat"
    "%ProgramFiles%\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvarsall.bat"
    "%ProgramFiles(x86)%\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvarsall.bat"
) do if exist %%v ( call %%v x86 >nul & goto :have_vs )
echo vcvarsall.bat not found & exit /b 1
:have_vs
cl /nologo /std:c++latest /EHsc /O2 /W4 /I"%HERE%." /Fo"%OUT%\\" /Fe"%OUT%\test_game_build.exe" ^
    "%HERE%test_game_build.cpp" "%HERE%..\..\src\comp\game\game_build.cpp"
