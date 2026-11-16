@echo off
setlocal enabledelayedexpansion

:: remix-comp-proxy build script (d3d9.dll proxy)
:: Usage: build.bat [release|debug] [--name OutputName] [--comp CompDir] [--no-deploy]
::
:: --no-deploy leaves the game folder alone (deploy.conf is ignored), e.g. to
:: build for the lookdev host while the game holds d3d9.dll.
::
:: Examples:
::   build.bat                          Build release d3d9.dll
::   build.bat debug                    Build debug d3d9.dll
::   build.bat release --name Warband   Build release d3d9.dll (Warband variant)
::   build.bat release --name Warband --comp ..\..\patches\Warband\proxy\comp
::                                      Build per-game variant from custom comp dir

set "ROOT=%~dp0"
set "CONFIG=release"
set "NAME=remix-comp-proxy"
set "COMP_DIR=%ROOT%src\comp"
set "CUSTOM_COMP=0"
set "NO_DEPLOY=0"

:: Parse args
:parse_args
if "%~1"=="" goto :args_done
if /i "%~1"=="release" ( set "CONFIG=release" & shift & goto :parse_args )
if /i "%~1"=="debug"   ( set "CONFIG=debug"   & shift & goto :parse_args )
if /i "%~1"=="--name"  ( set "NAME=%~2-comp-proxy"  & shift & shift & goto :parse_args )
if /i "%~1"=="--comp"  ( set "COMP_DIR=%~2" & set "CUSTOM_COMP=1" & shift & shift & goto :parse_args )
if /i "%~1"=="--no-deploy" ( set "NO_DEPLOY=1" & shift & goto :parse_args )
echo Unknown argument: %~1
exit /b 1
:args_done

:: Find Visual Studio
set "VCVARS="
for %%p in (
    "%ProgramFiles%\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvarsall.bat"
    "%ProgramFiles%\Microsoft Visual Studio\2022\Professional\VC\Auxiliary\Build\vcvarsall.bat"
    "%ProgramFiles%\Microsoft Visual Studio\2022\Enterprise\VC\Auxiliary\Build\vcvarsall.bat"
    "%ProgramFiles%\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvarsall.bat"
    "%ProgramFiles(x86)%\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvarsall.bat"
    "%ProgramFiles(x86)%\Microsoft Visual Studio\2022\Professional\VC\Auxiliary\Build\vcvarsall.bat"
    "%ProgramFiles(x86)%\Microsoft Visual Studio\2022\Enterprise\VC\Auxiliary\Build\vcvarsall.bat"
    "%ProgramFiles(x86)%\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvarsall.bat"
) do (
    if exist %%p set "VCVARS=%%~p"
)
if "%VCVARS%"=="" (
    echo ERROR: Visual Studio 2022 not found
    exit /b 1
)
call "%VCVARS%" x86 >nul 2>&1
if errorlevel 1 ( echo ERROR: vcvarsall.bat failed & exit /b 1 )

:: Paths
set "SRC=%ROOT%src"
set "DEPS=%ROOT%deps"
set "OUT=%ROOT%build\bin\%CONFIG%"
set "OBJ=%ROOT%build\obj\%CONFIG%"

:: Per-game output: d3d9.dll goes to the game's patches folder when --comp is custom
if "!CUSTOM_COMP!"=="1" (
    for %%G in ("!COMP_DIR!\..\..") do set "GAME_DIR=%%~fG"
    set "GAME_OUT=!GAME_DIR!\build\!CONFIG!"
    set "GAME_OBJ=!GAME_DIR!\build\obj\!CONFIG!"
) else (
    set "GAME_OUT=!OUT!"
    set "GAME_OBJ=!OBJ!\comp"
)

mkdir "%OUT%" 2>nul
mkdir "%OBJ%\minhook" 2>nul
mkdir "%OBJ%\imgui" 2>nul
mkdir "%OBJ%\shared" 2>nul
mkdir "%GAME_OUT%" 2>nul
mkdir "%GAME_OBJ%" 2>nul

