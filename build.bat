@echo off
rem ===========================================================================
rem  build.bat - standalone build for everything under src\
rem
rem  Self-contained: no dependency on any other script. All object files go to
rem  build\obj and all executables to build\bin.
rem
rem  Targets:
rem    build\bin\vst_stream_record.exe     src\vst_stream_record.c   + common
rem    build\bin\vst_stream_playback.exe   src\vst_stream_playback.c + common
rem    build\bin\vst_gui.exe               src\vst_gui.c
rem
rem  Usage:  build.bat          build
rem          build.bat clean    remove build\obj and build\bin first
rem ===========================================================================
setlocal enabledelayedexpansion

set "ROOT=%~dp0"
if "%ROOT:~-1%"=="\" set "ROOT=%ROOT:~0,-1%"

rem ---------------------------------------------------------------------------
rem Locate a Visual Studio with the x64 C/C++ toolset (VS2015 .. VS2022).
rem   1) vswhere (ships with VS2017 and later) picks the newest install with VC tools
rem   2) otherwise fall back to known layouts, newest first (VS2015 uses vcvarsall amd64)
rem ---------------------------------------------------------------------------
set "VCVARS="
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if exist "%VSWHERE%" (
    for /f "usebackq tokens=*" %%I in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do (
        if exist "%%I\VC\Auxiliary\Build\vcvars64.bat" set "VCVARS=%%I\VC\Auxiliary\Build\vcvars64.bat"
    )
)
if not defined VCVARS if exist "%ProgramFiles%\Microsoft Visual Studio\2022\Enterprise\VC\Auxiliary\Build\vcvars64.bat" set "VCVARS=%ProgramFiles%\Microsoft Visual Studio\2022\Enterprise\VC\Auxiliary\Build\vcvars64.bat"
if not defined VCVARS if exist "%ProgramFiles%\Microsoft Visual Studio\2022\Professional\VC\Auxiliary\Build\vcvars64.bat" set "VCVARS=%ProgramFiles%\Microsoft Visual Studio\2022\Professional\VC\Auxiliary\Build\vcvars64.bat"
if not defined VCVARS if exist "%ProgramFiles%\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" set "VCVARS=%ProgramFiles%\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
if not defined VCVARS if exist "%ProgramFiles%\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" set "VCVARS=%ProgramFiles%\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
if not defined VCVARS if exist "%ProgramFiles(x86)%\Microsoft Visual Studio\2019\Enterprise\VC\Auxiliary\Build\vcvars64.bat" set "VCVARS=%ProgramFiles(x86)%\Microsoft Visual Studio\2019\Enterprise\VC\Auxiliary\Build\vcvars64.bat"
if not defined VCVARS if exist "%ProgramFiles(x86)%\Microsoft Visual Studio\2019\Professional\VC\Auxiliary\Build\vcvars64.bat" set "VCVARS=%ProgramFiles(x86)%\Microsoft Visual Studio\2019\Professional\VC\Auxiliary\Build\vcvars64.bat"
if not defined VCVARS if exist "%ProgramFiles(x86)%\Microsoft Visual Studio\2019\Community\VC\Auxiliary\Build\vcvars64.bat" set "VCVARS=%ProgramFiles(x86)%\Microsoft Visual Studio\2019\Community\VC\Auxiliary\Build\vcvars64.bat"
if not defined VCVARS if exist "%ProgramFiles(x86)%\Microsoft Visual Studio\2019\BuildTools\VC\Auxiliary\Build\vcvars64.bat" set "VCVARS=%ProgramFiles(x86)%\Microsoft Visual Studio\2019\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
if not defined VCVARS if exist "%ProgramFiles(x86)%\Microsoft Visual Studio\2017\Enterprise\VC\Auxiliary\Build\vcvars64.bat" set "VCVARS=%ProgramFiles(x86)%\Microsoft Visual Studio\2017\Enterprise\VC\Auxiliary\Build\vcvars64.bat"
if not defined VCVARS if exist "%ProgramFiles(x86)%\Microsoft Visual Studio\2017\Professional\VC\Auxiliary\Build\vcvars64.bat" set "VCVARS=%ProgramFiles(x86)%\Microsoft Visual Studio\2017\Professional\VC\Auxiliary\Build\vcvars64.bat"
if not defined VCVARS if exist "%ProgramFiles(x86)%\Microsoft Visual Studio\2017\Community\VC\Auxiliary\Build\vcvars64.bat" set "VCVARS=%ProgramFiles(x86)%\Microsoft Visual Studio\2017\Community\VC\Auxiliary\Build\vcvars64.bat"
if not defined VCVARS if exist "%ProgramFiles(x86)%\Microsoft Visual Studio\2017\BuildTools\VC\Auxiliary\Build\vcvars64.bat" set "VCVARS=%ProgramFiles(x86)%\Microsoft Visual Studio\2017\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
set "NIINC=C:\Program Files\IVI Foundation\IVI\Include"
set "VISAINC=C:\Program Files\IVI Foundation\VISA\Win64\Include"
set "NILIB=C:\Program Files\IVI Foundation\IVI\Lib_x64\msc"