:: Include paths
set "INC=/I"%SRC%" /I"%DEPS%\bridge_api" /I"%DEPS%\dxsdk\Include" /I"%DEPS%\imgui" /I"%DEPS%\minhook\include""

:: Lib search path
set "LIBPATH=/LIBPATH:"%DEPS%\dxsdk\Lib\x86""

:: Common compiler flags
set "CF=/nologo /std:c++latest /W4 /MP /EHsc /Zm100"
set "CF=%CF% /wd4239 /wd4369 /wd4505 /wd4996 /wd5311 /wd6001 /wd6385 /wd6386 /wd26812"
set "CF=%CF% /D_SILENCE_ALL_CXX17_DEPRECATION_WARNINGS /D_WINDOWS /DWIN32"

:: Config-specific flags
if /i "%CONFIG%"=="release" (
    set "CF=%CF% /O2 /GL /Gy /Oi /GF /MT /Zi /DNDEBUG /WX"
    set "LF=/LTCG /OPT:REF /OPT:ICF"
) else (
    set "CF=%CF% /Od /MTd /ZI /DDEBUG /D_DEBUG"
    set "LF="
)

echo.
echo === remix-comp-proxy build (d3d9.dll proxy) ===
echo Config:  %CONFIG%
echo Output:  %GAME_OUT%\d3d9.dll
echo CompDir: %COMP_DIR%
echo.

:: -------------------------------------------------------
:: Step 1: minhook (C static lib)
:: -------------------------------------------------------
echo [1/4] minhook
set "MH_SRC=%DEPS%\minhook\src"
cl /nologo /c /W0 /MP /MT /O2 ^
    /I"%DEPS%\minhook\include" ^
    /Fo"%OBJ%\minhook\\" ^
    "%MH_SRC%\buffer.c" ^
    "%MH_SRC%\hook.c" ^
    "%MH_SRC%\trampoline.c" ^
    "%MH_SRC%\hde\hde32.c" ^
    "%MH_SRC%\hde\hde64.c"
if errorlevel 1 goto :fail
lib /nologo /OUT:"%OUT%\minhook.lib" "%OBJ%\minhook\*.obj"
if errorlevel 1 goto :fail

:: -------------------------------------------------------
:: Step 2: imgui (C++ static lib, no PCH)
:: -------------------------------------------------------
echo [2/4] imgui
set "IM=%DEPS%\imgui"
cl /nologo /c /W0 /MP /std:c++latest /MT /O2 /EHsc ^
    /I"%IM%" ^
    /Fo"%OBJ%\imgui\\" ^
    "%IM%\imgui.cpp" ^
    "%IM%\imgui_demo.cpp" ^
    "%IM%\imgui_draw.cpp" ^
    "%IM%\imgui_tables.cpp" ^
    "%IM%\imgui_widgets.cpp" ^
    "%IM%\backends\imgui_impl_dx9.cpp" ^
    "%IM%\backends\imgui_impl_win32.cpp" ^
    "%IM%\misc\cpp\imgui_stdlib.cpp"
if errorlevel 1 goto :fail
lib /nologo /OUT:"%OUT%\imgui.lib" "%OBJ%\imgui\*.obj"
if errorlevel 1 goto :fail

:: -------------------------------------------------------
:: Step 3: _shared (C++ static lib with PCH)
:: -------------------------------------------------------
echo [3/4] _shared

:: Create PCH
cl /nologo /c %CF% %INC% ^
    /Yc"std_include.hpp" /Fp"%OBJ%\shared\shared.pch" ^
    /Fo"%OBJ%\shared\std_include.obj" ^
    "%SRC%\shared\std_include.cpp"
if errorlevel 1 goto :fail