set "OBJ=%ROOT%\build\obj"
set "BIN=%ROOT%\build\bin"

if /i "%1"=="clean" (
    echo [build] clean
    if exist "%OBJ%" rmdir /s /q "%OBJ%"
    if exist "%BIN%" rmdir /s /q "%BIN%"
)

rem VS2015: Community / Professional / Enterprise all install into the same "14.0" folder,
rem and it only provides vcvarsall.bat (no vcvars64.bat).
set "VCVARSALL="
if not defined VCVARS if exist "%ProgramFiles(x86)%\Microsoft Visual Studio 14.0\VC\vcvarsall.bat" set "VCVARSALL=%ProgramFiles(x86)%\Microsoft Visual Studio 14.0\VC\vcvarsall.bat"
if not defined VCVARS if exist "%ProgramFiles%\Microsoft Visual Studio 14.0\VC\vcvarsall.bat" set "VCVARSALL=%ProgramFiles%\Microsoft Visual Studio 14.0\VC\vcvarsall.bat"
if defined VCVARS goto :toolset_ok
if defined VCVARSALL goto :toolset_ok
echo [error] no Visual Studio with C++ x64 tools found.
echo         searched VS2022 / VS2019 / VS2017 - Enterprise, Professional, Community, BuildTools
echo         and VS2015 - Microsoft Visual Studio 14.0
exit /b 1
:toolset_ok
if not exist "%NIINC%"   ( echo [error] not found: "%NIINC%" & exit /b 1 )
if not exist "%VISAINC%" ( echo [error] not found: "%VISAINC%" & exit /b 1 )
if not exist "%NILIB%"   ( echo [error] not found: "%NILIB%" & exit /b 1 )

if defined VCVARS (
    echo [build] toolset: "%VCVARS%"
    call "%VCVARS%" >nul
) else (
    echo [build] toolset: "%VCVARSALL%" amd64
    call "%VCVARSALL%" amd64 >nul
)
if errorlevel 1 ( echo [error] Visual Studio environment setup failed & exit /b 1 )
where cl.exe >nul 2>&1 || ( echo [error] cl.exe not on PATH & exit /b 1 )

if not exist "%OBJ%" mkdir "%OBJ%"
if not exist "%BIN%" mkdir "%BIN%"

set "CFLAGS=/nologo /W3 /utf-8 /O2 /wd4819 /D_CRT_SECURE_NO_WARNINGS /I"%NIINC%" /I"%VISAINC%" /I"%ROOT%\src\common""

rem ---------------------------------------------------------------------------
rem 1) common library (all .c under src\common) -> build\obj
rem ---------------------------------------------------------------------------
set "COMMON_OBJS="
set "COMMON_COUNT=0"
for %%F in ("%ROOT%\src\common\*.c") do (
    cl %CFLAGS% /c /Fo"%OBJ%\%%~nF.obj" "%%~fF"
    if errorlevel 1 ( echo [error] compile failed: %%~nxF & exit /b 1 )
    set "COMMON_OBJS=!COMMON_OBJS! "%OBJ%\%%~nF.obj""
    set /a COMMON_COUNT+=1
)
if %COMMON_COUNT%==0 ( echo [error] no sources under src\common & exit /b 1 )
echo [build] common: %COMMON_COUNT% objects

rem ---------------------------------------------------------------------------
rem 2) programs
rem ---------------------------------------------------------------------------
echo [build] vst_stream_record
cl %CFLAGS% /Fo"%OBJ%\vst_stream_record.obj" /Fe"%BIN%\vst_stream_record.exe" ^
   "%ROOT%\src\vst_stream_record.c" %COMMON_OBJS% ^
   /link /LIBPATH:"%NILIB%" niRFSA.lib niRFSG.lib legacy_stdio_definitions.lib
if errorlevel 1 ( echo [error] link failed: vst_stream_record & exit /b 1 )

echo [build] vst_stream_playback
cl %CFLAGS% /Fo"%OBJ%\vst_stream_playback.obj" /Fe"%BIN%\vst_stream_playback.exe" ^
   "%ROOT%\src\vst_stream_playback.c" %COMMON_OBJS% ^
   /link /LIBPATH:"%NILIB%" niRFSA.lib niRFSG.lib legacy_stdio_definitions.lib
if errorlevel 1 ( echo [error] link failed: vst_stream_playback & exit /b 1 )

echo [build] vst_gui
cl %CFLAGS% /Fo"%OBJ%\vst_gui.obj" /Fe"%BIN%\vst_gui.exe" ^
   "%ROOT%\src\vst_gui.c" ^
   /link user32.lib gdi32.lib comctl32.lib setupapi.lib cfgmgr32.lib legacy_stdio_definitions.lib
if errorlevel 1 ( echo [error] link failed: vst_gui & exit /b 1 )

echo.
echo [build] done
echo   %BIN%\vst_stream_record.exe
echo   %BIN%\vst_stream_playback.exe
echo   %BIN%\vst_gui.exe
exit /b 0