:: Compile sources
cl /nologo /c %CF% %INC% ^
    /Yu"std_include.hpp" /Fp"%OBJ%\shared\shared.pch" ^
    /Fo"%OBJ%\shared\\" ^
    "%SRC%\shared\common\config.cpp" ^
    "%SRC%\shared\common\dinput_hook_v1.cpp" ^
    "%SRC%\shared\common\dinput_hook_v2.cpp" ^
    "%SRC%\shared\common\ffp_prof.cpp" ^
    "%SRC%\shared\common\ffp_state.cpp" ^
    "%SRC%\shared\common\flags.cpp" ^
    "%SRC%\shared\common\imgui_helper.cpp" ^
    "%SRC%\shared\common\loader.cpp" ^
    "%SRC%\shared\common\remix_api.cpp" ^
    "%SRC%\shared\globals.cpp" ^
    "%SRC%\shared\utils\hooking.cpp" ^
    "%SRC%\shared\utils\memory.cpp" ^
    "%SRC%\shared\utils\utils.cpp"
if errorlevel 1 goto :fail
lib /nologo /OUT:"%OUT%\_shared.lib" "%OBJ%\shared\*.obj"
if errorlevel 1 goto :fail

:: -------------------------------------------------------
:: Step 4: comp DLL (C++ with PCH, links everything)
:: -------------------------------------------------------
echo [4/4] %NAME% (d3d9.dll)

:: Create PCH
cl /nologo /c %CF% %INC% /I"%COMP_DIR%\.." ^
    /Yc"std_include.hpp" /Fp"%GAME_OBJ%\comp.pch" ^
    /Fo"%GAME_OBJ%\std_include.obj" ^
    "%COMP_DIR%\std_include.cpp"
if errorlevel 1 goto :fail

:: Compile sources -- glob all .cpp files in comp dir (except std_include.cpp)
set "COMP_SRCS="
for /r "%COMP_DIR%" %%f in (*.cpp) do (
    if /i not "%%~nxf"=="std_include.cpp" (
        set "COMP_SRCS=!COMP_SRCS! "%%f""
    )
)

cl /nologo /c %CF% %INC% /I"%COMP_DIR%\.." ^
    /Yu"std_include.hpp" /Fp"%GAME_OBJ%\comp.pch" ^
    /Fo"%GAME_OBJ%\\" ^
    %COMP_SRCS%
if errorlevel 1 goto :fail

:: Link DLL as d3d9.dll proxy (exports via .def, no d3d9.lib import)
link /nologo /DLL /SUBSYSTEM:WINDOWS /DEBUG /PDBCompress /PDBALTPATH:%%_PDB%% %LF% %LIBPATH% ^
    /DEF:"%ROOT%d3d9.def" ^
    /OUT:"%GAME_OUT%\d3d9.dll" ^
    /PDB:"%GAME_OUT%\%NAME%.pdb" ^
    "%GAME_OBJ%\*.obj" ^
    "%OUT%\_shared.lib" ^
    "%OUT%\imgui.lib" ^
    "%OUT%\minhook.lib" ^
    d3dx9.lib psapi.lib user32.lib gdi32.lib shell32.lib advapi32.lib ole32.lib
if errorlevel 1 goto :fail

:: Copy INI to build output — prefer the root game INI over the assets template
if "!CUSTOM_COMP!"=="1" (
    if exist "!GAME_DIR!\rtx_comp\settings\remix-comp-proxy.ini" (
        copy /y "!GAME_DIR!\rtx_comp\settings\remix-comp-proxy.ini" "%GAME_OUT%\remix-comp-proxy.ini" >nul
    ) else if exist "!GAME_DIR!\remix-comp-proxy.ini" (
        copy /y "!GAME_DIR!\remix-comp-proxy.ini" "%GAME_OUT%\remix-comp-proxy.ini" >nul
    ) else if exist "%ROOT%remix-comp-proxy.ini" (
        copy /y "%ROOT%remix-comp-proxy.ini" "%GAME_OUT%\remix-comp-proxy.ini" >nul
    ) else if exist "%ROOT%assets\remix-comp-proxy.ini" (
        copy /y "%ROOT%assets\remix-comp-proxy.ini" "%GAME_OUT%\remix-comp-proxy.ini" >nul
    )
) else (
    if exist "%ROOT%remix-comp-proxy.ini" (
        copy /y "%ROOT%remix-comp-proxy.ini" "%GAME_OUT%\remix-comp-proxy.ini" >nul
    ) else if exist "%ROOT%assets\remix-comp-proxy.ini" (
        copy /y "%ROOT%assets\remix-comp-proxy.ini" "%GAME_OUT%\remix-comp-proxy.ini" >nul
    )
)

:: Deploy to game directory if configured
set "DEPLOY_TARGET="
if exist "%ROOT%deploy.conf" if "!NO_DEPLOY!"=="0" (
    set /p DEPLOY_TARGET=<"%ROOT%deploy.conf"
)
if not "!DEPLOY_TARGET!"=="" (
    if exist "!DEPLOY_TARGET!" (
        rem A DLL the game has loaded can't be overwritten but can be renamed: the running copy
        rem moves aside and the next launch loads the new one. Moved-aside copies go once free.
        del /q "!DEPLOY_TARGET!\d3d9.dll.old-*" >nul 2>&1
        copy /y "%GAME_OUT%\d3d9.dll" "!DEPLOY_TARGET!\d3d9.dll" >nul 2>&1
        if errorlevel 1 (
            ren "!DEPLOY_TARGET!\d3d9.dll" "d3d9.dll.old-!RANDOM!!RANDOM!" >nul 2>&1
            copy /y "%GAME_OUT%\d3d9.dll" "!DEPLOY_TARGET!\d3d9.dll" >nul
            if errorlevel 1 (
                echo ERROR: could not deploy d3d9.dll to the game folder
                goto :fail
            )
            echo Deployed past the running game: it keeps the old d3d9.dll until it restarts.
        )
        :: PDB travels with the DLL (its embedded name), so crash dumps stay symbolizable.
        copy /y "%GAME_OUT%\%NAME%.pdb" "!DEPLOY_TARGET!\%NAME%.pdb" >nul
        :: Only copy INI on first deploy — never overwrite user's game-folder customizations.
        :: One in the old place counts: the proxy moves it to rtx_comp\settings at startup, and a default
        :: already there would win over it.
        if not exist "!DEPLOY_TARGET!\rtx_comp\settings" mkdir "!DEPLOY_TARGET!\rtx_comp\settings"
        if not exist "!DEPLOY_TARGET!\rtx_comp\settings\remix-comp-proxy.ini" if not exist "!DEPLOY_TARGET!\remix-comp-proxy.ini" (
            if exist "%GAME_OUT%\remix-comp-proxy.ini" (
                copy /y "%GAME_OUT%\remix-comp-proxy.ini" "!DEPLOY_TARGET!\rtx_comp\settings\remix-comp-proxy.ini" >nul
            )
        )
        :: Synthesized map light tables. Only files missing at the destination are
        :: copied: these are hand-tunable in the game folder and a rebuild must not
        :: discard that. Delete one to have the generated table restored.
        if not exist "!DEPLOY_TARGET!\rtx_comp\settings\area_overrides.ini" if not exist "!DEPLOY_TARGET!\rtx_comp\area_overrides.ini" if exist "%ROOT%assets\area_overrides.ini" copy /y "%ROOT%assets\area_overrides.ini" "!DEPLOY_TARGET!\rtx_comp\settings\area_overrides.ini" >nul
        if exist "%ROOT%assets\lights" robocopy "%ROOT%assets\lights" "!DEPLOY_TARGET!\rtx_comp\lights" *.txt /XC /XN /XO >nul 2>&1
        if errorlevel 8 echo WARNING: could not deploy light tables
        :: Per-light edits from the F4 menu (scripts\save_game_settings.py keeps the repo copy current), only when the game has none.
        if not exist "!DEPLOY_TARGET!\rtx_comp\settings\rtx_comp\lights\light_overrides.txt" if exist "%ROOT%assets\light_overrides.txt" (
            if not exist "!DEPLOY_TARGET!\rtx_comp\settings\rtx_comp\lights" mkdir "!DEPLOY_TARGET!\rtx_comp\settings\rtx_comp\lights"
            copy /y "%ROOT%assets\light_overrides.txt" "!DEPLOY_TARGET!\rtx_comp\settings\rtx_comp\lights\light_overrides.txt" >nul
        )
        :: Emission scale per game texture (the runtime's Dark Souls menu > Emissive per texture), only when the game has none.
        if not exist "!DEPLOY_TARGET!\rtx_comp\emissive_scales.txt" if exist "%ROOT%assets\emissive_scales.txt" copy /y "%ROOT%assets\emissive_scales.txt" "!DEPLOY_TARGET!\rtx_comp\emissive_scales.txt" >nul
        ver >nul
        :: The lookdev host's map suns from scripts\gen_map_suns.py, and the per-mesh tables of game/mesh_tables.hpp with
        :: the sky layers among them: generated from the game files by scripts\gen_*.py, never hand-edited, so always refreshed.
        if exist "%ROOT%assets\sky" robocopy "%ROOT%assets\sky" "!DEPLOY_TARGET!\rtx_comp\sky" *.txt >nul 2>&1
        if errorlevel 8 echo WARNING: could not deploy the map suns
        ver >nul
        if exist "%ROOT%assets\mesh_tables" robocopy "%ROOT%assets\mesh_tables" "!DEPLOY_TARGET!\rtx_comp\mesh_tables" *.txt >nul 2>&1
        if errorlevel 8 echo WARNING: could not deploy the mesh tables
        ver >nul
        :: The derived window glass textures (scripts\gen_glass.py) go with the glass table.
        if exist "%ROOT%assets\glass" robocopy "%ROOT%assets\glass" "!DEPLOY_TARGET!\rtx_comp\glass" *.dds >nul 2>&1
        if errorlevel 8 echo WARNING: could not deploy the glass textures
        ver >nul
        :: The FFX effect names and effect lights (scripts\gen_ffx_tables.py), always refreshed.
        if exist "%ROOT%assets\ffx" robocopy "%ROOT%assets\ffx" "!DEPLOY_TARGET!\rtx_comp" *.tsv >nul 2>&1
        if errorlevel 8 echo WARNING: could not deploy the FFX tables
        ver >nul
        echo Deployed to: !DEPLOY_TARGET!
    ) else (
        echo WARNING: deploy.conf target not found: !DEPLOY_TARGET!
    )
)

:: Archive DLL+PDB per build so dumps from any deployed build can be symbolized
:: (each build overwrites the previous output; WER dumps often outlive it).
:: crash_triage.py matches dumps to an archive by the DLL's link timestamp.
set "TS="
for /f %%t in ('powershell -NoProfile -Command "Get-Date -Format yyyyMMdd_HHmmss"') do set "TS=%%t"
if not "!TS!"=="" (
    set "SYMDIR=%ROOT%build\symbols\!TS!"
    mkdir "!SYMDIR!" 2>nul
    copy /y "%GAME_OUT%\d3d9.dll" "!SYMDIR!\d3d9.dll" >nul
    copy /y "%GAME_OUT%\%NAME%.pdb" "!SYMDIR!\%NAME%.pdb" >nul
    :: Keep the 30 newest archives.
    powershell -NoProfile -Command "Get-ChildItem '%ROOT%build\symbols' -Directory | Sort-Object Name -Descending | Select-Object -Skip 30 | Remove-Item -Recurse -Force" >nul 2>&1
)

echo.
echo === Build succeeded: %GAME_OUT%\d3d9.dll ===
exit /b 0

:fail
echo.
echo === Build FAILED ===
exit /b 1
